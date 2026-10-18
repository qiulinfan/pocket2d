#include "net/Netplay.h"

#include "core/Log.h"
#include "input/ActionMap.h"
#include "net/UdpSocket.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <sstream>

// Peer packets start with "PKN1" and a type byte; room-server messages are text
// starting with "PK2 " and relayed peer packets travel inside "PK2R" frames.
namespace Netplay {
namespace {

constexpr std::uint8_t kMagic[4] = {'P', 'K', 'N', '1'};
enum PacketType : std::uint8_t { kPunch = 1, kPunchAck = 2, kStart = 3, kStartAck = 4, kInput = 5, kBye = 6 };
constexpr std::uint16_t kServerPort = 27777;
constexpr std::uint16_t kDirectPort = 27778;
constexpr std::uint32_t kTimeoutMs = 6000;
constexpr std::uint32_t kPunchFallbackMs = 3000;
constexpr std::uint32_t kServerTimeoutMs = 8000;
constexpr int kMaxFramesPerPacket = 32;
constexpr std::uint32_t kMeasureMs = 500;    // the host measures the round trip before START
constexpr int kMeasureSamples = 5;

enum class Mode { None, Server, Direct };

struct ActionField {
    std::string suffix; // name after the side prefix, or the full name for shared actions
    ActionMap::Type type;
};

struct Session {
    Config config;
    State state = State::Idle;
    Mode mode = Mode::None;
    int side = 0;
    std::string room, error, payload, peer_text, lan_text;
    std::uint32_t seed = 0;
    Net::UdpSocket socket;
    Net::Address server, peer, peer_lan;
    bool relay = false;
    bool peer_confirmed = false;     // direct traffic seen from the peer
    std::uint32_t nonce = 0;
    std::uint64_t last_server_send = 0, last_peer_recv = 0, punch_started = 0, last_send = 0, last_start_send = 0,
                  last_punch_send = 0, server_started = 0;
    bool start_acked = false;
    // Round trip measured by the host before START (automatic delay).
    std::uint64_t measure_started = 0;
    int rtt_samples = 0;
    int best_rtt = -1;
    int start_sends = 0;

