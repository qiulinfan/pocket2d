# Netplay

Two players on two computers play one game on a deterministic simulation:
each tick only the players' action values cross the network. Code:
`src/engine/net/` (`UdpSocket`, `ConnectionCode`, `Link`, `Netplay`), the Lua
`Net` namespace ([Lua API](../lua-api/netplay.md)) with its rollback snapshots
(`src/engine/scripting/luaapi/LuaSnapshot.cpp`), and the room server
`tools/room_server/room_server.py`.

## Layers

| Layer | Choices | Where |
| --- | --- | --- |
| Link: how the peers reach each other | room server, direct (LAN or virtual LAN, with LAN discovery), connection codes | `Link.cpp` |
| Session: start, input exchange, desync checks | one protocol for every link | `Netplay.cpp` |
| Sync: when a tick runs | lockstep, rollback, sync test | `Netplay.cpp` |
| Game state for rollback | Lua graph snapshots of tracked roots | `LuaSnapshot.cpp` |

Any link works with either sync mode. An Epic Online Services link was
designed and set aside in favour of connection codes ([netplay-eos.md](netplay-eos.md)). The engine loop calls
`Netplay::Tick(run_tick)`, which decides whether the tick runs and first
resimulates corrected ticks.

## Input

The local player's input is sampled from the actions named with
`local_prefix` (by default their `p1_` bindings, so both players use their
own keyboard's player 1 keys). One byte per button or axis (an axis2d takes
two). On both machines it becomes the input of that player's side: the host
is side 1 (`p1_` actions), the guest side 2 (`p2_`). `shared` actions (menu
confirm, pause) combine both players: buttons by OR, axes by sum. While a
session runs, every action not synced reads as released, and pressed /
released edges compare a tick's input with the tick before (not with what the
previous frame held), so a resimulated tick sees the same edges.

## Links

