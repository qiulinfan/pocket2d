#include "Link.h"

#include "core/Log.h"
#include "net/ConnectionCode.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <random>
#include <sstream>

namespace Netplay::detail {

std::uint64_t NowMs() { return SDL_GetTicks(); }

std::vector<std::uint8_t> Header(PacketType type) {
    std::vector<std::uint8_t> b(kMagic, kMagic + 4);
    b.push_back(type);
    return b;
}

void PutU32(std::vector<std::uint8_t> &b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

std::uint32_t GetU32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

namespace {

constexpr std::uint32_t kPunchFallbackMs = 3000; // room server: then relay...
constexpr int kPunchFallbackTries = 20;          // ...once that many punches went out (a stalled game sent none)
constexpr std::uint32_t kServerTimeoutMs = 8000;
constexpr std::uint32_t kDirectTimeoutMs = 15000;
constexpr std::uint32_t kCodeTimeoutMs = 20000;
constexpr std::uint32_t kStunWaitMs = 1500;      // for a second STUN answer (NAT type)
constexpr std::uint32_t kStunTimeoutMs = 5000;

// The address peers on the same network reach this socket at: the interface
// that routes to `route`.
Net::Address LanAddress(const Net::Address &route, std::uint16_t port) {
    std::uint32_t ip = 0;
    if (auto local = Net::LocalIPv4ForRoute(route)) ip = *local;
    return Net::Address{ip, port};
}

enum class Mode { Server, DirectHost, DirectJoin, Code };

class UdpLink final : public Link {
public:
    Mode mode = Mode::Server;
    int side = 1;
    Net::UdpSocket socket;
    Net::Address server, peer, peer_lan;
    bool peer_confirmed = false;
    bool force_relay = false;
    std::uint32_t nonce = 0;
    std::uint64_t started = 0, last_server_send = 0, last_punch_send = 0, punch_started = 0;
    int punches = 0;
    // LAN discovery (direct host).
    Net::UdpSocket discovery;
    DiscoveryInfo info;
    // Connection codes.
    struct Stun {
        std::string name;
        Net::Address address;
        Net::StunTransaction transaction{};
        std::optional<Net::Address> mapped;
        std::uint64_t last_send = 0;
    };
    std::vector<Stun> stun;
    std::uint64_t stun_first_answer = 0;

    ~UdpLink() override { socket.Close(); }

    void SendRaw(const Net::Address &to, const std::vector<std::uint8_t> &data) { socket.Send(to, data.data(), data.size()); }

    void SendServerText(const std::string &text) {
        SendRaw(server, std::vector<std::uint8_t>(text.begin(), text.end()));
        last_server_send = NowMs();
    }

    void Send(const std::vector<std::uint8_t> &packet) override {
        if (relay) {
            std::vector<std::uint8_t> wrapped = {'P', 'K', '2', 'R'};
            wrapped.insert(wrapped.end(), packet.begin(), packet.end());
            SendRaw(server, wrapped);
            return;
        }
        if (peer.Valid()) SendRaw(peer, packet);
        if (!peer_confirmed && peer_lan.Valid() && peer_lan != peer) SendRaw(peer_lan, packet);
    }

    void Probe() override {
        auto b = Header(kPunch);
        PutU32(b, static_cast<std::uint32_t>(NowMs()));
        Send(b);
        last_punch_send = NowMs();
        ++punches;
    }

    void Close() override {
        if (mode == Mode::Server && socket.IsOpen() && !room.empty()) SendServerText("PK2 LEAVE " + room);
        discovery.Close();
    }

    void Fail(const std::string &message) {
        error = message;
        state = LinkState::Failed;
        Log::Warning("netplay", message);
    }

    void StartConnecting() {
        state = LinkState::Connecting;
        punch_started = NowMs();
        punches = 0;
    }

    void SetPeer(const Net::Address &address) {
        peer = address;
        peer_text = address.ToString();
    }

    std::string ServerHello() const {
        const std::string lan = LanAddress(server, socket.LocalPort()).ToString();
        if (side == 1) return "PK2 HOST " + std::to_string(nonce) + " " + lan;
        return "PK2 JOIN " + room + " " + std::to_string(nonce) + " " + lan;
    }

    void HandleServerText(const std::string &text) {
        std::istringstream in(text);
        std::string tag, cmd;
        in >> tag >> cmd;
        if (tag != "PK2") return;
        if (cmd == "ROOM" && side == 1) {
            in >> room;
            if (state == LinkState::Opening) state = LinkState::Waiting;
        } else if (cmd == "PEER" && (state == LinkState::Opening || state == LinkState::Waiting)) {
            std::string pub, lan;
            in >> pub >> lan;
            if (auto a = Net::ParseAddress(pub, 0)) SetPeer(*a);
            if (auto a = Net::ParseAddress(lan, 0)) peer_lan = *a;
            StartConnecting();
            Log::Info("netplay", "peer " + pub + " (lan " + lan + "), punching");
        } else if (cmd == "ERR") {
            std::string why;
            std::getline(in, why);
            Fail("room server: " + (why.empty() ? std::string("error") : why.substr(1)));
        }
    }

    void HandlePeer(const std::uint8_t *p, std::size_t n, const Net::Address &from, bool via_relay,
                    std::vector<std::vector<std::uint8_t>> &received) {
        if (n < 5 || std::memcmp(p, kMagic, 4) != 0) return;
        if (!via_relay && peer.Valid() && from != peer && from != peer_lan &&
            (mode == Mode::Server || mode == Mode::DirectJoin || (mode == Mode::Code && peer_confirmed)))
            return;
        if (!via_relay && force_relay && mode == Mode::Server) return;
        last_recv = NowMs();
        if (!via_relay && !relay && !peer_confirmed) {
            peer_confirmed = true;
            SetPeer(from);
            if (state == LinkState::Waiting || state == LinkState::Connecting) state = LinkState::Connected;
            discovery.Close();
        }
        if (via_relay && state == LinkState::Connecting && !peer_confirmed) {
            relay = true; // the peer gave up on punching first
            state = LinkState::Connected;
        }
        const std::uint8_t type = p[4];
        if (type == kPunch) {
            auto b = Header(kPunchAck);
            if (n >= 9) b.insert(b.end(), p + 5, p + 9); // echo the stamp
            Send(b);
        } else if (type == kPunchAck) {
            if (n >= 9) {
                const auto rtt = static_cast<std::int64_t>(static_cast<std::uint32_t>(NowMs()) - GetU32(p + 5));
                if (rtt >= 0 && rtt < 5000) {
                    best_rtt = best_rtt < 0 ? static_cast<int>(rtt) : std::min(best_rtt, static_cast<int>(rtt));
                    ++rtt_samples;
                }
            }
        } else {
            received.emplace_back(p, p + n);
        }
    }

    void HandleDiscovery() {
        Net::Address from;
        while (auto packet = discovery.Receive(from)) {
            const std::string text(packet->begin(), packet->end());
            if (text.rfind("PKD1 ? ", 0) != 0 || text.substr(7) != info.game) continue;
            const std::string reply = "PKD1 ! " + std::to_string(socket.LocalPort()) + "\n" + info.game + "\n" + info.name +
                                      "\n" + info.payload.substr(0, 400);
            discovery.Send(from, reply.data(), reply.size());
        }
    }

    void StunPump(std::uint64_t now) {
        for (auto &s : stun) {
            if (!s.mapped && now - s.last_send > 400) {
                const auto req = Net::StunRequest(s.transaction);
                SendRaw(s.address, req);
                s.last_send = now;
            }
        }
        int answers = 0;
        for (const auto &s : stun) answers += s.mapped ? 1 : 0;
        const bool all = answers == static_cast<int>(stun.size());
        const bool timed_out = now - started > kStunTimeoutMs;
        if (!(all || (answers > 0 && now - stun_first_answer > kStunWaitMs) || timed_out)) return;
        Net::PeerCode own;
        own.lan_address = LanAddress(Net::Address{0x08080808u, 53}, socket.LocalPort());
        lan_text = own.lan_address.ToString();
        for (const auto &s : stun) {
            if (s.mapped) { own.public_address = *s.mapped; break; }
        }
        if (answers >= 2) {
            bool same = true;
            for (const auto &s : stun)
                if (s.mapped && *s.mapped != own.public_address) same = false;
            nat = same ? "cone" : "symmetric";
        }
        if (answers == 0) Log::Warning("netplay", "no STUN server answered; the code only works on the same network");
        code = Net::EncodeCode(own);
        Log::Info("netplay", "connection code " + code + " (public " + own.public_address.ToString() + ", nat " + nat + ")");
        if (side == 1) state = peer.Valid() || peer_lan.Valid() ? LinkState::Connecting : LinkState::Waiting;
        else StartConnecting();
        if (state == LinkState::Connecting) punch_started = now;
    }

    bool ConnectCode(const std::string &text) override {
        const auto decoded = Net::DecodeCode(text);
        if (!decoded) return false;
        if (decoded->public_address.Valid()) SetPeer(decoded->public_address);
        else SetPeer(decoded->lan_address);
        peer_lan = decoded->lan_address;
        if (state == LinkState::Waiting) StartConnecting();
        return true;
    }

    void Pump(std::uint64_t now, std::vector<std::vector<std::uint8_t>> &received) override {
        if (!socket.IsOpen()) return;
        Net::Address from;
        while (auto packet = socket.Receive(from)) {
            const auto &d = *packet;
            if (mode == Mode::Server && from == server) {
                if (d.size() >= 4 && std::memcmp(d.data(), "PK2R", 4) == 0) HandlePeer(d.data() + 4, d.size() - 4, from, true, received);
                else HandleServerText(std::string(d.begin(), d.end()));
            } else if (mode == Mode::Code && Net::IsStunPacket(d.data(), d.size())) {
                for (auto &s : stun) {
                    if (s.mapped) continue;
                    if (auto mapped = Net::ParseStunResponse(d.data(), d.size(), s.transaction)) {
                        s.mapped = mapped;
                        if (!stun_first_answer) stun_first_answer = now;
                    }
                }
            } else {
                HandlePeer(d.data(), d.size(), from, false, received);
            }
            if (state == LinkState::Failed) return;
        }
        if (discovery.IsOpen()) HandleDiscovery();
        switch (state) {
        case LinkState::Opening:
            if (mode == Mode::Code) {
                StunPump(now);
            } else if (now - started > kServerTimeoutMs) {
                Fail("no answer from the room server " + server.ToString());
            } else if (now - last_server_send > 1000) {
                SendServerText(ServerHello());
            }
            break;
        case LinkState::Waiting:
            if (mode == Mode::Server && now - last_server_send > 2000) SendServerText("PK2 KEEP " + room);
            break;
        case LinkState::Connecting:
            if (mode == Mode::Server && !relay &&
                (force_relay || (now - punch_started > kPunchFallbackMs && punches >= kPunchFallbackTries))) {
                relay = true;
                state = LinkState::Connected;
                Log::Info("netplay", "punching failed; relaying through the room server");
                break;
            }
            if (now - last_punch_send > 100) Probe();
            if (mode == Mode::Server && now - last_server_send > 2000) SendServerText("PK2 KEEP " + room);
            if (mode == Mode::DirectJoin && now - punch_started > kDirectTimeoutMs) Fail("no answer from " + peer_text);
            if (mode == Mode::Code && now - punch_started > kCodeTimeoutMs) Fail("could not connect directly to " + peer_text);
            break;
        case LinkState::Connected:
            if (relay && now - last_server_send > 2000) SendServerText("PK2 KEEP " + room);
            break;
        case LinkState::Failed:
            break;
        }
    }
};

std::unique_ptr<UdpLink> NewLink(Mode mode, int side, std::uint16_t port, std::string &error) {
    auto link = std::make_unique<UdpLink>();
    link->mode = mode;
    link->side = side;
    link->started = NowMs();
    link->last_recv = link->started;
    std::random_device rd;
    link->nonce = rd();
    if (!link->socket.Open(port)) {
        error = "cannot open a UDP socket: " + link->socket.LastError();
        return nullptr;
    }
    return link;
}

} // namespace

std::unique_ptr<Link> ServerLink(const std::string &server, int side, const std::string &room, bool force_relay,
                                 std::string &error) {
    const auto addr = Net::ParseAddress(server, kServerPort);
    if (!addr) { error = "cannot resolve the room server " + server; return nullptr; }
    auto link = NewLink(Mode::Server, side, 0, error);
    if (!link) return nullptr;
    link->kind = "server";
    link->server = *addr;
    link->room = room;
    link->force_relay = force_relay;
    link->SendServerText(link->ServerHello());
    return link;
}

std::unique_ptr<Link> DirectHostLink(int port, const DiscoveryInfo &discovery, std::string &error) {
    auto link = NewLink(Mode::DirectHost, 1, static_cast<std::uint16_t>(port > 0 ? port : kDirectPort), error);
    if (!link) return nullptr;
    link->kind = "direct";
    link->state = LinkState::Waiting;
    link->room = "LAN:" + std::to_string(link->socket.LocalPort());
    link->lan_text = LanAddress(Net::Address{0x08080808u, 53}, link->socket.LocalPort()).ToString();
    link->info = discovery;
    if (!discovery.game.empty() && !link->discovery.Open(static_cast<std::uint16_t>(discovery.port), true))
        Log::Warning("netplay", "LAN discovery unavailable: " + link->discovery.LastError());
    return link;
}

std::unique_ptr<Link> DirectJoinLink(const std::string &address, std::string &error) {
    const auto addr = Net::ParseAddress(address, kDirectPort);
    if (!addr) { error = "cannot resolve " + address; return nullptr; }
    auto link = NewLink(Mode::DirectJoin, 2, 0, error);
    if (!link) return nullptr;
    link->kind = "direct";
    link->SetPeer(*addr);
    link->StartConnecting();
    return link;
}

std::unique_ptr<Link> CodeLink(int side, const std::vector<std::string> &stun, const std::string &host_code,
                               std::string &error) {
    std::optional<Net::PeerCode> host;
    if (side == 2) {
        host = Net::DecodeCode(host_code);
        if (!host) { error = "invalid connection code"; return nullptr; }
    }
    auto link = NewLink(Mode::Code, side, 0, error);
    if (!link) return nullptr;
    link->kind = "code";
    if (host) link->ConnectCode(host_code);
    std::mt19937 rng(std::random_device{}());
    for (const auto &name : stun) {
        const auto addr = Net::ParseAddress(name, 3478);
        if (!addr) { Log::Warning("netplay", "cannot resolve the STUN server " + name); continue; }
        UdpLink::Stun s;
        s.name = name;
        s.address = *addr;
        for (auto &b : s.transaction) b = static_cast<std::uint8_t>(rng());
        link->stun.push_back(s);
    }
    return link;
}

} // namespace Netplay::detail