    // Lockstep.
    std::vector<ActionField> player_fields, shared_fields;
    std::size_t input_size = 0;
    std::uint32_t frame = 0;
    std::map<std::uint32_t, std::vector<std::uint8_t>> local, remote;
    std::uint32_t remote_contiguous = 0; // remote frames [0, n) all received
    std::uint32_t remote_acked = 0;      // our frames [0, n) the peer has
    std::uint32_t stalls = 0;
    std::map<std::uint32_t, std::uint32_t> local_sums, remote_sums;
    bool desync = false;
    std::uint32_t desync_frame = 0;
    // Ping.
    std::uint32_t last_remote_stamp = 0;
    std::uint64_t last_remote_stamp_at = 0;
    int ping_ms = -1;
};

std::unique_ptr<Session> &Holder() {
    static std::unique_ptr<Session> session = std::make_unique<Session>();
    return session;
}
Session &S() { return *Holder(); }
// The configuration as requested; a session's copy fixes an automatic delay.
Config &Requested() {
    static Config config;
    return config;
}

std::uint64_t NowMs() { return SDL_GetTicks(); }

void PutU32(std::vector<std::uint8_t> &b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
std::uint32_t GetU32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::vector<std::uint8_t> Header(PacketType type) {
    std::vector<std::uint8_t> b(kMagic, kMagic + 4);
    b.push_back(type);
    return b;
}

void SendRaw(const Net::Address &to, const std::vector<std::uint8_t> &data) { S().socket.Send(to, data.data(), data.size()); }

void SendServerText(const std::string &text) {
    SendRaw(S().server, std::vector<std::uint8_t>(text.begin(), text.end()));
    S().last_server_send = NowMs();
}

// To the peer: directly, or wrapped for the room server to relay.
void SendPeer(const std::vector<std::uint8_t> &packet) {
    auto &s = S();
    if (s.relay) {
        std::vector<std::uint8_t> wrapped = {'P', 'K', '2', 'R'};
        wrapped.insert(wrapped.end(), packet.begin(), packet.end());
        SendRaw(s.server, wrapped);
    } else {
        SendRaw(s.peer, packet);
        if (!s.peer_confirmed && s.peer_lan.Valid() && s.peer_lan != s.peer) SendRaw(s.peer_lan, packet);
    }
    s.last_send = NowMs();
}

void Fail(const std::string &message) {
    auto &s = S();
    s.error = message;
    s.state = State::Error;
    ActionMap::SetNetValues(nullptr);
    Log::Warning("netplay", message);
}


bool OpenSocket(std::uint16_t port) {
    auto &s = S();
    if (!s.socket.Open(port)) {
        Fail("cannot open a UDP socket: " + s.socket.LastError());
        return false;
    }
    return true;
}

void ResetSession() {
    auto &s = S();
    s.socket.Close();
    ActionMap::SetNetValues(nullptr);
    Holder() = std::make_unique<Session>();
    S().config = Requested();
    std::random_device rd;
    S().nonce = rd();
}

// Fields of the player input vector, from the actions declared with the local prefix.
void BuildFields() {
    auto &s = S();
    s.player_fields.clear();
    s.shared_fields.clear();
    for (const auto &d : ActionMap::Declarations()) {
        if (d.name.rfind(s.config.local_prefix, 0) == 0)
            s.player_fields.push_back({d.name.substr(s.config.local_prefix.size()), d.type});
    }
    for (const auto &name : s.config.shared) {
        if (auto type = ActionMap::Find(name)) s.shared_fields.push_back({name, *type});
    }
    auto bytes = [](ActionMap::Type t) { return t == ActionMap::Type::Axis2D ? 2u : 1u; };
    s.input_size = 0;
    for (const auto &f : s.player_fields) s.input_size += bytes(f.type);
    for (const auto &f : s.shared_fields) s.input_size += bytes(f.type);
}

std::int8_t Quantize(float v) { return static_cast<std::int8_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 127.0f)); }
float Dequantize(std::uint8_t b) { return static_cast<float>(static_cast<std::int8_t>(b)) / 127.0f; }

std::vector<std::uint8_t> SampleInput() {
    auto &s = S();
    std::vector<std::uint8_t> out;
    out.reserve(s.input_size);
    auto put = [&](const std::string &name, ActionMap::Type type) {
        const ActionMap::Value v = ActionMap::SampleLocal(name);
        out.push_back(static_cast<std::uint8_t>(Quantize(v.x)));
        if (type == ActionMap::Type::Axis2D) out.push_back(static_cast<std::uint8_t>(Quantize(v.y)));
    };
    for (const auto &f : s.player_fields) put(s.config.local_prefix + f.suffix, f.type);
    for (const auto &f : s.shared_fields) put(f.suffix, f.type);
    return out;
}

void StartLockstep() {
    auto &s = S();
    BuildFields();
    s.frame = 0;
    s.local.clear();
    s.remote.clear();
    const std::vector<std::uint8_t> neutral(s.input_size, 0);
    for (int f = 0; f < s.config.delay; ++f) {
        s.local[static_cast<std::uint32_t>(f)] = neutral;
        s.remote[static_cast<std::uint32_t>(f)] = neutral;
    }
    s.remote_contiguous = static_cast<std::uint32_t>(s.config.delay);
    s.remote_acked = static_cast<std::uint32_t>(s.config.delay);
    s.state = State::Running;
    s.last_peer_recv = NowMs();
    Log::Info("netplay", "lockstep started as side " + std::to_string(s.side) + (s.relay ? " (relayed)" : " (direct)") +
                         ", delay " + std::to_string(s.config.delay) + ", input " + std::to_string(s.input_size) + " bytes");
}

// Automatic delay: the ticks a one-way trip takes, plus one for jitter and resends.
int DelayForRtt(int rtt_ms) {
    if (rtt_ms < 0) return 4;
    const int ticks = static_cast<int>(std::ceil((rtt_ms / 2.0) / (1000.0 / 60.0)));
    return std::clamp(ticks + 1, 2, 10);
}

void SendPunch() {
    auto &s = S();
    auto b = Header(kPunch);
    PutU32(b, static_cast<std::uint32_t>(NowMs()));
    SendPeer(b);
    s.last_punch_send = NowMs();
}

void SendStart() {
    auto &s = S();
    if (s.config.delay <= 0) {
        s.config.delay = DelayForRtt(s.best_rtt);
        Log::Info("netplay", "round trip " + std::to_string(s.best_rtt) + " ms: input delay " + std::to_string(s.config.delay));
    }
    auto b = Header(kStart);
    PutU32(b, s.seed);
    b.push_back(static_cast<std::uint8_t>(s.config.delay));
    const std::size_t n = std::min<std::size_t>(s.payload.size(), 1000);
    b.push_back(static_cast<std::uint8_t>(n & 255));
    b.push_back(static_cast<std::uint8_t>(n >> 8));
    b.insert(b.end(), s.payload.begin(), s.payload.begin() + static_cast<std::ptrdiff_t>(n));
    SendPeer(b);
    s.last_start_send = NowMs();
}

void SendInput() {
    auto &s = S();
    auto b = Header(kInput);
    PutU32(b, s.remote_contiguous);
    PutU32(b, static_cast<std::uint32_t>(NowMs()));
    // Echo the peer's last stamp with the time it waited here.
    PutU32(b, s.last_remote_stamp);
    PutU32(b, s.last_remote_stamp_at ? static_cast<std::uint32_t>(NowMs() - s.last_remote_stamp_at) : 0);
    std::uint32_t first = s.remote_acked;
    std::uint32_t last = s.local.empty() ? first : s.local.rbegin()->first + 1;
    if (last > first + kMaxFramesPerPacket) last = first + kMaxFramesPerPacket;
    if (last < first) last = first;
    PutU32(b, first);
    b.push_back(static_cast<std::uint8_t>(last - first));
    b.push_back(static_cast<std::uint8_t>(s.input_size));
    for (std::uint32_t f = first; f < last; ++f) {
        const auto found = s.local.find(f);
        if (found == s.local.end()) { b.resize(b.size() + s.input_size, 0); continue; }
        b.insert(b.end(), found->second.begin(), found->second.end());
    }
    // The latest few checksums.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> sums;
    for (auto it = s.local_sums.rbegin(); it != s.local_sums.rend() && sums.size() < 4; ++it) sums.emplace_back(it->first, it->second);
    b.push_back(static_cast<std::uint8_t>(sums.size()));
    for (const auto &[f, v] : sums) { PutU32(b, f); PutU32(b, v); }
    SendPeer(b);
}

void CompareSums() {
    auto &s = S();
    for (auto it = s.remote_sums.begin(); it != s.remote_sums.end();) {
        const auto mine = s.local_sums.find(it->first);
        if (mine == s.local_sums.end()) { ++it; continue; }
        if (mine->second != it->second && !s.desync) {
            s.desync = true;
            s.desync_frame = it->first;
            Log::Warning("netplay", "desync detected at frame " + std::to_string(it->first));
        }
        it = s.remote_sums.erase(it);
    }
    while (s.local_sums.size() > 64) s.local_sums.erase(s.local_sums.begin());
}

void HandlePeerPacket(const std::uint8_t *p, std::size_t n, const Net::Address &from, bool via_relay) {
    auto &s = S();
    if (n < 5 || std::memcmp(p, kMagic, 4) != 0) return;
    if (!via_relay && s.peer.Valid() && from != s.peer && from != s.peer_lan && s.mode == Mode::Server) return;
    const std::uint8_t type = p[4];
    s.last_peer_recv = NowMs();
    if (!via_relay && s.config.force_relay && s.mode == Mode::Server) return;
    if (!via_relay && !s.relay && !s.peer_confirmed) {
        s.peer_confirmed = true;
        s.peer = from;
        s.peer_text = from.ToString();
    }
    if (s.mode == Mode::Direct && !s.peer.Valid()) { s.peer = from; s.peer_text = from.ToString(); }
    switch (type) {
    case kPunch: {
        auto b = Header(kPunchAck);
        if (n >= 9) b.insert(b.end(), p + 5, p + 9); // echo the stamp
        SendPeer(b);
        if (s.mode == Mode::Direct && s.side == 1 && s.state == State::Waiting) s.state = State::Connecting;
        break;
    }
    case kPunchAck:
        if (n >= 9 && s.side == 1 && s.state == State::Connecting) {
            const auto rtt = static_cast<std::int64_t>(static_cast<std::uint32_t>(NowMs()) - GetU32(p + 5));
            if (rtt >= 0 && rtt < 5000) {
                s.best_rtt = s.best_rtt < 0 ? static_cast<int>(rtt) : std::min(s.best_rtt, static_cast<int>(rtt));
                ++s.rtt_samples;
            }
        }
        break;
    case kStart:
        if (s.side == 2 && n >= 12) {
            if (s.state != State::Running) {
                s.seed = GetU32(p + 5);
                s.config.delay = std::clamp<int>(p[9], 1, 15);
                const std::size_t len = static_cast<std::size_t>(p[10]) | (static_cast<std::size_t>(p[11]) << 8);
                if (12 + len <= n) s.payload.assign(reinterpret_cast<const char *>(p + 12), len);
                StartLockstep();
            }
            SendPeer(Header(kStartAck));
        }
        break;
    case kStartAck:
        if (s.side == 1 && !s.start_acked) {
            s.start_acked = true;
            if (s.state != State::Running) StartLockstep();
        }
        break;
    case kInput: {
        if (s.state != State::Running || n < 5 + 4 * 5 + 2) break;
        if (s.side == 1 && !s.start_acked) { s.start_acked = true; }
        const std::uint8_t *q = p + 5;
        const std::uint32_t ack = GetU32(q); q += 4;
        const std::uint32_t stamp = GetU32(q); q += 4;
        const std::uint32_t echo = GetU32(q); q += 4;
        const std::uint32_t held = GetU32(q); q += 4;
        const std::uint32_t first = GetU32(q); q += 4;
        const std::uint8_t count = *q++;
        const std::uint8_t size = *q++;
        if (size != s.input_size) { Fail("the peer runs a different game version (input size differs)"); return; }
        if (q + static_cast<std::size_t>(count) * size > p + n) break;
        s.remote_acked = std::max(s.remote_acked, ack);
        for (auto it = s.local.begin(); it != s.local.end() && it->first + 64 < s.remote_acked;) it = s.local.erase(it);
        s.last_remote_stamp = stamp;
        s.last_remote_stamp_at = NowMs();
        if (echo != 0) {
            const std::int64_t rtt = static_cast<std::int64_t>(static_cast<std::uint32_t>(NowMs()) - echo) - held;
            if (rtt >= 0 && rtt < 5000) s.ping_ms = s.ping_ms < 0 ? static_cast<int>(rtt) : (s.ping_ms * 7 + static_cast<int>(rtt)) / 8;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t f = first + i;
            if (f >= s.remote_contiguous && !s.remote.count(f)) s.remote[f] = std::vector<std::uint8_t>(q, q + size);
            q += size;
        }
        while (s.remote.count(s.remote_contiguous)) ++s.remote_contiguous;
        if (q < p + n) {
            const std::uint8_t sums = *q++;
            for (std::uint8_t i = 0; i < sums && q + 8 <= p + n; ++i, q += 8) s.remote_sums[GetU32(q)] = GetU32(q + 4);
            CompareSums();
        }
        break;
    }
    case kBye:
        if (s.state == State::Running || s.state == State::Connecting) {
            s.state = State::Disconnected;
            s.error = "the other player left";
            ActionMap::SetNetValues(nullptr);
        }
        break;
    default:
        break;
    }
}

void HandleServerText(const std::string &text) {
    auto &s = S();
    std::istringstream in(text);
    std::string tag, cmd;
    in >> tag >> cmd;
    if (tag != "PK2") return;
    if (cmd == "ROOM" && s.side == 1) {
        in >> s.room;
        if (s.state == State::Server) s.state = State::Waiting;
    } else if (cmd == "PEER" && (s.state == State::Server || s.state == State::Waiting)) {
        std::string pub, lan;
        in >> pub >> lan;
        if (auto a = Net::ParseAddress(pub, 0)) s.peer = *a;
        if (auto a = Net::ParseAddress(lan, 0)) s.peer_lan = *a;
        s.peer_text = s.peer.ToString();
        s.state = State::Connecting;
        s.punch_started = NowMs();
        Log::Info("netplay", "peer " + pub + " (lan " + lan + "), punching");
    } else if (cmd == "ERR") {
        std::string why;
        std::getline(in, why);
        Fail("room server: " + (why.empty() ? std::string("error") : why.substr(1)));
    }
}

// The address peers on the same network reach us at: the interface that routes to `route`.
std::string LanAddressText(const Net::Address &route) {
    auto &s = S();
    std::uint32_t ip = 0;
#if !defined(__EMSCRIPTEN__)
    if (auto local = Net::LocalIPv4ForRoute(route)) ip = *local;
#endif
    Net::Address a{ip, s.socket.LocalPort()};
    return a.ToString();
}

} // namespace

