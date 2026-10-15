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

Construction does not call function-valued `__index` getters. Such a getter
cannot synthesize membership in the core lifecycle callback cache. Define these
callbacks directly on the component or on a statically inherited prototype.
Dispatch of discovered callbacks still uses protected runtime lookup. Dynamic
data getters and protected runtime lookup of `OnAnimationEvent` remain supported.

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
