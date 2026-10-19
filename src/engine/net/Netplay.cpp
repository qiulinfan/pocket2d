#include "net/Netplay.h"

#include "Link.h"
#include "core/Log.h"
#include "core/World.h"
#include "input/ActionMap.h"
#include "net/UdpSocket.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <random>

// The session: the START handshake (seed, sync mode, delay, payload), input
// exchange and the lockstep, rollback and sync test tick rules. Links
// (Link.cpp) bring the peers together.
namespace Netplay {
namespace {

using namespace detail;

constexpr std::uint32_t kTimeoutMs = 6000;
constexpr int kMaxFramesPerPacket = 32;
constexpr std::uint32_t kMeasureMs = 500;    // the host measures the round trip before START
constexpr int kMeasureSamples = 5;
constexpr int kRelaySamples = 3;               // ...and the relay's, when a room server could relay
constexpr std::uint32_t kProbeEveryMs = 250;   // while playing: probes for the loss
constexpr std::uint32_t kEngineSum = 0x80000000u; // checksum keys of engine state hashes
constexpr std::uint32_t kHashEvery = 60;
constexpr std::uint32_t kNone = UINT32_MAX;
constexpr std::size_t kChunk = 1000;           // desync recovery: state bytes per packet
constexpr std::uint32_t kMaxChunks = 8192;
constexpr int kChunksPerPump = 16;
constexpr std::uint32_t kInputHistory = 300;   // ticks of inputs kept for a resync's resimulation
constexpr int kMaxResyncs = 5;                 // per session; a desync that keeps coming back stays shown
constexpr std::uint32_t kResyncTimeoutMs = 20000;

using Bytes = std::vector<std::uint8_t>;
using Values = std::map<std::string, ActionMap::Value>;

struct ActionField {
    std::string suffix; // name after the side prefix, or the full name for shared actions
    ActionMap::Type type;
};

struct Session {
    Config config;
    State state = State::Idle;
    int side = 0;
    std::unique_ptr<Link> link;
    bool synctest = false;
    std::string error, payload;
    std::uint32_t seed = 0;
    // Handshake.
    bool start_acked = false;
    int start_sends = 0;
    std::uint64_t measure_started = 0, last_start_send = 0, last_send = 0, last_peer_recv = 0, last_probe = 0;

    // Inputs.
    std::vector<ActionField> player_fields, shared_fields;
    std::size_t input_size = 0;
    std::uint32_t frame = 0;      // the next tick to simulate
    std::uint32_t sim_frame = 0;  // the tick being simulated (also while resimulating)
    std::map<std::uint32_t, Bytes> local, remote;
    std::uint32_t remote_contiguous = 0; // remote frames [0, n) all received
    std::uint32_t remote_acked = 0;      // our frames [0, n) the peer has
    std::uint32_t stalls = 0, waits = 0;
    std::map<std::uint32_t, std::uint32_t> local_sums, remote_sums;
    bool desync = false;
    std::uint32_t desync_frame = 0;
    std::uint32_t last_remote_stamp = 0;
    std::uint64_t last_remote_stamp_at = 0;
    int ping_ms = -1;

    // Rollback.
    std::map<std::uint32_t, Bytes> used_remote; // the remote input each simulated tick used
    std::uint32_t rollback_to = kNone;
    std::uint32_t hashed_upto = 0;
    std::vector<int> slot_world_frame;
    std::uint32_t rollbacks = 0, resimulated = 0;
    int max_rollback_seen = 0;
    double advantage = 0.0;      // ticks ahead of the peer (smoothed)
    double peer_advantage = 0.0; // the peer's own estimate
    int ticks_since_wait = 0;
    bool paced = false;          // the caller slows its clock by TickRate
    double slowest_rollback_ms = 0.0;
    bool resimulating = false;

    // Desync recovery: one transfer at a time (part 0 the state, part 1 code
    // the guest lacked), numbered by the host's resync number.
    struct Transfer {
        std::uint32_t id = 0;
        std::uint8_t part = 0;
        std::uint32_t frame = 0;
        std::string data;
        std::vector<std::uint8_t> got;   // per chunk: received (guest), acknowledged (host)
        std::vector<std::uint64_t> sent; // host: when each chunk last went out
        std::uint32_t received = 0;
        bool complete = false;
    };
    Transfer transfer;
    bool resync_active = false;      // host: sending; guest: receiving or waiting to apply
    bool resync_wanted = false;      // host: the guest asked
    std::uint32_t resync_id = 0;     // the latest resync (both sides)
    std::uint32_t resync_done = 0;   // guest: the latest applied
    std::uint32_t resync_loaded_at = 0; // guest: the tick it was applied at
    std::uint32_t resync_target = 0; // guest: the tick the state is of
    std::string resync_state;        // guest: kept while the code it lacks comes
    std::vector<std::uint64_t> resync_missing;
    Code resync_code;
    bool resync_ready = false;       // guest: applied when its tick comes
    std::uint32_t resync_floor = 0;  // checksums of earlier ticks predate the last resync
    std::uint32_t resyncs = 0, resync_frame = 0, resync_bytes = 0;
    int resync_attempts = 0, done_sends = 0;
    std::uint64_t resync_started = 0, resync_asked = 0, last_ack = 0;
    bool ack_dirty = false;
    std::string resync_error;