const char *StateName(State state) {
    switch (state) {
    case State::Idle: return "idle";
    case State::Server: return "server";
    case State::Waiting: return "waiting";
    case State::Connecting: return "connecting";
    case State::Running: return "running";
    case State::Disconnected: return "disconnected";
    case State::Error: return "error";
    }
    return "idle";
}

void Configure(const Config &config) {
    auto &c = Requested();
    c = config;
    c.delay = config.delay <= 0 ? 0 : std::clamp(config.delay, 1, 15);
    if (c.sides.size() != 2) c.sides = {"p1_", "p2_"};
    S().config = c;
}

const Config &GetConfig() { return Requested(); }

bool Host(const std::string &server, const std::string &payload) {
    ResetSession();
    auto &s = S();
    const auto addr = Net::ParseAddress(server, kServerPort);
    if (!addr) { Fail("cannot resolve the room server " + server); return false; }
    if (!OpenSocket(0)) return false;
    s.mode = Mode::Server;
    s.server = *addr;
    s.side = 1;
    s.payload = payload;
    std::random_device rd;
    s.seed = rd();
    s.state = State::Server;
    s.server_started = NowMs();
    SendServerText("PK2 HOST " + std::to_string(s.nonce) + " " + LanAddressText(s.server));
    return true;
}

