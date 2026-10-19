# Netplay

`Net` runs a two-player session on a deterministic game: both machines run
the same game and exchange only the players' action values each tick
([design and deployment](../development/netplay.md)).

```lua
Net.Configure{local_prefix = "p1_", sides = {"p1_", "p2_"}, shared = {"confirm", "back"},
              mode = "rollback", delay = 0, game = "my_game"}
Net.Track(self)                       -- rollback: the state to save (default: the global table)
Net.Static(LEVEL_DATA)                -- data that never changes, left out of snapshots
Net.HostDirect(0, "mode=team")        -- LAN: other machines find it with Net.Discover
```

## Configuration and sessions

| Function | Meaning |
| --- | --- |
| `Net.Configure{...}` | `local_prefix`: the local player's bindings (default `"p1_"`). `sides`: the action prefixes of side 1 (host) and side 2 (guest). `shared`: full action names both players drive. `mode`: `"lockstep"` (default) or `"rollback"`. `delay`: input delay in ticks, 1..15, or 0 to pick it from the measured round trip. `max_rollback`: how far rollback may run ahead of the peer's input (default 8). `compare_state`: also compare hashes of the whole tracked state with the peer (only when it holds nothing machine-local). `force_relay`: always relay through the room server. `game`: the id LAN discovery matches. `name`: the host's name in discovery (default: the computer's name). `stun`: STUN servers for connection codes. `discovery_port` (default 27779). |
| `Net.Host(server, payload)` | Opens a room on the room server (`"host[:port]"`, default port 27777). `payload` (up to 1000 bytes) reaches the guest with the session. |
| `Net.Join(server, room)` | Joins the room with that code. |
| `Net.HostDirect(port, payload)` | Listens for a direct connection (port 0: 27778) and answers LAN discovery. |
| `Net.JoinDirect(address)` | Connects to `"ip[:port]"` (default port 27778): a LAN or virtual LAN address. |
| `Net.CodeHost(payload)` | Learns this machine's public address; `Net.Status().code` is the code to give the guest. |
| `Net.CodeJoin(code)` | Connects to the host's code; `Net.Status().code` is this side's code for the host. |
| `Net.CodeConnect(code)` | The host enters the guest's code (needed unless the guest got through on its own). |
| `Net.SyncTest(frames)` | An offline session that rolls back `frames` ticks every tick and compares the resimulated state; `desync` reports state the snapshots miss. Inputs are the local actions. |
| `Net.Leave()` | Ends the session or leaves the room; the peer sees `disconnected`. |
| `Net.Running()` | True while the session runs. |
| `Net.IsResimulating()` | True while rollback simulates a tick again (skip effects that should happen once). |
| `Net.Status()` | A table: `state` (`idle`, `server`, `waiting`, `connecting`, `running`, `disconnected`, `error`), `side` (1 host, 2 guest), `link` (`server`, `direct`, `code`, `synctest`), `mode`, `room`, `code`, `nat` (`cone`, `symmetric`, `unknown`), `error`, `payload`, `seed`, `ping` (ms, -1 unknown), `delay`, `relay`, `frame` (the tick being simulated), `stalls`, `waits` (rollback time sync), `rollbacks`, `resimulated`, `max_rollback_seen`, `slowest_rollback_ms`, `desync`, `desync_frame`, `peer`, `lan` (a direct host's address). |
| `Net.Checksum(frame, value)` | Reports a 32-bit hash of the game state at a tick; a mismatch with the peer's sets `desync`. With rollback only final ticks are compared. |

The functions return true when the request started; failures leave the reason
in `Net.Status().error`.

## Finding the other machine

| Function | Meaning |
| --- | --- |
| `Net.Discover(on)` | Starts (or stops) looking for direct hosts with the configured `game` on the LAN. |
| `Net.Hosts()` | `{{address, name, payload, age}, ...}`: hosts heard in the last 3.5 s; pass `address` to `Net.JoinDirect`. |
| `Net.LocalAddresses()` | `{{name, ip, kind}, ...}`: this machine's IPv4 interfaces; `kind` is `lan`, `tailscale`, `zerotier` or `radmin`. |

## Rollback state

| Function | Meaning |
| --- | --- |
| `Net.Track(root, ...)` | The roots of the rollback state: everything reachable from them is saved before each tick and restored in place when a prediction was wrong. Without arguments: the global table. |
| `Net.Static(table, ...)` | Tables that never change (game data). They are left out of snapshots; changing one breaks rollback. |
| `Net.Snapshot(slot, what)` | By hand (tests): `"save"`, `"load"`, `"hash"` or `"objects"`. |

While `Net.Running()` is true, `Input.GetAction*` for the synced actions return
the session's values on both machines (side prefix + the player's input), and
every other action reads as released. The first update that sees
`Net.Running()` is tick 0 on both machines: reset the game there and seed its
random numbers with `Net.Status().seed`. Gameplay must stay deterministic (no
raw keys, mouse, wall-clock time or `math.random`). With rollback, keep the
simulation in Lua tables reachable from the tracked roots, and run
`Net.SyncTest` to check it. Call `Audio.Play` from the simulation as usual:
a resimulated tick does not repeat a sound it already played, a sound that
first appears in a correction plays late, and a mispredicted one fades out
(see the development notes on netplay).