    // Sync test.
    int synctest_frames = 0;
    std::map<std::uint32_t, Values> synctest_inputs;
    std::map<std::uint32_t, std::uint32_t> synctest_hash;
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

StateHooks &Hooks() {
    static StateHooks hooks;
    return hooks;
}

TickObserver &Observer() {
    static TickObserver observer;
    return observer;
}

bool Rollback() { return S().config.mode == SyncMode::Rollback; }
// Rollback time sync: how many ticks this side runs ahead of the peer, by both sides' estimates.
double Excess() { return (S().advantage - S().peer_advantage) / 2.0; }
int Slots() { return S().synctest ? S().synctest_frames + 2 : S().config.max_rollback + 2; }
int Slot(std::uint32_t frame) { return static_cast<int>(frame % static_cast<std::uint32_t>(Slots())); }

void Fail(const std::string &message) {
    auto &s = S();
    s.error = message;
    s.state = State::Error;
    ActionMap::SetNetValues(nullptr, nullptr);
    Log::Warning("netplay", message);
}

void ResetSession() {
    auto &s = S();
    if (s.link) s.link.reset();
    ActionMap::SetNetValues(nullptr, nullptr);
    Holder() = std::make_unique<Session>();
    S().config = Requested();
}

std::uint32_t NewSeed() {
    std::random_device rd;
    return rd();
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

Bytes SampleInput() {
    auto &s = S();
    Bytes out;
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

// The remote input a tick uses: the real one, or the prediction (the last one received).
const Bytes &RemoteFor(std::uint32_t frame) {
    static const Bytes empty;
    auto &s = S();
    const auto found = s.remote.find(frame);
    if (found != s.remote.end()) return found->second;
    if (s.remote_contiguous > 0) {
        const auto last = s.remote.find(s.remote_contiguous - 1);
        if (last != s.remote.end()) return last->second;
    }
    return empty;
}

const Bytes &LocalFor(std::uint32_t frame) {
    static const Bytes empty;
    const auto found = S().local.find(frame);
    return found == S().local.end() ? empty : found->second;
}

Values Decode(const Bytes &mine, const Bytes &theirs) {
    auto &s = S();
    Values values;
    const Bytes *inputs[2] = {nullptr, nullptr};
    inputs[s.side - 1] = &mine;
    inputs[2 - s.side] = &theirs;
    for (int side = 0; side < 2; ++side) {
        const Bytes &bytes = *inputs[side];
        std::size_t i = 0;
        auto next = [&]() { return i < bytes.size() ? Dequantize(bytes[i++]) : (++i, 0.0f); };
        for (const auto &f : s.player_fields) {
            ActionMap::Value v;
            v.x = next();
            if (f.type == ActionMap::Type::Axis2D) v.y = next();
            values[s.config.sides[static_cast<std::size_t>(side)] + f.suffix] = v;
        }
        for (const auto &f : s.shared_fields) {
            ActionMap::Value v;
            v.x = next();
            if (f.type == ActionMap::Type::Axis2D) v.y = next();
            auto &slot = values[f.suffix];
            if (f.type == ActionMap::Type::Button) slot.x = std::max(slot.x, v.x);
            else { slot.x += v.x; slot.y += v.y; }
        }
    }
    return values;
}

// Sets the actions for a tick from the inputs of it and of the tick before (edges).
void ApplyInputs(std::uint32_t frame) {
    const Values now = Decode(LocalFor(frame), RemoteFor(frame));
    const Values before = frame > 0 ? Decode(LocalFor(frame - 1), RemoteFor(frame - 1)) : Values{};
    ActionMap::SetNetValues(&now, &before);
}

void SaveSlot(std::uint32_t frame) {
    auto &s = S();
    const int slot = Slot(frame);
    if (s.slot_world_frame.size() != static_cast<std::size_t>(Slots())) s.slot_world_frame.assign(Slots(), 0);
    s.slot_world_frame[slot] = World::Current().frame_number;
    Hooks().save(slot);
}

void LoadSlot(std::uint32_t frame) {
    auto &s = S();
    const int slot = Slot(frame);
    Hooks().load(slot);
    World::Current().frame_number = s.slot_world_frame[slot];
}

// Automatic delay from the measured round trip.
int DelayForRtt(int rtt_ms, SyncMode mode) {
    if (rtt_ms < 0) return mode == SyncMode::Rollback ? 2 : 4;
    const int ticks = static_cast<int>(std::ceil((rtt_ms / 2.0) / (1000.0 / 60.0)));
    // Lockstep hides the whole trip; rollback hides part of it and predicts the rest.
    // Snapshots are cheap enough that two ticks of delay suit any trip.
    if (mode == SyncMode::Rollback) return std::clamp((ticks + 1) / 2, 1, 2);
    return std::clamp(ticks + 1, 2, 10);
}

// Room server: the relay instead of the direct path when it is clearly faster
// (a fifth and 10 ms), decided by the host once before START.
void ChoosePath() {
    auto &s = S();
    Link &l = *s.link;
    if (s.start_sends == 0 && l.CanRelay() && !l.relay && l.direct_rtt >= 0 && l.relay_rtt >= 0 &&
        l.relay_rtt + 10 < l.direct_rtt * 4 / 5)
        l.UseRelay();
    l.probe_relay = false;
}

void SendStart() {
    auto &s = S();
    ChoosePath();
    if (s.config.delay <= 0) s.config.delay = DelayForRtt(s.link->best_rtt, s.config.mode);
    // Rollback over a long trip predicts further ahead: the window covers the one-way trip
    // beyond the delay plus some jitter (at least the configured window, at most 15 ticks).
    if (s.config.mode == SyncMode::Rollback && s.link->best_rtt >= 0) {
        const int one_way = static_cast<int>(std::ceil((s.link->best_rtt / 2.0) / (1000.0 / 60.0)));
        s.config.max_rollback = std::max(s.config.max_rollback, std::clamp(one_way - s.config.delay + 4, 8, 15));
    }
    if (s.start_sends == 0)
        Log::Info("netplay", "round trip " + std::to_string(s.link->best_rtt) + " ms: input delay " +
                             std::to_string(s.config.delay) + ", rollback window " + std::to_string(s.config.max_rollback));
    auto b = Header(kStart);
    PutU32(b, s.seed);
    b.push_back(static_cast<std::uint8_t>(s.config.delay));
    b.push_back(static_cast<std::uint8_t>(s.config.mode == SyncMode::Rollback ? 1 : 0));
    b.push_back(static_cast<std::uint8_t>(s.config.max_rollback));
    const std::size_t n = std::min<std::size_t>(s.payload.size(), 1000);
    b.push_back(static_cast<std::uint8_t>(n & 255));
    b.push_back(static_cast<std::uint8_t>(n >> 8));
    b.insert(b.end(), s.payload.begin(), s.payload.begin() + static_cast<std::ptrdiff_t>(n));
    b.push_back(static_cast<std::uint8_t>(s.link->relay ? 1 : 0)); // the guest relays too
    s.link->Send(b);
    s.last_start_send = NowMs();
    ++s.start_sends;
}

void StartSession() {
    auto &s = S();
    if (s.config.mode == SyncMode::Rollback && !Hooks().save) {
        Fail("rollback needs state snapshots (Net.Track)");
        return;
    }
    BuildFields();
    s.frame = s.sim_frame = 0;
    s.local.clear();
    s.remote.clear();
    s.used_remote.clear();
    const Bytes neutral(s.input_size, 0);
    for (int f = 0; f < s.config.delay; ++f) {
        s.local[static_cast<std::uint32_t>(f)] = neutral;
        s.remote[static_cast<std::uint32_t>(f)] = neutral;
    }
    s.remote_contiguous = static_cast<std::uint32_t>(s.config.delay);
    s.remote_acked = static_cast<std::uint32_t>(s.config.delay);
    s.state = State::Running;
    s.last_peer_recv = NowMs();
    Log::Info("netplay", std::string(Rollback() ? "rollback" : "lockstep") + " started as side " + std::to_string(s.side) +
                         " (" + s.link->kind + (s.link->relay ? ", relayed" : "") + "), delay " +
                         std::to_string(s.config.delay) + ", input " + std::to_string(s.input_size) + " bytes");
}

// Ticks whose state no longer changes: simulated with every real input (and
// not waiting to be simulated again after a wrong prediction).
std::uint32_t FinalFrame() {
    auto &s = S();
    return std::min({s.remote_contiguous, s.frame, s.rollback_to});
}

// A checksum is final once its tick (or, for state hashes, the ticks before
// it) ran with every real input; only final ones are sent or compared.
bool SumIsFinal(std::uint32_t key) {
    const std::uint32_t f = key & ~kEngineSum;
    if (f < S().resync_floor) return false;
    return (key & kEngineSum) ? f <= FinalFrame() : f < FinalFrame();
}

void SendInput() {
    auto &s = S();
    auto b = Header(kInput);
    PutU32(b, s.remote_contiguous);
    PutU32(b, static_cast<std::uint32_t>(NowMs()));
    // Echo the peer's last stamp with the time it waited here.
    PutU32(b, s.last_remote_stamp);
    PutU32(b, s.last_remote_stamp_at ? static_cast<std::uint32_t>(NowMs() - s.last_remote_stamp_at) : 0);
    PutU32(b, s.frame);
    b.push_back(static_cast<std::uint8_t>(static_cast<std::int8_t>(std::clamp(std::lround(s.advantage * 8.0), -127L, 127L))));
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
    // The latest few checksums of final ticks.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> sums;
    for (auto it = s.local_sums.rbegin(); it != s.local_sums.rend() && sums.size() < 4; ++it)
        if (SumIsFinal(it->first)) sums.emplace_back(it->first, it->second);
    b.push_back(static_cast<std::uint8_t>(sums.size()));
    for (const auto &[f, v] : sums) { PutU32(b, f); PutU32(b, v); }
    s.link->Send(b);
    s.last_send = NowMs();
}

void NoteDesync(std::uint32_t frame) {
    auto &s = S();
    if (s.desync) return;
    s.desync = true;
    s.desync_frame = frame & ~kEngineSum;
    Log::Warning("netplay", std::string("desync detected at frame ") + std::to_string(s.desync_frame) +
                            ((frame & kEngineSum) ? " (state hash)" : ""));
}

void CompareSums() {
    auto &s = S();
    for (auto it = s.remote_sums.begin(); it != s.remote_sums.end();) {
        if ((it->first & ~kEngineSum) < s.resync_floor) { it = s.remote_sums.erase(it); continue; }
        const auto mine = s.local_sums.find(it->first);
        if (mine == s.local_sums.end() || !SumIsFinal(it->first)) { ++it; continue; }
        if (mine->second != it->second) NoteDesync(it->first);
        it = s.remote_sums.erase(it);
    }
    while (s.local_sums.size() > 64) s.local_sums.erase(s.local_sums.begin());
    while (s.remote_sums.size() > 64) s.remote_sums.erase(s.remote_sums.begin());
}

void HandleInput(const std::uint8_t *p, std::size_t n) {
    auto &s = S();
    if (s.state != State::Running || n < 5 + 4 * 6 + 3) return;
    if (s.side == 1) s.start_acked = true;
    const std::uint8_t *q = p + 5;
    const std::uint32_t ack = GetU32(q); q += 4;
    const std::uint32_t stamp = GetU32(q); q += 4;
    const std::uint32_t echo = GetU32(q); q += 4;
    const std::uint32_t held = GetU32(q); q += 4;
    const std::uint32_t peer_frame = GetU32(q); q += 4;
    s.peer_advantage = static_cast<std::int8_t>(*q++) / 8.0;
    const std::uint32_t first = GetU32(q); q += 4;
    const std::uint8_t count = *q++;
    const std::uint8_t size = *q++;
    if (size != s.input_size) { Fail("the peer runs a different game version (input size differs)"); return; }
    if (q + static_cast<std::size_t>(count) * size > p + n) return;
    s.remote_acked = std::max(s.remote_acked, ack);
    s.last_remote_stamp = stamp;
    s.last_remote_stamp_at = NowMs();
    if (echo != 0) {
        const std::int64_t rtt = static_cast<std::int64_t>(static_cast<std::uint32_t>(NowMs()) - echo) - held;
        if (rtt >= 0 && rtt < 5000) s.ping_ms = s.ping_ms < 0 ? static_cast<int>(rtt) : (s.ping_ms * 7 + static_cast<int>(rtt)) / 8;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t f = first + i;
        if (f >= s.remote_contiguous && !s.remote.count(f)) {
            s.remote[f] = Bytes(q, q + size);
            // A tick already run on a different prediction is simulated again.
            if (Rollback() && f < s.frame) {
                const auto used = s.used_remote.find(f);
                if (used == s.used_remote.end() || used->second != s.remote[f]) s.rollback_to = std::min(s.rollback_to, f);
            }
        }
        q += size;
    }
    while (s.remote.count(s.remote_contiguous)) ++s.remote_contiguous;
    // Time sync (rollback): how far this side runs ahead of the peer.
    const double peer_now = peer_frame + (s.ping_ms > 0 ? s.ping_ms / 2.0 / (1000.0 / 60.0) : 0.0);
    s.advantage = s.advantage * 0.9 + (static_cast<double>(s.frame) - peer_now) * 0.1;
    if (q < p + n) {
        const std::uint8_t sums = *q++;
        for (std::uint8_t i = 0; i < sums && q + 8 <= p + n; ++i, q += 8) s.remote_sums[GetU32(q)] = GetU32(q + 4);
        CompareSums();
    }
}

// Desync recovery -------------------------------------------------------------
//
// The host, on a desync it noticed or the guest reported, sends its state of
// the latest final tick F (the state at the start of F). The guest asks for
// the code of functions it lacks, if any, then (at tick F or later) loads the
// state and simulates again from F to its present with the inputs it holds,
// as a rollback correction, and tells the host it is done. Checksums of ticks
// before F are no longer compared.

void PutU16(Bytes &b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 255));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 255));
}
std::uint32_t GetU16(const std::uint8_t *p) { return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8); }