bool Join(const std::string &server, const std::string &room) {
    ResetSession();
    auto &s = S();
    const auto addr = Net::ParseAddress(server, kServerPort);
    if (!addr) { Fail("cannot resolve the room server " + server); return false; }
    if (!OpenSocket(0)) return false;
    s.mode = Mode::Server;
    s.server = *addr;
    s.side = 2;
    s.room = room;
    s.state = State::Server;
    s.server_started = NowMs();
    SendServerText("PK2 JOIN " + room + " " + std::to_string(s.nonce) + " " + LanAddressText(s.server));
    return true;
}

bool HostDirect(int port, const std::string &payload) {
    ResetSession();
    auto &s = S();
    if (!OpenSocket(static_cast<std::uint16_t>(port > 0 ? port : kDirectPort))) return false;
    s.mode = Mode::Direct;
    s.side = 1;
    s.payload = payload;
    std::random_device rd;
    s.seed = rd();
    s.state = State::Waiting;
    s.room = "LAN:" + std::to_string(s.socket.LocalPort());
    s.lan_text = LanAddressText(Net::Address{0x08080808u, 53}); // a route lookup; nothing is sent
    return true;
}

bool JoinDirect(const std::string &address) {
    ResetSession();
    auto &s = S();
    const auto addr = Net::ParseAddress(address, kDirectPort);
    if (!addr) { Fail("cannot resolve " + address); return false; }
    if (!OpenSocket(0)) return false;
    s.mode = Mode::Direct;
    s.side = 2;
    s.peer = *addr;
    s.peer_text = addr->ToString();
    s.state = State::Connecting;
    s.punch_started = NowMs();
    return true;
}

