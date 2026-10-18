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

Any link works with either sync mode. The engine loop calls
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
  3 s; if no direct packet arrives, traffic is relayed through the server
  (`PK2R` frames). `force_relay` skips punching. Rooms expire after two minutes
  without traffic; no answer from the server for 8 s fails.
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
that (1..3). Input packets carry every unacknowledged frame (up to 32), the
acknowledgement, a ping stamp, the sender's frame (time sync) and the latest
checksums; they are resent every 30 ms. No packet for 6 s is a lost
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
  (`run_tick(true)`): their draws are discarded and the audio API stays silent.
  A side may run at most `max_rollback` ticks past the peer's last confirmed
  input, then it waits. Time sync: from the peer's frame and the ping each side
  estimates how far it runs ahead; above 1.5 ticks it skips a tick at most
  every 8 ticks. Checksums are only sent for final ticks (all real inputs).
  Every 60 ticks the engine also hashes the final state snapshot and compares
  it with the peer's (`desync_frame` names the tick).
- **Sync test.** `Net.SyncTest(n)` runs offline with the local actions as
  input. Every tick it saves and hashes the state, then restores the state of
  `n` ticks ago and simulates them again, comparing every tick's hash with the
  first run's. A mismatch means some state that gameplay depends on is not in
  the snapshots (or is not deterministic); `desync_frame` is the first tick
  that differed.

## Rollback state

`LuaSnapshot` saves the Lua object graph reachable from the tracked roots
(`Net.Track`, default the global table; a component's functions reach the
global table through `_ENV` anyway). Each slot is one flat registry array
reused between saves: for every reachable table its contents and metatable,
for every Lua closure its upvalues. Loading writes them back in place, so
every table keeps its identity and closures and other tables still refer to
the right objects; tables created after the save simply become unreachable.
`Net.Static` marks tables that never change (converted game data); they are
neither walked nor restored, which is what keeps a save cheap. Not covered:
engine-side state (actors, physics bodies, event subscriptions, particles),
userdata contents (kept as values) and coroutines. Games that use rollback
keep their simulation in Lua and check it with the sync test.

## Determinism requirements

- **Lua string hashing** is seeded with a constant (`luai_makeseed` in
  `thirdparty/CMakeLists.txt`), so `pairs` over string-keyed tables visits keys
  in the same order on every machine.
- **Games** must not read raw keys, the mouse, wall-clock time, `math.random`,
  `Audio.IsPlaying` or other machine-local state in gameplay during a session,
  and must not depend on `pairs` order over tables keyed by tables or
  functions (addresses differ).
- **Floating point**: see the cross-platform section below.

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
  (round trip, typos), STUN messages, and Lua snapshots (in-place restore,
  identity, upvalues, static tables, hashes).
- `tests/netplay_test.py` (CTest `netplay`): two headless runtimes through a
  local room server (punching, forced relay, rollback with corrections), a
  direct host found by LAN discovery, connection codes through a local STUN
  server with rollback, and the sync test (passing with all state tracked,
  failing on state outside the snapshots).
- Games test their own sessions; BleachVsNaruto's `tools/net_test.py` plays
  whole sessions, also over a local UDP proxy with added latency and loss.