void BeginTransfer(std::uint32_t id, std::uint8_t part, std::uint32_t frame, std::string data) {
    auto &t = S().transfer;
    t = Session::Transfer{};
    t.id = id;
    t.part = part;
    t.frame = frame;
    t.data = std::move(data);
    const std::size_t chunks = std::max<std::size_t>(1, (t.data.size() + kChunk - 1) / kChunk);
    t.got.assign(chunks, 0);
    t.sent.assign(chunks, 0);
}

void StartResync() {
    auto &s = S();
    if (s.side != 1 || s.synctest || s.resync_active || !Hooks().export_state) return;
    if (s.resync_attempts >= kMaxResyncs) return;
    ++s.resync_attempts;
    s.resync_wanted = false;
    // The state at the start of the latest final tick: the live one when that is
    // the present tick, else restored from its slot for the export.
    std::uint32_t f = Rollback() ? FinalFrame() : s.frame;
    std::string state;
    if (f >= s.frame) {
        f = s.frame;
        state = Hooks().export_state();
    } else {
        const int scratch = Slots();
        Hooks().save(scratch);
        Hooks().load(Slot(f));
        state = Hooks().export_state();
        Hooks().load(scratch);
    }
    if (state.size() > kMaxChunks * kChunk) {
        s.resync_error = "the state is too large to send";
        Log::Warning("netplay", s.resync_error);
        return;
    }
    s.resync_active = true;
    s.resync_started = NowMs();
    ++s.resync_id;
    s.resync_floor = std::max(s.resync_floor, f);
    s.resync_frame = f;
    s.resync_bytes = static_cast<std::uint32_t>(state.size());
    Log::Info("netplay", "desync: sending the state of tick " + std::to_string(f) + " (" + std::to_string(state.size()) + " bytes)");
    BeginTransfer(s.resync_id, 0, f, std::move(state));
}

