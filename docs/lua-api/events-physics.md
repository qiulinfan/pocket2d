# Events and Physics

## `Event`

| Function | Description |
| --- | --- |
| `Event.Publish(event_type, event_object)` | Publishes an event object |
| `Event.Subscribe(event_type, component_ref, function_ref)` | Subscribes a component callback |
| `Event.Unsubscribe(event_type, component_ref, function_ref)` | Removes a subscription |

The event bus is component-oriented: subscriptions are associated with the live
component reference that owns the callback.

## `Vector2`

`Vector2` is the Box2D-facing vector type exposed to Lua.

### Constructor

```lua
local v = Vector2(x, y)
```

### Properties

- `x`
- `y`

### Methods

| Method | Description |
| --- | --- |
| `v:Normalize()` | Normalizes the vector in place |
| `v:Length()` | Returns vector length |

### Static functions

| Function | Description |
| --- | --- |
| `Vector2.Distance(a, b)` | Distance between two vectors |
| `Vector2.Dot(a, b)` | Dot product |

### Operators

- `a + b`
- `a - b`
- `vector * scalar`
- `scalar * vector`

## `Collision`

Collision / trigger callbacks receive a `Collision` object with:

- `other`
- `point`
- `relative_velocity`
- `normal`

## `HitResult`

Physics raycasts return `HitResult` objects with:

- `actor`
- `point`
- `normal`
- `is_trigger`

## `Physics`

| Function | Description |
| --- | --- |
| `Physics.Raycast(position, direction, distance)` | Returns the first hit or `nil` |
| `Physics.RaycastAll(position, direction, distance)` | Returns all hits as a Lua array |
| `Physics.CircleCast(origin, radius, direction, distance)` | Sweeps a circle; returns the nearest collider hit or `nil` |
| `Physics.OverlapCircle(center, radius, include_triggers)` | Actors overlapping the circle, each once, in UID order |
| `Physics.OverlapBox(center, width, height, include_triggers)` | Actors overlapping the axis-aligned box, each once, in UID order |
| `Physics.GetGravity()` / `Physics.SetGravity(vec)` | Reads or changes world gravity (default `(0, 9.8)`) |
| `Physics.MoveCharacter(body, delta)` | Moves a kinematic Rigidbody by up to `delta` against colliders its mask accepts; returns the applied movement. Circle/capsule movers slide along any surface; a box mover moves x then y with square corners (tile platformers), ignoring its rotation. Start movers clear of colliders: a box already overlapping a one-sided tilemap outline does not collide with it |
| `Physics.FindPath(from, to, cell, radius, max_nodes)` | Grid A* (8-way, no corner cutting, at most `max_nodes` expansions) over `cell`-sized cells; static colliders within `radius` of a cell centre and solid tilemap cells block. Margin cells within ceil(radius / cell) + 1 cells of the start or goal still pass, so a walker standing closer to an obstacle than `radius` can step out and a goal beside one is reachable; a step into a margin cell costs four free steps, so paths leave the margin at once instead of running along the obstacle; cells inside colliders or solid tiles never pass. Returns the waypoints after the start (ending exactly at `to`), or the path to the cell nearest the goal when it is not reached |
| `Physics.PathRegions(points, cell, radius, max_nodes)` | Labels each point's walkable region under FindPath's cell rules (clearance cells pass within the margin of any of the points): one 8-way flood per region, each expanding at most `max_nodes` cells. Returns 1-based integers in order of first appearance; equal labels are connected. Cheaper than a FindPath per pair, whose unreachable goals flood the whole region. A flood stopped by the limit can split a region, never join two |
| `Physics.ConnectDistance(a, b, anchor_a, anchor_b)` | Distance joint between two world anchors |
| `Physics.ConnectRevolute(a, b, anchor)` | Hinge at a world anchor |
| `Physics.ConnectWeld(a, b, anchor)` | Rigid (or springy) weld |
| `Physics.ConnectPrismatic(a, b, anchor, axis)` | Slider along a world axis |
| `Physics.ConnectWheel(a, b, anchor, axis)` | Wheel with suspension along a world axis |
| `Physics.ConnectMotor(a, b)` | Drives `b` toward its current offset from `a` |

Joints need two different, started Rigidbodies.

## `Joint`

| Method | Description |
| --- | --- |
| `IsValid()` | `false` after `Destroy()` or once either body is destroyed |
| `Destroy()` | Removes the joint |
| `GetKind()` | `distance`, `revolute`, `weld`, `prismatic`, `wheel` or `motor` |
| `SetMotor(enabled, speed, max_force)` | Revolute/wheel speed in degrees per second, distance/prismatic in meters per second; motor joints use `max_force` only; welds have no motor |
| `SetLimits(enabled, lower, upper)` | Revolute angle limits in degrees; distance length or prismatic/wheel translation in meters |
| `SetSpring(enabled, hertz, damping_ratio)` | Spring softness for distance, weld, prismatic, revolute and wheel joints |
