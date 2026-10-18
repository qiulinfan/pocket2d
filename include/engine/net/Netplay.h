#ifndef POCKET2D_NETPLAY_H
#define POCKET2D_NETPLAY_H

#include <cstdint>
#include <string>
#include <vector>

// Two-player lockstep netplay. Both machines run the same deterministic
// simulation; each tick only the players' action values cross the network.
// The local player's input is sampled from the actions named with the local
// prefix (their own key bindings) and becomes, on both machines, the input of
// their side's actions; shared actions combine both players. A tick runs only
// when both players' input for it has arrived; input is scheduled `delay`
// ticks ahead to hide the round trip (automatic: the host times a few round
// trips before the session starts and picks one-way ticks + 1, 2..10).
//
// Peers find each other through a room server (tools/room_server: a room code,
// UDP hole punching, relaying when punching fails) or directly on a LAN.
namespace Netplay {

struct Config {
    std::string local_prefix = "p1_";              // the local player's bindings
    std::vector<std::string> sides = {"p1_", "p2_"}; // side 1 = host, side 2 = guest
    std::vector<std::string> shared;               // actions both players drive
    int delay = 3;                                 // input delay in ticks (1..15); 0: from the round trip
    bool force_relay = false;                      // skip hole punching (tests, strict NATs)
};

enum class State { Idle, Server, Waiting, Connecting, Running, Disconnected, Error };
const char *StateName(State state);

struct Status {
    State state = State::Idle;
    int side = 0;              // 1 host, 2 guest
    std::string room;          // room code (room server)
    std::string error;
    std::string payload;       // the host's session payload (both sides)
    std::uint32_t seed = 0;    // shared session seed
    int ping_ms = -1;
    int delay = 0;             // input delay in ticks (0 until chosen)
    bool relay = false;        // traffic goes through the room server
    std::uint32_t frame = 0;   // lockstep ticks run
    std::uint32_t stalls = 0;  // ticks spent waiting for the peer
    bool desync = false;
    std::uint32_t desync_frame = 0;
    std::string peer;          // peer address
    std::string lan;           // LAN host: the address the guest enters
};

void Configure(const Config &config);
const Config &GetConfig();

// Room server: Host opens a room (the code appears in Status().room); Join
// enters one. `server` is "host[:port]" (default port 27777).
bool Host(const std::string &server, const std::string &payload);
bool Join(const std::string &server, const std::string &room);
// LAN: listen on a port, or connect to "ip[:port]" (default port 27778).
bool HostDirect(int port, const std::string &payload);
bool JoinDirect(const std::string &address);
void Leave();

// Called every frame: network housekeeping (handshakes, resends, timeouts).
void Pump();

bool Running();
// The current tick's input from both players has arrived.
bool ReadyForTick();
// Applies the current tick's synced input to the actions.
void BeginTick();
// After the tick: schedules local input `delay` ticks ahead and sends it.
void EndTick();
// Notes a stall (a frame that could not tick).
void NoteStall();

// Game state hashes compared between the peers (desync detection).
void ReportChecksum(std::uint32_t frame, std::uint32_t value);

Status GetStatus();

} // namespace Netplay

#endif