// Host: chunks not acknowledged yet, again after a while (a few per pump).
void SendChunks() {
    auto &s = S();
    auto &t = s.transfer;
    const std::uint64_t now = NowMs();
    const std::uint64_t again = static_cast<std::uint64_t>(std::max(150, s.ping_ms > 0 ? s.ping_ms * 3 / 2 : 300));
    int budget = kChunksPerPump;
    const auto count = static_cast<std::uint32_t>(t.got.size());
    for (std::uint32_t i = 0; i < count && budget > 0; ++i) {
        if (t.got[i] || (t.sent[i] && now - t.sent[i] < again)) continue;
        auto b = Header(kResync);
        PutU32(b, t.id);
        b.push_back(t.part);
        PutU32(b, t.frame);
        PutU32(b, static_cast<std::uint32_t>(t.data.size()));
        PutU16(b, i);
        PutU16(b, count);
        const std::size_t at = static_cast<std::size_t>(i) * kChunk;
        const std::size_t len = std::min(kChunk, t.data.size() - std::min(at, t.data.size()));
        b.insert(b.end(), t.data.begin() + static_cast<std::ptrdiff_t>(at), t.data.begin() + static_cast<std::ptrdiff_t>(at + len));
        s.link->Send(b);
        t.sent[i] = now;
        --budget;
    }
}

void SendResyncAck() {
    auto &s = S();
    const auto &t = s.transfer;
    auto b = Header(kResyncAck);
    PutU32(b, t.id);
    b.push_back(t.part);
    b.push_back(static_cast<std::uint8_t>(s.resync_done == t.id && t.id ? 1 : 0));
    PutU32(b, s.resync_loaded_at);
    PutU16(b, static_cast<std::uint32_t>(t.got.size()));
    for (std::size_t i = 0; i < t.got.size(); i += 8) {
        std::uint8_t bits = 0;
        for (std::size_t k = 0; k < 8 && i + k < t.got.size(); ++k)
            if (t.got[i + k]) bits |= static_cast<std::uint8_t>(1u << k);
        b.push_back(bits);
    }
    s.link->Send(b);
    s.last_ack = NowMs();
    s.ack_dirty = false;
}

// Guest: what = 0 asks for a resync (it saw the desync), 1 for the code it lacks.
void SendResyncAsk(std::uint8_t what) {
    auto &s = S();
    auto b = Header(kResyncAsk);
    PutU32(b, what == 0 ? s.resync_done : s.transfer.id);
    b.push_back(what);
    const std::size_t n = what == 1 ? std::min<std::size_t>(s.resync_missing.size(), 120) : 0;
    PutU16(b, static_cast<std::uint32_t>(n));
    for (std::size_t i = 0; i < n; ++i) {
        PutU32(b, static_cast<std::uint32_t>(s.resync_missing[i]));
        PutU32(b, static_cast<std::uint32_t>(s.resync_missing[i] >> 32));
    }
    s.link->Send(b);
    s.resync_asked = NowMs();
}

// Guest: a whole part arrived.
void TransferComplete() {
    auto &s = S();
    auto &t = s.transfer;
    if (t.part == 0) {
        s.resync_state = t.data;
        s.resync_target = t.frame;
        s.resync_code.clear();
        s.resync_missing = Hooks().missing_code ? Hooks().missing_code(s.resync_state) : std::vector<std::uint64_t>{};
        if (s.resync_missing.empty()) s.resync_ready = true;
        else SendResyncAsk(1);
        return;
    }
    // Code: hash, length, code ...
    const auto *p = reinterpret_cast<const std::uint8_t *>(t.data.data());
    const auto *end = p + t.data.size();
    while (end - p >= 12) {
        const std::uint64_t hash = GetU32(p) | (static_cast<std::uint64_t>(GetU32(p + 4)) << 32);
        const std::uint32_t len = GetU32(p + 8);
        p += 12;
        if (static_cast<std::size_t>(end - p) < len) break;
        s.resync_code[hash].assign(reinterpret_cast<const char *>(p), len);
        p += len;
    }
    s.resync_missing.clear();
    s.resync_ready = true;
}

void HandleResync(const std::uint8_t *p, std::size_t n) {
    auto &s = S();
    if (s.side != 2 || s.state != State::Running || n < 5 + 4 + 1 + 4 + 4 + 2 + 2) return;
    const std::uint8_t *q = p + 5;
    const std::uint32_t id = GetU32(q); q += 4;
    const std::uint8_t part = *q++;
    const std::uint32_t frame = GetU32(q); q += 4;
    const std::uint32_t size = GetU32(q); q += 4;
    const std::uint32_t index = GetU16(q); q += 2;
    const std::uint32_t count = GetU16(q); q += 2;
    if (count == 0 || count > kMaxChunks || size > static_cast<std::size_t>(count) * kChunk || index >= count) return;
    if (id < s.resync_id || id <= s.resync_done) { s.ack_dirty = true; return; } // an old one: acknowledge (done)
    auto &t = s.transfer;
    if (id > s.resync_id || t.id != id || part > t.part) {
        if (id > s.resync_id) {
            s.resync_id = id;
            s.resync_ready = false;
            s.resync_state.clear();
            s.resync_code.clear();
            s.resync_missing.clear();
        }
        t = Session::Transfer{};
        t.id = id;
        t.part = part;
        t.frame = frame;
        t.data.assign(size, '\0');
        t.got.assign(count, 0);
        s.resync_active = true;
        s.resync_started = NowMs();
    }
    if (part != t.part || size != t.data.size() || count != t.got.size()) return;
    s.ack_dirty = true;
    if (t.complete) return;
    const std::size_t at = static_cast<std::size_t>(index) * kChunk;
    const std::size_t len = std::min(kChunk, static_cast<std::size_t>(size) - std::min<std::size_t>(at, size));
    if (static_cast<std::size_t>(p + n - q) != len) return;
    if (!t.got[index]) {
        std::memcpy(t.data.data() + at, q, len);
        t.got[index] = 1;
        ++t.received;
    }
    if (t.received == count) {
        t.complete = true;
        TransferComplete();
    }
}

