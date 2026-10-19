#ifndef POCKET2D_NETPLAY_H
#define POCKET2D_NETPLAY_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Two-player netplay on a deterministic simulation: each tick only the
// players' action values cross the network. The local player's input is
// sampled from the actions named with the local prefix (their own key
// bindings) and becomes, on both machines, the input of their side's actions;
// shared actions combine both players.
//
// How peers meet (the link) and how ticks are kept in step (the sync mode) are
// independent:
//   links: a room server (room code, hole punching, relay), a direct address
//          (LAN or a virtual LAN such as Tailscale or ZeroTier, found by LAN
//          discovery or typed), or connection codes (public STUN, codes
//          exchanged by the players).
//   sync:  lockstep (a tick runs once both inputs have arrived; input is
//          delayed to hide the round trip) or rollback (ticks run at once on a
//          prediction of the peer's input; when the real input differs, the
//          game state is restored and the ticks are simulated again).
//
// Rollback needs the game state to be saved and restored (SetStateHooks; the
// Lua API snapshots the Lua state). A sync test session checks that: every
// tick it rolls back and resimulates and compares the state.
namespace Netplay {

enum class SyncMode { Lockstep, Rollback };

struct Config {
    std::string local_prefix = "p1_";              // the local player's bindings
    std::vector<std::string> sides = {"p1_", "p2_"}; // side 1 = host, side 2 = guest
    std::vector<std::string> shared;               // actions both players drive
    SyncMode mode = SyncMode::Lockstep;
    int delay = 3;                                 // input delay in ticks (1..15); 0: from the round trip
    int max_rollback = 8;                          // rollback: ticks run ahead of the peer's confirmed input
    bool compare_state = false;                    // rollback: also compare whole state hashes with the peer
                                                   // (only when everything tracked is shared, nothing local)
    bool force_relay = false;                      // room server: skip hole punching
    std::string game;                              // LAN discovery: only games with the same id see each other
    std::string name;                              // LAN discovery: the host's name (default: the computer's)
    std::vector<std::string> stun = {"stun.l.google.com:19302", "stun.cloudflare.com:3478"};
    int discovery_port = 27779;
};

enum class State { Idle, Server, Waiting, Connecting, Running, Disconnected, Error };
const char *StateName(State state);

struct Status {
    State state = State::Idle;
    int side = 0;              // 1 host, 2 guest
    std::string link;          // "server", "direct", "code", "synctest"
    std::string mode;          // "lockstep", "rollback"
    std::string room;          // room code (room server)
    std::string code;          // connection code (once the public address is known)
    std::string nat;           // connection codes: "cone", "symmetric", "unknown"
    std::string error;
    std::string payload;       // the host's session payload (both sides)
    std::uint32_t seed = 0;    // shared session seed
    int ping_ms = -1;
    int delay = 0;             // input delay in ticks (0 until chosen)
    bool relay = false;        // traffic goes through the room server
    std::uint32_t frame = 0;   // the tick being (or next to be) simulated
    std::uint32_t stalls = 0;  // ticks spent waiting
    std::uint32_t waits = 0;   // rollback: of those, ticks held back for the peer to catch up
    std::uint32_t rollbacks = 0;       // rollback: corrections
    std::uint32_t resimulated = 0;     // rollback: ticks simulated again
    int max_rollback_seen = 0;
    double slowest_rollback_ms = 0;    // rollback: the longest correction (restore and resimulation)
    bool desync = false;
    std::uint32_t desync_frame = 0;
    std::string peer;          // peer address
    std::string lan;           // direct host: the address the guest enters
};

void Configure(const Config &config);
const Config &GetConfig();

// Room server: Host opens a room (the code appears in Status().room); Join
// enters one. `server` is "host[:port]" (default port 27777).
bool Host(const std::string &server, const std::string &payload);
bool Join(const std::string &server, const std::string &room);
// Direct (LAN or virtual LAN): listen on a port, or connect to "ip[:port]"
// (default port 27778). A direct host answers LAN discovery.
bool HostDirect(int port, const std::string &payload);
bool JoinDirect(const std::string &address);
// Connection codes: CodeHost and CodeJoin learn this machine's public address
// (Status().code); the guest connects to the host's code at once, and the host
// connects to the guest's code when given it (CodeConnect). Both sides punch.
bool CodeHost(const std::string &payload);
bool CodeJoin(const std::string &host_code);
bool CodeConnect(const std::string &peer_code);
// An offline session that rolls back `frames` ticks every tick and compares
// the resimulated state (needs state hooks). Inputs are the local actions.
bool SyncTest(int frames);
void Leave();

// LAN discovery of direct hosts with the configured game id.
struct FoundHost {
    std::string address;   // "ip:port" to JoinDirect
    std::string name;
    std::string payload;
    int age_ms = 0;
};
void Discover(bool on);
std::vector<FoundHost> FoundHosts();

// Called every frame: network housekeeping (handshakes, resends, timeouts).
void Pump();
bool Running();
// Whether Tick would run a tick now (lockstep: both inputs are here; rollback:
// not too far ahead). Headless steppers wait on it.
bool CanTick();
// Runs the session's next tick through `run_tick(resimulating)`, first
// resimulating corrected ticks (rollback). Returns false when the tick has to
// wait (counted as a stall). Draws of resimulated ticks are the caller's to
// drop; the audio API follows the ticks (SetTickObserver) to play each sound
// once.
bool Tick(const std::function<void(bool resimulating)> &run_tick);
bool Resimulating();
// Rollback time sync for a caller that runs ticks on a real-time clock: the
// rate (just below 1 while this side runs ahead of the peer, else 1) at which
// to advance it. A small slowdown hides what a skipped tick would show, so
// once a caller asks, Tick holds a tick back only when far ahead.
double TickRate();

// Rollback state: save to and restore from numbered slots, and hash a slot.
struct StateHooks {
    std::function<void(int slot)> save;
    std::function<void(int slot)> load;
    std::function<std::uint32_t(int slot)> hash;
};
void SetStateHooks(StateHooks hooks);

// Effects outside the rolled-back state (sounds) follow the ticks: told when
// each session tick starts and finishes (resimulated or not), and around a
// correction that simulates ticks [from, to) again (`to` is the present tick).
struct TickObserver {
    std::function<void(std::uint32_t frame, bool resimulating)> tick_started;
    std::function<void()> tick_finished;
    std::function<void(std::uint32_t from, std::uint32_t to)> correction_started;
    std::function<void()> correction_finished;
};
void SetTickObserver(TickObserver observer);

// Game state hashes compared between the peers (desync detection).
void ReportChecksum(std::uint32_t frame, std::uint32_t value);

Status GetStatus();

} // namespace Netplay

#endif
