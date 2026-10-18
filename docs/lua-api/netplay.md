# Netplay

`Net` runs a two-player lockstep session: both machines run the same game and
exchange only the players' action values each tick
([design and deployment](../development/netplay.md)).

```lua
Net.Configure{local_prefix = "p1_", sides = {"p1_", "p2_"}, shared = {"confirm", "back"}, delay = 0}
Net.Host("203.0.113.5:27777", "mode=team")   -- then show Net.Status().room
-- the other machine:
Net.Join("203.0.113.5:27777", "K86R")
```

| Function | Meaning |
| --- | --- |
| `Net.Configure{local_prefix, sides, shared, delay, force_relay}` | `local_prefix`: the local player's bindings (default `"p1_"`). `sides`: the action prefixes of side 1 (host) and side 2 (guest). `shared`: full action names both players drive. `delay`: input delay in ticks, 1..15, or 0 to pick it from the measured round trip. `force_relay`: always relay through the room server. |
| `Net.Host(server, payload)` | Opens a room on the room server (`"host[:port]"`, default port 27777). `payload` (up to 1000 bytes) reaches the guest with the session. |
| `Net.Join(server, room)` | Joins the room with that code. |
| `Net.HostDirect(port, payload)` | Listens on the LAN (port 0: 27778). |
| `Net.JoinDirect(address)` | Connects to `"ip[:port]"` (default port 27778). |
| `Net.Leave()` | Ends the session or leaves the room; the peer sees `disconnected`. |
| `Net.Running()` | True while lockstep runs. |
| `Net.Status()` | A table: `state` (`idle`, `server`, `waiting`, `connecting`, `running`, `disconnected`, `error`), `side` (1 host, 2 guest), `room`, `error`, `payload`, `seed`, `ping` (ms, -1 unknown), `delay`, `relay`, `frame` (lockstep ticks run), `stalls`, `desync`, `desync_frame`, `peer`, `lan` (LAN host: the address to type). |
| `Net.Checksum(frame, value)` | Reports a 32-bit hash of the game state at a lockstep frame; a mismatch with the peer's sets `desync`. |

The functions return true when the request started; failures leave the reason
in `Net.Status().error`.

While `Net.Running()` is true, `Input.GetAction*` for the synced actions return
the session's values on both machines (side prefix + the player's input), and
every other action reads as released. The first update that sees
`Net.Running()` is lockstep frame 0 on both machines: reset the game there and
seed its random numbers with `Net.Status().seed`. Gameplay must stay
deterministic (no raw keys, mouse, wall-clock time or `math.random`).