void HandleResyncAck(const std::uint8_t *p, std::size_t n) {
    auto &s = S();
    if (s.side != 1 || n < 5 + 4 + 1 + 1 + 4 + 2) return;
    const std::uint8_t *q = p + 5;
    const std::uint32_t id = GetU32(q); q += 4;
    const std::uint8_t part = *q++;
    const bool done = *q++ != 0;
    const std::uint32_t loaded_at = GetU32(q); q += 4;
    const std::uint32_t count = GetU16(q); q += 2;
    auto &t = s.transfer;
    if (!s.resync_active || id != s.resync_id) return;
    if (done) {
        // The guest's checksums of ticks before it loaded the state may still be on the way.
        s.resync_floor = std::max(s.resync_floor, loaded_at);
        s.resync_active = false;
        s.desync = false;
        ++s.resyncs;
        s.resync_error.clear();
        t = Session::Transfer{};
        Log::Info("netplay", "desync: the guest loaded the state of tick " + std::to_string(s.resync_frame));
        return;
    }
    if (part != t.part || count != t.got.size()) return;
    for (std::uint32_t i = 0; i < count && q + i / 8 < p + n; ++i)
        if (q[i / 8] & (1u << (i % 8))) t.got[i] = 1;
}

void HandleResyncAsk(const std::uint8_t *p, std::size_t n) {
    auto &s = S();
    if (s.side != 1 || n < 5 + 4 + 1 + 2) return;
    const std::uint8_t *q = p + 5;
    const std::uint32_t id = GetU32(q); q += 4;
    const std::uint8_t what = *q++;
    const std::uint32_t count = GetU16(q); q += 2;
    if (what == 0) {
        // The guest saw a desync after the latest resync it loaded.
        if (!s.resync_active && id == s.resync_id) s.resync_wanted = true;
        return;
    }
    auto &t = s.transfer;
    if (!s.resync_active || id != s.resync_id || t.part != 0 || !Hooks().code) return;
    std::string bundle;
    for (std::uint32_t i = 0; i < count && q + 8 <= p + n; ++i, q += 8) {
        const std::uint64_t hash = GetU32(q) | (static_cast<std::uint64_t>(GetU32(q + 4)) << 32);
        const std::string code = Hooks().code(hash);
        Bytes head;
        PutU32(head, static_cast<std::uint32_t>(hash));
        PutU32(head, static_cast<std::uint32_t>(hash >> 32));
        PutU32(head, static_cast<std::uint32_t>(code.size()));
        bundle.append(reinterpret_cast<const char *>(head.data()), head.size());
        bundle += code;
    }
    Log::Info("netplay", "desync: sending code the guest lacks (" + std::to_string(count) + " functions)");
    BeginTransfer(s.resync_id, 1, t.frame, std::move(bundle));
}

void PumpResync() {
    auto &s = S();
    const std::uint64_t now = NowMs();
    if (s.side == 1) {
        if (!s.resync_active) return;
        if (now - s.resync_started > kResyncTimeoutMs) {
            s.resync_active = false;
            s.resync_error = "the guest did not load the state in time";
            Log::Warning("netplay", "desync: " + s.resync_error);
            return;
        }
        SendChunks();
        return;
    }
    if (s.ack_dirty && now - s.last_ack >= 15) SendResyncAck();
    if (s.done_sends > 0 && now - s.last_ack >= 100) {
        SendResyncAck();
        --s.done_sends;
    }
    if (s.desync && !s.resync_active && now - s.resync_asked > 500 && s.resync_attempts < kMaxResyncs) SendResyncAsk(0);
    if (s.resync_active && !s.resync_missing.empty() && s.transfer.part == 0 && now - s.resync_asked > 400) SendResyncAsk(1);
    if (s.resync_active && !s.resync_ready && now - s.resync_started > kResyncTimeoutMs) {
        s.resync_active = false;
        s.resync_error = "the host's state did not arrive in time";
        Log::Warning("netplay", "desync: " + s.resync_error);
    }
}

void HandleSessionPacket(const Bytes &packet) {
    auto &s = S();
    const std::uint8_t *p = packet.data();
    const std::size_t n = packet.size();
    s.last_peer_recv = NowMs();
    switch (p[4]) {
    case kStart:
        if (s.side == 2 && n >= 14) {
            if (s.state != State::Running) {
                s.seed = GetU32(p + 5);
                s.config.delay = std::clamp<int>(p[9], 1, 15);
                s.config.mode = p[10] ? SyncMode::Rollback : SyncMode::Lockstep;
                s.config.max_rollback = std::clamp<int>(p[11], 1, 30);
                const std::size_t len = static_cast<std::size_t>(p[12]) | (static_cast<std::size_t>(p[13]) << 8);
                if (14 + len <= n) s.payload.assign(reinterpret_cast<const char *>(p + 14), len);
                if (14 + len < n && p[14 + len] != 0) s.link->UseRelay();
                s.link->probe_relay = false;
                StartSession();
            }
            s.link->Send(Header(kStartAck));
        }
        break;
    case kStartAck:
        if (s.side == 1 && !s.start_acked) {
            s.start_acked = true;
            if (s.state != State::Running) StartSession();
        }
        break;
    case kInput:
        HandleInput(p, n);
        break;
    case kResync:
        HandleResync(p, n);
        break;
    case kResyncAck:
        HandleResyncAck(p, n);
        break;
    case kResyncAsk:
        HandleResyncAsk(p, n);
        break;
    case kBye:
        if (s.state == State::Running || s.state == State::Connecting || s.state == State::Waiting) {
            s.state = State::Disconnected;
            s.error = "the other player left";
            ActionMap::SetNetValues(nullptr, nullptr);
        }
        break;
    default:
        break;
    }
}