void Leave() {
    auto &s = S();
    if (s.socket.IsOpen() && (s.peer.Valid() || s.relay)) {
        const auto bye = Header(kBye);
        for (int i = 0; i < 3; ++i) SendPeer(bye);
    }
    if (s.socket.IsOpen() && s.mode == Mode::Server && !s.room.empty()) SendServerText("PK2 LEAVE " + s.room);
    ResetSession();
}

void Pump() {
    auto &s = S();
    if (!s.socket.IsOpen()) return;
    Net::Address from;
    while (auto packet = s.socket.Receive(from)) {
        const auto &d = *packet;
        if (s.mode == Mode::Server && from == s.server) {
            if (d.size() >= 4 && std::memcmp(d.data(), "PK2R", 4) == 0) {
                HandlePeerPacket(d.data() + 4, d.size() - 4, from, true);
                if (!s.peer_confirmed && s.state == State::Connecting && !s.relay) {
                    // The peer gave up on punching first.
                    s.relay = true;
                }
            } else {
                HandleServerText(std::string(d.begin(), d.end()));
            }
        } else {
            HandlePeerPacket(d.data(), d.size(), from, false);
        }
        if (!s.socket.IsOpen()) return;
    }
    const std::uint64_t now = NowMs();
    switch (s.state) {
    case State::Server:
        if (now - s.server_started > kServerTimeoutMs) {
            Fail("no answer from the room server " + s.server.ToString());
            break;
        }
        if (now - s.last_server_send > 1000) {
            if (s.side == 1) SendServerText("PK2 HOST " + std::to_string(s.nonce) + " " + LanAddressText(s.server));
            else SendServerText("PK2 JOIN " + s.room + " " + std::to_string(s.nonce) + " " + LanAddressText(s.server));
        }
        break;
    case State::Waiting:
        if (s.mode == Mode::Server && now - s.last_server_send > 2000) SendServerText("PK2 KEEP " + s.room);
        break;
    case State::Connecting:
        if (!s.peer_confirmed && !s.relay && s.mode == Mode::Server &&
            (s.config.force_relay || now - s.punch_started > kPunchFallbackMs)) {
            s.relay = true;
            Log::Info("netplay", "punching failed; relaying through the room server");
        }
        // Separate timers: replies to the peer's punches must not starve START.
        if (s.side == 1 && (s.peer_confirmed || s.relay)) {
            // With an automatic delay, first time a few round trips.
            if (!s.measure_started) s.measure_started = now;
            const bool measuring = s.config.delay <= 0 && s.start_sends == 0 && now - s.measure_started < kMeasureMs &&
                                   s.rtt_samples < kMeasureSamples;
            if (measuring) {
                if (now - s.last_punch_send > 50) SendPunch();
            } else if (now - s.last_start_send > 100) {
                SendStart();
                ++s.start_sends;
            }
        } else if (now - s.last_punch_send > 100) {
            SendPunch();
        }
        if (s.mode == Mode::Server && now - s.last_server_send > 2000) SendServerText("PK2 KEEP " + s.room);
        if (s.mode == Mode::Direct && s.side == 2 && now - s.punch_started > 15000) Fail("no answer from " + s.peer_text);
        break;
    case State::Running:
        if (s.side == 1 && !s.start_acked && now - s.last_start_send > 100) SendStart();
        if (now - s.last_send > 30) SendInput();
        if (s.relay && now - s.last_server_send > 2000) SendServerText("PK2 KEEP " + s.room);
        if (now - s.last_peer_recv > kTimeoutMs) {
            s.state = State::Disconnected;
            s.error = "connection lost";
            ActionMap::SetNetValues(nullptr);
            Log::Warning("netplay", "connection lost");
        }
        break;
    default:
        break;
    }
}

