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
| `Physics.MoveCharacter(body, delta)` | Moves a kinematic Rigidbody by up to `delta`, sliding along colliders its mask accepts; returns the applied movement |
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