// LAN discovery: queries direct hosts and collects their answers.
struct Browser {
    Net::UdpSocket socket;
    std::uint64_t last_query = 0;
    std::map<std::string, std::pair<FoundHost, std::uint64_t>> hosts;
};
Browser &Browse() {
    static Browser browser;
    return browser;
}

void PumpDiscovery() {
    auto &b = Browse();
    if (!b.socket.IsOpen()) return;
    const auto &config = Requested();
    Net::Address from;
    while (auto packet = b.socket.Receive(from)) {
        const std::string text(packet->begin(), packet->end());
        if (text.rfind("PKD1 ! ", 0) != 0) continue;
        std::vector<std::string> lines;
        std::size_t at = 7;
        for (int i = 0; i < 3; ++i) {
            const auto nl = text.find('\n', at);
            if (nl == std::string::npos) break;
            lines.push_back(text.substr(at, nl - at));
            at = nl + 1;
        }
        if (lines.size() < 3 || lines[1] != config.game) continue;
        FoundHost host;
        const int port = std::atoi(lines[0].c_str());
        host.address = Net::Address{from.ip, static_cast<std::uint16_t>(port)}.ToString();
        host.name = lines[2];
        host.payload = text.substr(at);
        b.hosts[host.address] = {host, NowMs()};
    }
    const std::uint64_t now = NowMs();
    if (now - b.last_query < 1000) return;
    b.last_query = now;
    const std::string query = "PKD1 ? " + config.game;
    const auto port = static_cast<std::uint16_t>(config.discovery_port);
    b.socket.Send(Net::Address{0xFFFFFFFFu, port}, query.data(), query.size());
    b.socket.Send(Net::Address{0x7F000001u, port}, query.data(), query.size());
    for (const auto &i : Net::LocalInterfaces())
        if (i.broadcast != i.ip) b.socket.Send(Net::Address{i.broadcast, port}, query.data(), query.size());
}

bool BeginLink(std::unique_ptr<Link> link, const std::string &error, int side, const std::string &payload) {
    auto &s = S();
    if (!link) { Fail(error); return false; }
    s.link = std::move(link);
    s.side = side;
    s.payload = payload;
    if (side == 1) s.seed = NewSeed();
    s.state = State::Server;
    return true;
}

void SyncLinkState() {
    auto &s = S();
    if (s.state == State::Running || s.state == State::Error || s.state == State::Disconnected) return;
    switch (s.link->state) {
    case LinkState::Opening: s.state = State::Server; break;
    case LinkState::Waiting: s.state = State::Waiting; break;
    case LinkState::Connecting:
    case LinkState::Connected: s.state = State::Connecting; break;
    case LinkState::Failed: Fail(s.link->error); break;
    }
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
    c.max_rollback = std::clamp(config.max_rollback, 1, 30);
    if (c.sides.size() != 2) c.sides = {"p1_", "p2_"};
    if (c.name.empty()) c.name = Net::HostName();
    if (S().state == State::Idle) S().config = c;
}

const Config &GetConfig() { return Requested(); }

bool Host(const std::string &server, const std::string &payload) {
    ResetSession();
    std::string error;
    auto link = ServerLink(server, 1, "", S().config.force_relay, error);
    return BeginLink(std::move(link), error, 1, payload);
}

bool Join(const std::string &server, const std::string &room) {
    ResetSession();
    std::string error;
    auto link = ServerLink(server, 2, room, S().config.force_relay, error);
    return BeginLink(std::move(link), error, 2, "");
}

bool HostDirect(int port, const std::string &payload) {
    ResetSession();
    const auto &c = S().config;
    DiscoveryInfo info{c.discovery_port, c.game, c.name, payload};
    std::string error;
    auto link = DirectHostLink(port, info, error);
    return BeginLink(std::move(link), error, 1, payload);
}

bool JoinDirect(const std::string &address) {
    ResetSession();
    std::string error;
    auto link = DirectJoinLink(address, error);
    return BeginLink(std::move(link), error, 2, "");
}

bool CodeHost(const std::string &payload) {
    ResetSession();
    std::string error;
    auto link = CodeLink(1, S().config.stun, "", error);
    return BeginLink(std::move(link), error, 1, payload);
}

bool CodeJoin(const std::string &host_code) {
    ResetSession();
    std::string error;
    auto link = CodeLink(2, S().config.stun, host_code, error);
    return BeginLink(std::move(link), error, 2, "");
}

bool CodeConnect(const std::string &peer_code) {
    auto &s = S();
    if (!s.link || s.link->kind != "code" || s.side != 1) return false;
    return s.link->ConnectCode(peer_code);
}

bool SyncTest(int frames) {
    ResetSession();
    auto &s = S();
    if (!Hooks().save) { Fail("a sync test needs state snapshots (Net.Track)"); return false; }
    s.synctest = true;
    s.synctest_frames = std::clamp(frames, 1, 30);
    s.side = 1;
    s.seed = NewSeed();
    s.state = State::Running;
    s.config.mode = SyncMode::Rollback;
    s.config.delay = 0;
    return true;
}

void Leave() {
    auto &s = S();
    if (s.link) {
        const auto bye = Header(kBye);
        if (s.link->state == LinkState::Connected || s.state == State::Running)
            for (int i = 0; i < 3; ++i) s.link->Send(bye);
        s.link->Close();
    }
    ResetSession();
}

void Discover(bool on) {
    auto &b = Browse();
    if (!on) {
        b.socket.Close();
        b.hosts.clear();
        return;
    }
    if (!b.socket.IsOpen() && !b.socket.Open(0)) Log::Warning("netplay", "LAN discovery: " + b.socket.LastError());
    b.last_query = 0;
}

std::vector<FoundHost> FoundHosts() {
    auto &b = Browse();
    std::vector<FoundHost> out;
    const std::uint64_t now = NowMs();
    for (auto it = b.hosts.begin(); it != b.hosts.end();) {
        if (now - it->second.second > 3500) { it = b.hosts.erase(it); continue; }
        FoundHost h = it->second.first;
        h.age_ms = static_cast<int>(now - it->second.second);
        out.push_back(h);
        ++it;
    }
    // A host on this machine answers on loopback and on its LAN address: keep the LAN one.
    auto port_of = [](const std::string &a) { return a.substr(a.rfind(':')); };
    std::erase_if(out, [&](const FoundHost &h) {
        if (h.address.rfind("127.", 0) != 0) return false;
        return std::any_of(out.begin(), out.end(), [&](const FoundHost &o) {
            return &o != &h && o.address.rfind("127.", 0) != 0 && o.name == h.name && port_of(o.address) == port_of(h.address);
        });
    });
    return out;
}