bool Running() { return S().state == State::Running; }

bool ReadyForTick() {
    auto &s = S();
    return s.state == State::Running && s.remote.count(s.frame) && s.local.count(s.frame);
}

void BeginTick() {
    auto &s = S();
    std::map<std::string, ActionMap::Value> values;
    const std::vector<std::uint8_t> *inputs[2] = {nullptr, nullptr};
    inputs[s.side - 1] = &s.local[s.frame];
    inputs[2 - s.side] = &s.remote[s.frame];
    for (int side = 0; side < 2; ++side) {
        const auto &bytes = *inputs[side];
        std::size_t i = 0;
        for (const auto &f : s.player_fields) {
            ActionMap::Value v;
            v.x = Dequantize(bytes[i++]);
            if (f.type == ActionMap::Type::Axis2D) v.y = Dequantize(bytes[i++]);
            values[s.config.sides[static_cast<std::size_t>(side)] + f.suffix] = v;
        }
        for (const auto &f : s.shared_fields) {
            ActionMap::Value v;
            v.x = Dequantize(bytes[i++]);
            if (f.type == ActionMap::Type::Axis2D) v.y = Dequantize(bytes[i++]);
            auto &slot = values[f.suffix];
            if (f.type == ActionMap::Type::Button) slot.x = std::max(slot.x, v.x);
            else { slot.x += v.x; slot.y += v.y; }
        }
    }
    ActionMap::SetNetValues(&values);
}

void EndTick() {
    auto &s = S();
    if (s.state != State::Running) return;
    s.local[s.frame + static_cast<std::uint32_t>(s.config.delay)] = SampleInput();
    s.remote.erase(s.frame);
    ++s.frame;
    SendInput();
}

void NoteStall() { ++S().stalls; }

void ReportChecksum(std::uint32_t frame, std::uint32_t value) {
    auto &s = S();
    if (s.state != State::Running) return;
    s.local_sums[frame] = value;
    CompareSums();
}

Status GetStatus() {
    auto &s = S();
    Status st;
    st.state = s.state;
    st.side = s.side;
    st.room = s.room;
    st.error = s.error;
    st.payload = s.payload;
    st.seed = s.seed;
    st.ping_ms = s.ping_ms;
    st.delay = s.config.delay;
    st.relay = s.relay;
    st.frame = s.frame;
    st.stalls = s.stalls;
    st.desync = s.desync;
    st.desync_frame = s.desync_frame;
    st.peer = s.peer_text;
    st.lan = s.lan_text;
    return st;
}

} // namespace Netplay
