# Netplay

Two players on two computers play one game through deterministic lockstep:
both machines run the same simulation, and each tick only the players' action
values cross the network. Code: `src/engine/net/` (`UdpSocket`, `Netplay`),
the Lua `Net` namespace ([Lua API](../lua-api/netplay.md)) and the room server
`tools/room_server/room_server.py`.

## Lockstep

- **Input.** The local player's input is sampled from the actions named with
  `local_prefix` (by default their `p1_` bindings, so both players use their own
  keyboard's player 1 keys). One byte per button or axis (an axis2d takes two).
  On both machines it becomes the input of that player's side: the host is side
  1 (`p1_` actions), the guest side 2 (`p2_`). `shared` actions (menu confirm,
  pause) combine both players: buttons by OR, axes by sum. While a session runs,
  every action not synced reads as released, so nothing local leaks into the
  simulation.
- **Ticks.** A tick runs only when both players' input for it has arrived.
  Local input is scheduled `delay` ticks ahead, so it has `delay` ticks to cross
  the network before it is needed. `Engine::GameLoop` skips the tick (and keeps
  drawing) while waiting; headless `StepSimulation` waits up to 0.5 s and then
  returns without advancing, so an agent harness stepping two peers in turn can
  never deadlock them.
- **Delay.** `delay = 0` is automatic: before START the host times a few
  round trips (five punch / punch-ack pairs, at most 0.5 s) and picks
  `ceil(rtt / 2 / 16.7 ms) + 1` ticks, clamped to 2..10. The host sends it with
  START. On a LAN that is 2 ticks (33 ms); a 120 ms round trip gives 5 or 6.
- **Reliability.** UDP. Each input packet carries every frame the peer has not
  acknowledged (up to 32), the receiver's acknowledgement, a timestamp echo for
  the ping, and the latest four game checksums. Packets are resent every 30 ms
  even when no tick runs. No packet for 6 s means the connection is lost; a
  BYE (sent three times on `Leave`) ends the session at once.
- **Start.** The host's START carries the session seed (`std::random_device`),
  the delay and the game's payload (up to 1000 bytes: the host's settings). The
  guest starts on START, the host on its acknowledgement or the guest's first
  input. Both peers' first lockstep tick is frame 0 with the same seed and
  payload; the game resets its state there. Peers with different input layouts
  (different `actions.json`) fail with "different game version".
- **Desync detection.** `Net.Checksum(frame, value)` records a hash of the
  game's state; checksums travel with the inputs and a mismatch for the same
  frame sets `desync` and `desync_frame` in the status.

## Determinism requirements

Lockstep only works if the same inputs give the same states on both machines.

- **Lua string hashing** is seeded with a constant
  (`luai_makeseed` in `thirdparty/CMakeLists.txt`) instead of Lua's per-process
  random seed, so `pairs` over string-keyed tables visits keys in the same
  order on both machines.
- **Games** must not read raw keys, the mouse, wall-clock time, `math.random`
  or other machine-local state in gameplay during a session, and must not rely
  on `pairs` order over tables keyed by tables or functions (addresses differ).
  Rendering may do anything.
- **Floating point**: gameplay math should avoid transcendental functions
  (`math.sin`, `math.cos`, `math.atan`, `math.exp`), whose last bits can differ
  between C libraries. Basic arithmetic and `math.sqrt` are IEEE exact. Mac arm64
  and Windows x64 builds both use SSE2/NEON doubles without x87.

## Connecting

- **Room server.** The host sends `PK2 HOST` and gets a four-character room
  code (no O/0/I/1). The guest sends `PK2 JOIN <code>`; the server sends each
  side the other's public and LAN addresses. Both punch to both addresses for
  3 s; if no direct packet arrives, traffic is relayed through the server
  (`PK2R` frames). `force_relay` skips punching. Rooms expire after two minutes
  without traffic. The server sees only the two endpoints, never game data.
  No answer from the server for 8 s fails with "no answer from the room server".
- **LAN.** `Net.HostDirect(port)` listens (default port 27778) and its status
  `lan` names the address to type; `Net.JoinDirect("ip[:port]")` connects.

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
through `POCKET_NET_SERVER` or their own configuration (BleachVsNaruto:
`netplay.json` in the project, `{"server": "VPS_IP:27777"}`).

## Tests

- `tests/netplay_test.py` (CTest `netplay`): two headless runtimes through a
  local room server with hole punching, with forced relay, and directly; each
  drives its own player with the same local bindings, and both must agree on
  every synced value, edge and shared action; the guest must see the host
  leave.
- Games test their own sessions; BleachVsNaruto's `tools/net_test.py` plays a
  whole session through the fighter select and a fight, also over a local UDP
  proxy with added latency and loss.