void Pump() {
    PumpDiscovery();
    auto &s = S();
    if (!s.link) return;
    std::vector<Bytes> received;
    s.link->Pump(NowMs(), received);
    for (const auto &packet : received) {
        HandleSessionPacket(packet);
        if (!s.link) return;
    }
    SyncLinkState();
    const std::uint64_t now = NowMs();
    switch (s.state) {
    case State::Connecting:
        if (s.side == 1 && s.link->state == LinkState::Connected) {
            // With an automatic delay, or for the rollback window, first time a few round trips.
            if (!s.measure_started) s.measure_started = now;
            const bool measuring = (s.config.delay <= 0 || s.config.mode == SyncMode::Rollback || s.link->probe_relay) &&
                                   s.start_sends == 0 && now - s.measure_started < kMeasureMs &&
                                   (s.link->rtt_samples < kMeasureSamples ||
                                    (s.link->probe_relay && s.link->relay_samples < kRelaySamples));
            if (measuring) {
                if (now - s.last_probe > 50) { s.link->Probe(); s.last_probe = now; }
            } else if (now - s.last_start_send > 100) {
                SendStart();
            }
        }
        break;
    case State::Running:
        if (s.side == 1 && !s.start_acked && now - s.last_start_send > 100) SendStart();
        if (now - s.last_send > 30) SendInput();
        if (now - s.last_probe > kProbeEveryMs) { s.link->Probe(); s.last_probe = now; }
        PumpResync();
        if (now - s.last_peer_recv > kTimeoutMs) {
            s.state = State::Disconnected;
            s.error = "connection lost";
            ActionMap::SetNetValues(nullptr, nullptr);
            Log::Warning("netplay", "connection lost");
        }
        break;
    default:
        break;
    }
}

bool Running() { return S().state == State::Running; }

bool CanTick() {
    auto &s = S();
    if (s.state != State::Running) return false;
    if (s.synctest) return true;
    if (!s.local.count(s.frame)) return false;
    // A guest loading a resync stays within the inputs it keeps for the resimulation.
    if (s.side == 2 && s.resync_active && s.transfer.id && s.frame + 30 > s.transfer.frame + kInputHistory) return false;
    if (Rollback()) return s.frame < s.remote_contiguous + static_cast<std::uint32_t>(s.config.max_rollback);
    return s.remote.count(s.frame) > 0;
}

bool Resimulating() { return S().resimulating; }

double TickRate() {
    auto &s = S();
    if (s.state != State::Running || s.synctest || !Rollback()) return 1.0;
    s.paced = true;
    // Up to 6% slower: a tick of lead is gone in about a third of a second.
    return 1.0 - std::clamp((Excess() - 0.25) * 0.08, 0.0, 0.06);
}

namespace {

void RunTick(const std::function<void(bool)> &run_tick, std::uint32_t frame, bool resim) {
    auto &s = S();
    s.sim_frame = frame;
    s.resimulating = resim;
    const TickObserver &observer = Observer();
    if (observer.tick_started) observer.tick_started(frame, resim);
    run_tick(resim);
    if (observer.tick_finished) observer.tick_finished();
    s.resimulating = false;
}

void BeginCorrection(std::uint32_t from, std::uint32_t to) {
    if (Observer().correction_started) Observer().correction_started(from, to);
}

void EndCorrection() {
    if (Observer().correction_finished) Observer().correction_finished();
}

void FinishTick() {
    auto &s = S();
    s.local[s.frame + static_cast<std::uint32_t>(s.config.delay)] = SampleInput();
    ++s.frame;
    s.sim_frame = s.frame;
    // Inputs still needed: unacknowledged ones (resent), and those of ticks
    // that may be simulated again (and the tick before them, for edges).
    // A history of inputs stays for a resync's resimulation.
    const std::uint32_t history = s.frame > kInputHistory ? s.frame - kInputHistory : 0;
    const std::uint32_t keep = std::min({FinalFrame(), s.remote_acked, s.remote_contiguous, history});
    while (!s.local.empty() && s.local.begin()->first + 1 < keep) s.local.erase(s.local.begin());
    while (!s.remote.empty() && s.remote.begin()->first + 1 < std::min({FinalFrame(), s.remote_contiguous, history}))
        s.remote.erase(s.remote.begin());
    while (!s.used_remote.empty() && s.used_remote.begin()->first < FinalFrame()) s.used_remote.erase(s.used_remote.begin());
    CompareSums(); // checksums that became final
    SendInput();
}

// Guest: loads the host's state of tick F once this side has reached F and
// simulates again from there to the present (a correction).
void ApplyResync(const std::function<void(bool)> &run_tick) {
    auto &s = S();
    if (!s.resync_ready || s.frame < s.resync_target) return;
    const std::uint32_t from = s.resync_target;
    std::string error;
    BeginCorrection(from, s.frame);
    // This side's own state of tick F first, so what the import leaves alone
    // (tables the game marked local) is of tick F as well.
    if (Rollback() && from < s.frame && s.frame - from < static_cast<std::uint32_t>(Slots())) LoadSlot(from);
    const bool ok = Hooks().import_state && Hooks().import_state(s.resync_state, s.resync_code, error);
    if (ok) {
        s.rollback_to = kNone;
        for (std::uint32_t f = from; f < s.frame; ++f) {
            if (Rollback()) {
                SaveSlot(f);
                s.used_remote[f] = RemoteFor(f);
            }
            ApplyInputs(f);
            RunTick(run_tick, f, true);
        }
    }
    EndCorrection();
    s.resync_bytes = static_cast<std::uint32_t>(s.resync_state.size());
    s.resync_ready = false;
    s.resync_active = false;
    s.resync_state.clear();
    s.resync_code.clear();
    s.resync_done = s.transfer.id;
    s.resync_loaded_at = s.frame;
    ++s.resync_attempts;
    if (ok) {
        ++s.resyncs;
        s.resync_frame = from;
        s.resync_floor = std::max(s.resync_floor, from);
        s.desync = false;
        s.resync_error.clear();
        s.done_sends = 5;
        Log::Info("netplay", "desync: loaded the host's state of tick " + std::to_string(from) + ", simulated " +
                             std::to_string(s.frame - from) + " ticks again");
    } else {
        s.resync_error = error;
        Log::Warning("netplay", "desync: the host's state does not load: " + error);
    }
    SendResyncAck();
}

bool SyncTestTick(const std::function<void(bool)> &run_tick) {
    auto &s = S();
    auto sample_all = []() {
        Values v;
        for (const auto &d : ActionMap::Declarations()) v[d.name] = ActionMap::SampleLocal(d.name);
        return v;
    };
    auto apply = [&](std::uint32_t f) {
        const Values &now = s.synctest_inputs[f];
        const Values before = f > 0 ? s.synctest_inputs[f - 1] : Values{};
        ActionMap::SetNetValues(&now, &before);
    };
    auto check = [&](std::uint32_t f) {
        const std::uint32_t h = Hooks().hash(Slot(f));
        const auto found = s.synctest_hash.find(f);
        if (found == s.synctest_hash.end()) s.synctest_hash[f] = h;
        else if (found->second != h) NoteDesync(f);
    };
    const std::uint32_t f = s.frame;
    s.synctest_inputs[f] = sample_all();
    if (f == 0) { SaveSlot(0); check(0); }
    apply(f);
    RunTick(run_tick, f, false);
    ++s.frame;
    SaveSlot(s.frame);
    check(s.frame);
    const auto n = static_cast<std::uint32_t>(s.synctest_frames);
    if (s.frame > n) {
        const std::uint32_t target = s.frame - n;
        BeginCorrection(target, s.frame);
        LoadSlot(target);
        for (std::uint32_t g = target; g < s.frame; ++g) {
            if (g > target) { SaveSlot(g); check(g); }
            apply(g);
            RunTick(run_tick, g, true);
        }
        EndCorrection();
        SaveSlot(s.frame);
        check(s.frame);
        s.resimulated += n;
        ++s.rollbacks;
    }
    s.sim_frame = s.frame;
    while (!s.synctest_inputs.empty() && s.synctest_inputs.begin()->first + n + 2 < s.frame) s.synctest_inputs.erase(s.synctest_inputs.begin());
    while (!s.synctest_hash.empty() && s.synctest_hash.begin()->first + n + 2 < s.frame) s.synctest_hash.erase(s.synctest_hash.begin());
    return true;
}

} // namespace

