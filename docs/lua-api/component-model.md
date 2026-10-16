# Component Model

pocket2d gameplay code is built around Lua component tables.

## Declaring a component type

Each component type lives in the project's `component_types/*.lua` and is declared
as a global table whose name matches the file name.

```lua
Mover = {
    speed = 4.0,
    target_names = {},
}
```

## Instance fields automatically provided by the engine

Every live component instance receives:

- `self.actor`
- `self.key`
- `self.enabled`

`actor` and `key` identify the component's engine binding. Scene/template
overrides and the host property-edit APIs reject writes to these fields; binding
and component renaming use dedicated engine operations. This protection does not
make arbitrary direct Lua field assignments read-only or turn trusted gameplay
scripts into a sandbox.

The instance owns its default state. Methods are looked up through the component
type table and share their function objects. Scene/template overrides are applied
after defaults are copied.

Mutable Lua default tables are copied per instance, including nested tables,
keys, metatables, aliases and cycles. `Vector2`, input `vec2`, and immutable actor
handles have explicit value-copy support. Other native userdata, light userdata
and coroutines cannot be safely copied generically; construct them in `OnStart`.
An unsupported default produces a component initialization error.

Before any defaults are cloned, the engine inspects the copied data graph,
including table keys, values and metatables. A data table whose metatable has a
non-nil `__gc`, `__close` or `__mode` is rejected. This prevents a rejected
component's temporary defaults from later running finalizers or changing weak
table behavior. Construct resource-owning or finalizable state in `OnStart`.
Ordinary data metatables, including static `__index` tables, remain supported;
shared functions, their upvalues and the original prototype's lifetime are not
cloned.

Static prototype inheritance through a table-valued metatable `__index` is
included, with derived fields/methods taking precedence. Function-valued
`__index` remains dynamic code: it is not evaluated to discover defaults or to
copy closure state.

A table stored in a prototype default becomes instance-owned even if it was
originally obtained from a global. For intentionally shared state, access a
global directly from a method or explicitly use a closure upvalue. Shared
functions retain their original upvalues; function code is not cloned.

## Lifecycle callbacks

During scene construction and `AddComponent`, the engine discovers these eight
core callbacks through raw fields and static, table-valued `__index` chains:

- `OnStart()`
- `OnUpdate()`
- `OnLateUpdate()`
- `OnDestroy()`
- `OnCollisionEnter(collision)`
- `OnCollisionExit(collision)`
- `OnTriggerEnter(collision)`
- `OnTriggerExit(collision)`

`OnAwake()` is found the same static way and runs once per component as soon
as it is published: `Actor.Instantiate` and `actor:AddComponent` return
components whose `OnAwake` already ran, and every component of a loaded scene
wakes before any `OnStart`. `OnStart` still runs at the next frame start (like
Unity's `Awake`/`Start`), so initialize what spawners read in `OnAwake`.
Within one frame-start batch the built-in components (Rigidbody, ...) start
before any Lua `OnStart`, so an `OnStart` can query every body and collider of
its scene; a body that an `OnStart` enables still starts in the same batch.
Bodies of actors instantiated during that `OnStart` start in the next batch,
which is why a spawner may still set their `x`/`y` fields after `Instantiate`.

Construction does not call function-valued `__index` getters. Such a getter
cannot synthesize membership in the core lifecycle callback cache. Define these
callbacks directly on the component or on a statically inherited prototype.
Dispatch of discovered callbacks still uses protected runtime lookup. Dynamic
data getters and protected runtime lookup of `OnAnimationEvent` remain supported.

### Agent observations

A component may define `OnAgentObserve(self)` returning a table (or any
value) that summarizes it for agents, for example
`return {health = self.health, inventory = self.bag_summary}`. Agents receive it
when they request `game: true` observations. It must only read state.

### Batched updates

A component type may define `OnUpdateBatch(instances)` on its type table
instead of a per-instance `OnUpdate`. Each frame, at the update-order position
of the type's first started instance, the engine calls it once with an array
of that type's live, enabled, started instances in update order. Instances
are collected when the call starts; the array is reused between frames, so do
not keep it. A Lua error stops the whole batch and is reported once as
`Type.OnUpdateBatch`. This removes the per-instance protected call: the
bunnymark's 50,000 movers spend about 7% less main-thread time
(`bunnymark_benchmark ... batch`). Use it for many simple instances; keep
`OnUpdate` when instances must interleave with other components exactly.

```lua
Mover = {
    vx = 1,
    OnUpdateBatch = function(movers)
        for i = 1, #movers do
            local self = movers[i]
            self.transform:SetLocalPosition(self.transform.x + self.vx / 60, self.transform.y)
        end
    end
}
```

`Transform:SetLocalPosition(x, y)` sets both local coordinates with one call,
about a fifth cheaper than two property writes for per-frame movement.

An initially disabled component keeps its `OnStart` pending until a frame-start
boundary observes it enabled. Startup occurs once; disabling and reenabling an
already-started component does not repeat it. Components enabled after that
boundary begin on a subsequent tick, before their update callbacks.

## Authoring defaults vs runtime overrides

A component property can come from three places:

1. the Lua component type default
2. scene/template overrides stored in JSON
3. runtime edits applied to the live instance

pocket2d now uses the same array-aware property writer for both:

- scene/template override application
- live runtime property editing

so arrays and scalars stay consistent across editor and runtime.

## Arrays

Arrays must stay homogeneous:

- `bool[]`
- `int[]`
- `double[]`
- `string[]`

Good:

```lua
frames = {"sprite://images/Run.png?row=1&column=1",
          "sprite://images/Run.png?row=1&column=2"}
```

Not supported as JSON-serialized component properties:

- mixed-type tables
- nested tables as property values
- map-like tables as serialized component properties

This serialization restriction does not prohibit nested Lua tables in runtime
component defaults. Those tables participate in instance isolation but are not
automatically included in scene files or agent observations.

## Built-in component names

These component types are built into the engine and can be added from Lua or
the editor:

- `Transform`
- `SpriteRenderer`
- `Rigidbody`
- `ParticleSystem`

## Native numeric inputs

Rigidbody and ParticleSystem numeric properties reject invalid types and unsupported
numeric values before changing the stored value. Rigidbody logical flags and enums
also require their declared Lua types. Raw configuration fields and immediate native
methods have different effects: changing configuration does not generally rebuild a
live body or resnapshot an emitter. Finish coherent multi-field configuration before
initialization or emission. See [runtime validation](../development/native-runtime-validation.md)
for supported domains, particle capacity, preserved color/unset behavior and limits.

`Rigidbody.enabled` and `body_type` have explicit live behavior: changed values
invalidate hierarchy decisions and are applied at the next safe physics boundary.
Disabling suspends Box2D participation; reenabling retains the existing body.
`SetVelocity` before the first native startup remains a no-op. Direct Rigidbody
or Tilemap destruction from activation/type contact-exit callbacks is rejected;
use deferred `RemoveComponent` or `Actor.Destroy`. See
[rigidbody activation](../development/rigidbody-activation.md).
