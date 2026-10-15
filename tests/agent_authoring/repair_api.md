# Pocket2D Lua API needed for Repair

Each component script defines a global table matching its filename, e.g.
`Repair = { OnStart = function(self) ... end, OnUpdate = function(self) ... end }`.
The runtime creates an instance with `self.actor`, `self.key`, and `self.enabled`.
OnStart runs once before that instance's first OnUpdate. A simulation step is
1/60 second. The same lifecycle is used in headless and graphical runs.

- `self.actor:GetComponent("Rigidbody")`: returns that actor's native body.
- `body:GetPosition()`: returns Vector2 with readable numeric `.x` and `.y`.
- `Actor.Find("altar")` / `Actor.Find("village")`: returns matching actor handle,
  or nil if absent. A valid handle supports `:GetComponent("TypeName")`.
- `Actor.Find("village"):GetComponent("Village")`: Lua component table. Its
  `health`, `gathered`, and `state` are regular mutable fields shared with gameplay.
- `Input.GetKeyDown("e")`: true only on the first simulation tick of a new press.
- `Input.GetKey("e")`: true on every tick while held; this is not an edge.
- Standard Lua `math`, local variables, tables and functions are available.
- Use colon syntax for actor/body methods; Input functions use dot syntax.

The project file `scenes/defense.scene` is JSON. Actors have numeric `uid`, `name`,
and a `components` object keyed by component key. Each component has `type` plus
initial scalar properties. Body and Transform starting x/y must match. The
player's new component is already attached as `"repair":{"type":"Repair"}`.

The agent host accepts one JSON command per line and emits exactly one JSON
response per command. `observe` does not advance. `step` accepts `frames` (1-3600)
and `keys`, the full held set. Omitted keys retain held state; [] releases keys.
Key-down occurs once on the first tick after the held set gains the key.
`reset` restores the initial project state; `quit` exits. Failures have `ok:false`
and `error`; normal diagnostics go to stderr.

Successful snapshots include `frame`, `actors`. Each actor includes `uid`, `name`,
`position` when available, and `components` with `key`, `type`, `properties`.
Read `Village` properties to verify health and gathered before/after each action.
This is a real engine process, not a Python mock of Lua behavior.