bool Tick(const std::function<void(bool resimulating)> &run_tick) {
    auto &s = S();
    if (s.state != State::Running) { run_tick(false); return true; }
    if (s.synctest) return SyncTestTick(run_tick);
    if (s.side == 1 && (s.desync || s.resync_wanted) && !s.resync_active) StartResync();
    if (s.side == 2) ApplyResync(run_tick);
    if (!Rollback()) {
        if (!CanTick()) { ++s.stalls; return false; }
        ApplyInputs(s.frame);
        RunTick(run_tick, s.frame, false);
        FinishTick();
        return true;
    }
    // Rollback: first simulate again from the earliest wrong prediction.
    if (s.rollback_to < s.frame) {
        const auto started = std::chrono::steady_clock::now();
        const std::uint32_t from = s.rollback_to;
        BeginCorrection(from, s.frame);
        LoadSlot(from);
        for (std::uint32_t f = from; f < s.frame; ++f) {
            if (f > from) SaveSlot(f);
            ApplyInputs(f);
            s.used_remote[f] = RemoteFor(f);
            RunTick(run_tick, f, true);
        }
        EndCorrection();
        ++s.rollbacks;
        s.resimulated += s.frame - from;
        s.max_rollback_seen = std::max(s.max_rollback_seen, static_cast<int>(s.frame - from));
        const std::chrono::duration<double, std::milli> took = std::chrono::steady_clock::now() - started;
        s.slowest_rollback_ms = std::max(s.slowest_rollback_ms, took.count());
    }
    s.rollback_to = kNone;
    // State hashes of final ticks, compared with the peer's.
    const std::uint32_t final_frame = FinalFrame();
    const auto oldest = s.frame >= static_cast<std::uint32_t>(Slots()) ? s.frame - static_cast<std::uint32_t>(Slots()) + 1 : 0;
    for (std::uint32_t f = (s.hashed_upto / kHashEvery + 1) * kHashEvery; f <= final_frame && f < s.frame; f += kHashEvery) {
        if (f >= oldest && Hooks().hash && s.config.compare_state) s.local_sums[f | kEngineSum] = Hooks().hash(Slot(f));
        s.hashed_upto = f;
    }
    if (!CanTick()) { ++s.stalls; return false; }
    // Time sync (as GGPO): the side that runs ahead of the peer, by both
    // sides' estimates, waits a tick now and then (a paced caller has
    // already slowed down, so only when that has not been enough).
    if (++s.ticks_since_wait >= 8 && Excess() >= (s.paced ? 2.0 : 1.0)) {
        s.ticks_since_wait = 0;
        s.advantage -= 1.0;
        ++s.stalls;
        ++s.waits;
        return false;
    }
    SaveSlot(s.frame);
    ApplyInputs(s.frame);
    s.used_remote[s.frame] = RemoteFor(s.frame);
    RunTick(run_tick, s.frame, false);
    FinishTick();
    return true;
}

void SetStateHooks(StateHooks hooks) { Hooks() = std::move(hooks); }
void SetTickObserver(TickObserver observer) { Observer() = std::move(observer); }

void ReportChecksum(std::uint32_t frame, std::uint32_t value) {
    auto &s = S();
    if (s.state != State::Running) return;
    s.local_sums[frame & ~kEngineSum] = value;
    CompareSums();
}

Status GetStatus() {
    auto &s = S();
    Status st;
    st.state = s.state;
    st.side = s.side;
    st.link = s.synctest ? "synctest" : (s.link ? s.link->kind : "");
    st.mode = s.config.mode == SyncMode::Rollback ? "rollback" : "lockstep";
    st.error = s.error;
    st.payload = s.payload;
    st.seed = s.seed;
    st.ping_ms = s.ping_ms >= 0 || !s.link ? s.ping_ms : s.link->best_rtt;
    st.delay = s.config.delay;
    st.frame = s.sim_frame;
    st.stalls = s.stalls;
    st.waits = s.waits;
    st.rollbacks = s.rollbacks;
    st.resimulated = s.resimulated;
    st.max_rollback_seen = s.max_rollback_seen;
    st.slowest_rollback_ms = s.slowest_rollback_ms;
    st.desync = s.desync;
    st.desync_frame = s.desync_frame;
    st.resyncing = s.resync_active;
    st.resyncs = s.resyncs;
    st.resync_frame = s.resync_frame;
    st.resync_bytes = s.resync_bytes;
    st.resync_error = s.resync_error;
    if (s.link) {
        st.room = s.link->room;
        st.code = s.link->code;
        st.nat = s.link->nat;
        st.relay = s.link->relay;
        st.loss = s.link->Loss(NowMs());
        st.direct_ms = s.link->direct_rtt;
        st.relay_ms = s.link->relay_rtt;
        st.peer = s.link->peer_text;
        st.lan = s.link->lan_text;
    }
    return st;
}

} // namespace Netplay