- **Room server.** The host sends `PK2 HOST` and gets a four-character room
  code (no O/0/I/1). The guest sends `PK2 JOIN <code>`; the server sends each
  side the other's public and LAN addresses. Both punch to both addresses for
  3 s (and at least 20 punches, so a stalled game does not give up early); if
  no direct packet arrives, traffic is relayed through the server
  (`PK2R` frames). `force_relay` skips punching. While punching, each side
  also times the relay (punches through the server, answered the same way, the
  stamp's top bit set); before START the host compares the best round trip of
  each path and plays over the relay when it is faster by a fifth and 10 ms
  (`direct_ms`, `relay_ms` in the status), and START tells the guest to relay
  too. Rooms expire after two minutes without traffic; no answer from the
  server for 8 s fails.
- **Direct.** `HostDirect(port)` listens (default 27778) and its status `lan`
  is the address to type; `JoinDirect("ip[:port]")` connects. This is also the
  virtual LAN path: with Tailscale, ZeroTier or Radmin VPN both machines are on
  one virtual network and the guest joins the host's virtual address.
  `Net::LocalInterfaces()` lists every interface with a `kind` (lan,
  tailscale for 100.64.0.0/10, zerotier by adapter name, radmin for
  26.0.0.0/8) so a game can show the right address.
- **LAN discovery.** A direct host whose config names a `game` answers
  `PKD1 ? <game>` queries on UDP 27779 (`discovery_port`, shared with
  SO_REUSEADDR/SO_REUSEPORT) with its game port, name and payload. Browsers
  query once a second by broadcast (255.255.255.255 and each interface's
  subnet broadcast) and on 127.0.0.1, and forget hosts silent for 3.5 s.
  Broadcasts cross ZeroTier and Radmin networks, not Tailscale.
- **Connection codes.** No server of ours: each side asks public STUN servers
  (`stun`, default Google's and Cloudflare's) for its public address from the
  game socket, and shows it with its LAN address as a 21-character code
  (Crockford base32, CRC-8 check, "ABCDEFG-HJKMNPQ-RSTVWXY"). The guest types
  the host's code and starts punching; the host types the guest's code back
  (`CodeConnect`) and punches too. Hosts behind a permissive NAT get through
  without the second code. Two different mapped ports from two STUN servers
  mean a symmetric NAT (`nat = "symmetric"`), which rarely punches through;
  there is no relay, so after 20 s it fails. If no STUN server answers, the
  code carries only the LAN address.

## Session

The host's START carries the seed (`std::random_device`), the sync mode, the
delay, the rollback window and the game's payload (up to 1000 bytes); the
guest adopts them. With `delay = 0` the host first times five punch round
trips (at most 0.5 s): lockstep uses one-way ticks + 1 (2..10), rollback half
that (1..2), and rollback (automatic delay or not) widens its window to the one-way trip beyond the delay plus 4 ticks (8..15). Input packets carry every unacknowledged frame (up to 32), the
acknowledgement, a ping stamp, the sender's frame (time sync) and the latest
checksums; they are resent every 30 ms. While playing, a timed punch goes out
every 250 ms: `loss` in the status is the share of the last 40 that got no
answer within 1.5 s (-1 until 8 are that old), and before the session `ping` is
the best punch round trip, so a game can show the connection's quality before
the match. No packet for 6 s is a lost
connection; BYE ends the session. Different input layouts fail with
"different game version".

## Sync

- **Lockstep.** A tick runs when both inputs for it are here. Input is
  scheduled `delay` ticks ahead. `GameLoop` keeps drawing while it waits;
  headless `StepSimulation` waits up to 0.5 s, then returns without advancing.
- **Rollback.** A tick runs at once with the peer's input predicted (its last
  known input). Before each tick the state is saved in a ring of
  `max_rollback + 2` slots (with the world frame number). When the peer's real
  input for an already simulated tick differs from the prediction, the next
  `Tick` restores that tick's slot and simulates the ticks up to now again
  (`run_tick(true)`): their draws are discarded and their sounds go through the
  sound ledger (below).
  A side may run at most `max_rollback` ticks past the peer's last confirmed
  input, then it waits. Time sync (as GGPO): from the peer's frame and the ping each side
  estimates how far it runs ahead and sends the estimate; when half the
  difference of the two estimates reaches a tick, the side ahead skips a tick
  (at most every 8 ticks, counted in `waits`). `GameLoop` asks `TickRate` each
  frame and runs its tick clock up to 6% slower while ahead, so the lead
  melts without a visible skip; once a caller paces itself that way, `Tick`
  skips only at two ticks ahead. Checksums are only sent for final ticks (all real inputs).
  With `compare_state`, every 60 ticks the engine also hashes the final state
  snapshot and compares it with the peer's (`desync_frame` names the tick);
  that only works when the tracked state holds nothing local (settings, the
  player's side, save data), so it is off by default and games report their
  own checksums of the shared state.
- **Sync test.** `Net.SyncTest(n)` runs offline with the local actions as
  input. Every tick it saves and hashes the state, then restores the state of
  `n` ticks ago and simulates them again, comparing every tick's hash with the
  first run's. A mismatch means some state that gameplay depends on is not in
  the snapshots (or is not deterministic); `desync_frame` is the first tick
  that differed.
- **Desync recovery (not implemented).** A desync is detected and reported,
  never repaired. The planned repair: the host is authoritative. When the
  checksums of final tick F differ, both sides stop advancing past F (as they
  wait for input today) and the host sends its saved state of F. The guest
  loads it and both simulate from F to the present with the confirmed inputs
  they already hold, then resume. Open points: a `LuaSnapshot` slot refers to
  the process's own tables and closures, so the state needs a portable form
  (tables numbered in walk order with type-tagged values; a closure named by
  its prototype's source and line, rebuilt with its upvalues), which also has
  to cope with a guest whose object graph already differs in shape; the
  transfer (its size for a real game is not measured yet) goes in numbered chunks
  over the session's resent messages; engine-side state is outside the
  snapshot as it is for rollback; and a game hook rebuilds what is derived
  from the state (draw caches, sounds in flight). Until then a desynced
  session ends with the desync shown, as now.

## Rollback state

`LuaSnapshot` saves the Lua object graph reachable from the tracked roots
(`Net.Track`, default the global table; a component's functions reach the
global table through `_ENV` anyway). Each slot is one flat registry array
reused between saves: for every reachable table its contents and metatable,
for every Lua closure its upvalues. Loading writes them back in place, so
every table keeps its identity and closures and other tables still refer to
the right objects; tables created after the save simply become unreachable.
`Net.Static` marks tables that never change (converted game data); they are
neither walked nor restored, which is what keeps a save cheap. Only the marked
table is skipped: data that live objects refer to directly (a clip's sprite)
must be marked too, or it is walked through them. A load writes values in
place when a table still has exactly the saved keys and clears and refills it
otherwise; it sets a metatable only when it changed. `slowest_rollback_ms`
in the status is the longest correction (restore plus resimulation). Not covered:
engine-side state (actors, physics bodies, event subscriptions, particles),
userdata contents (kept as values) and coroutines. Games that use rollback
keep their simulation in Lua and check it with the sync test.

## Sounds across corrections

Session ticks play sound effects through a `SoundLedger`
(`src/engine/audio/SoundLedger.cpp`), told by `Netplay::SetTickObserver` when
each tick starts and finishes and around each correction. As in GGPO games,
every request is keyed by its tick, its clip and how many requests of that clip
came before it in the tick:

- a resimulated tick that asks for a sound it already played does not play it
  again;
- a sound that first appears in a correction (the prediction missed the hit)
  plays at once, from its start while it is at most 6 ticks late, and that far
  into the clip after that, so long sounds stay in time with the picture;
- a sound of a corrected tick that the correction did not ask for again was
  mispredicted: it fades out over 40 ms.

Effects then play as voices on a free mixer channel (the asked one when it is
free), so a late sound cannot cut off one already playing; the game's channel
numbers keep their gain (`Audio.SetVolume`) and name their latest voice for
`Audio.Halt`, `Audio.IsPlaying` and later gain changes. A halt in a resimulated
tick is ignored. Music is level triggered: a correction's last `Music.Play` /
`Music.Halt` is applied when it ends. Requests older than 64 ticks are
forgotten. Agents see `audio.play` events with `tick`, `nth` and `late`, and
`audio.cancel` for mispredicted sounds; `tests/netplay_test.py` checks that both
machines end up with the same sounds, each played once.

## Determinism requirements

- **Lua string hashing** is seeded with a constant (`luai_makeseed` in
  `thirdparty/CMakeLists.txt`), so `pairs` over string-keyed tables visits keys
  in the same order on every machine.
- **Games** must not read raw keys, the mouse, wall-clock time, `math.random`,
  `Audio.IsPlaying` or other machine-local state in gameplay during a session,
  and must not depend on `pairs` order over tables keyed by tables or
  functions (addresses differ).
- **Floating point**: see the cross-platform section below.

## Cross-platform determinism

Peers on different systems (macOS arm64, Windows x64, Linux x64) must compute
the same bits.

- **Basic arithmetic** (`+ - * /`, `math.sqrt`, `math.fmod`, `math.floor`,
  integer operations) is exact IEEE 754 on every supported CPU: arm64 and
  x86_64 both use 64-bit doubles (SSE2, no x87). Lua is compiled with
  `-ffp-contract=off`, so the compiler never fuses a multiply and an add into
  an FMA on one platform and not on another.
- **Transcendental functions and `^`** differ between C libraries in their
  last bits. Lua's `math.sin/cos/tan/asin/acos/atan/exp/log` and `^` call
  `pocket_det_*` instead (`thirdparty/detmath`): openlibm's (FreeBSD msun /
  FDLIBM) double functions, fetched at a pinned tag and compiled from source
  with plain IEEE arithmetic and no contraction, under prefixed names so the
  platform's own libm is untouched (`cmake/GameEngineDetMath.cmake`).
  `pocket_lua_user.h` (Lua's `LUA_USER_H`) maps them, and also fixes the
  string-hash seed.
- **Formatting and parsing** (`tostring`, `string.format`, `tonumber`) use the
  C library's correctly rounded `printf`/`strtod` (Apple libc, glibc, the
  Windows UCRT).
- **`math.log`** is replaced at startup (`APIRegistration.cpp`): Lua's own
  computes `math.log(x, 2)` with `log2`, except where `luaconf.h` declares the
  platform C89 (Windows), where it divides two logarithms, which differs in
  the last bits.
- `tests/determinism_test.py` (CTest `determinism`) hashes 2000 rounds of
  float math, formatting, table order and sorting, one hash per operation, and
  compares each with the recorded value, so a failure names the operation.
  macOS arm64, the Linux x86_64 runtime kit (GCC 14, glibc, under qemu) and
  Windows x64 (MSVC and llvm-mingw) agree; Windows differed only in `log2`
  before the `math.log` replacement.
- Engine-side math (Box2D, particles) still uses the platform's libm; games
  that use rollback keep their simulation in Lua.

## Deploying the room server

The server is one Python 3.8+ file with no dependencies, using UDP port 27777.

```bash
scp tools/room_server/room_server.py tools/room_server/pocket2d-room.service user@VPS:/tmp/
ssh user@VPS
sudo install -D -m 0755 /tmp/room_server.py /opt/pocket2d-room/room_server.py
sudo install -m 0644 /tmp/pocket2d-room.service /etc/systemd/system/pocket2d-room.service
sudo systemctl daemon-reload
sudo systemctl enable --now pocket2d-room
sudo ufw allow 27777/udp        # or the cloud provider's security group: UDP 27777 inbound
journalctl -u pocket2d-room -f  # "room NNNN opened by ..." when someone hosts
```

The unit runs the server as a dynamic unprivileged user. Games find the server
through `POCKET_NET_SERVER` or their own configuration.

## Tests

- `tests/netplay_unit_test.cpp` (CTest `netplay_unit`): connection codes
  (round trip, typos), STUN messages, Lua snapshots (in-place restore,
  identity, upvalues, static tables, hashes) and the sound ledger.
- `tests/netplay_test.py` (CTest `netplay`): two headless runtimes through a
  local room server (punching, forced relay, rollback with corrections, the
  relay chosen over a slow direct path behind a NAT-like proxy), a
  direct host found by LAN discovery, connection codes through a local STUN
  server with rollback, and the sync test (passing with all state tracked,
  failing on state outside the snapshots); sounds are played once, late ones
  and cancellations happen with rollback, and both machines keep the same.
- Games test their own sessions; BleachVsNaruto's `tools/net_test.py` plays
  whole sessions, also over a local UDP proxy with added latency and loss.
