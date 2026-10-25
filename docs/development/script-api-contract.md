# Curated script host contract

The existing Lua 5.4 host has one versioned description at
[`api/host-api.json`](../../api/host-api.json). It generates:

- [`api/types/pocket2d.d.ts`](../../api/types/pocket2d.d.ts): ambient TypeScript declarations for optional TypeScriptToLua authoring.
- [`api/types/pocket2d.lua`](../../api/types/pocket2d.lua): LuaLS metadata, never loaded as a component or runtime library.
- [`ScriptAPIContract.h`](../../src/engine/scripting/ScriptAPIContract.h): embedded JSON at `PocketScriptAPI::kContractJson`, used by agent discovery without a runtime file dependency.

Regenerate after editing the source:

```sh
python3 tools/generate_script_api.py
python3 tools/generate_script_api.py --check
```

`--check` fails on missing or stale generated files and does not rewrite them.
`--root PATH` supports isolated checkout/copy checks. All outputs derive from the
same structured types; do not edit declarations or the embedded header directly.
The generator uses Python's standard library and requires neither npm nor a
running engine. It validates type references, identifiers, property access modes
and calling conventions before rendering outputs.

## Scope and representation

Contract version `1` has `coverage="curated"`. It covers Actor lookup, hierarchy,
component access and validity; engine-injected component identity; Transform;
SpriteRenderer; Rigidbody motion; native Vector2 construction and value methods;
Input keys; SpriteAnimation controls, state and event payload; Tilemap cells;
Save result/value shapes; and Debug.Log. It does not claim complete binding
coverage, JavaScript/npm compatibility, a second VM or a generic VM abstraction.

Each entry in `types` has a name and kind. `native` and `component` describe
host-created instances; `record`, `dictionary` and `alias` describe data/type
shapes, not runtime globals. `extends` supplies inherited component fields.
Properties have structured `type` and `access`. Methods have `call="instance"`,
parameters and a return type. Namespace functions use `call="static"` and actual
callable constructors use `call="constructor"`.

Type expressions use named/primitive references or one structured operator:
`{"nullable":T}`, `{"array":T}`, `{"literal":value}` or `{"union":[...]}`.
Nullable results become `T | undefined` in TypeScript and `T|nil` in LuaLS.
`integer` becomes TypeScript `number` and LuaLS `integer`; finite/integer/range
constraints still need runtime validation. No TypeScript declaration here makes
numbers beyond JavaScript's exact integer range safe.

`Actor.GetUID` is deliberately omitted: its uint64 identity must not be presented
as a lossless TypeScript number. Agent observations expose string UIDs separately.
Input mouse position is also omitted because its `glm::vec2` is different native
userdata from `Vector2`/`b2Vec2`. An object with `x` and `y` cannot be passed to a
Rigidbody method that expects native Vector2.

## Self, identity and access

The generated constructor is called as `Vector2(x, y)`, not `new Vector2(...)`.
The only advertised constructor is Vector2. A native class table being present
does not imply it is constructible. Static calls and constructors explicitly use
`this: void`, while each instance method explicitly declares its native receiver
(for example `SetVelocity(this: Rigidbody, value: Vector2)`). Detaching a method
and calling it without its object is therefore a TypeScript error, rather than
deferring a missing-self failure until Lua execution. This follows the
[official TSTL self rules](https://typescripttolua.github.io/docs/the-self-parameter/).

```ts
const owner = Actor.Find("player");
if (owner !== undefined) {
    const body = owner.GetComponent("Rigidbody");
    if (body !== undefined) {
        const velocity = Vector2(1, 0);
        velocity.Normalize();
        body.SetVelocity(velocity);
    }
}
```

The emitted calls are `Actor.Find(...)`, `Vector2(...)`,
`owner:GetComponent(...)`, `velocity:Normalize()` and `body:SetVelocity(...)`.
Lifecycle callbacks retain `self`; a component can declare
`OnAnimationEvent(this: MyComponent, event: AnimationEvent)` or use the generated
`OnAnimationEventCallback<MyComponent>` type. Do not add file-wide `@noSelf`
annotations to gameplay component methods.

Native instance interfaces have distinct readonly `unique symbol` markers. These
exist only during TypeScript checking; there are no corresponding runtime Lua
fields. They prevent structurally similar plain objects from implicitly standing
in for Actor, Vector2 or native components. Type assertions and `any` can bypass
authoring checks and do not make a value safe at runtime.

The source distinguishes three access modes:

| Access | Meaning |
| --- | --- |
| `mutable` | Authoring declarations permit writes. Runtime validation/coercion remains binding-specific. |
| `readonly` | Actual getter-only binding, such as Actor.is_valid or SpriteAnimation.frame. SDK rejects writes. |
| `engine_owned` | Engine manages identity, such as Component.actor/key. SDK rejects author writes, though existing Lua bindings still expose setters. |

The SDK intentionally asks for booleans/numbers/strings even where LuaBridge may
coerce another Lua value. It does not claim that every native parameter has strict
runtime type validation. LuaLS metadata documents access modes in field comments;
it is not a runtime guard or proof of equivalent TypeScript diagnostics. LuaLS
annotation forms follow its [official documentation](https://luals.github.io/wiki/annotations/).

Known native string-literal lookups such as `GetComponent("Rigidbody")` infer
`Rigidbody | undefined`. Custom lookup can use
`GetComponentByKey<MyComponent>("controller")`; that generic is an explicit
caller assertion about the selected component, not runtime reflection or
validation. Lookup results still require a nil check. Cached Actor handles also
need `IsValid()` after lifecycle changes.

## Save typing and validation

`Save.Write` accepts a `SavePayload`: a string-keyed object or array containing
supported recursive scalar/object/array data. `SaveWriteResult` is a discriminated
success/failure union. Cycle, density, finite-number, schema and storage limits
remain runtime checks; a static type cannot prove the whole persistence contract.

`Save.Read` returns `SaveReadResult`; a successful result contains
`schema_version: number` and **`data: unknown`** in TypeScript. It has no generic
that asserts a game schema on behalf of the caller. Check the version and every
required field before applying any gameplay mutation. LuaLS uses `any` with an
explicit validation comment because it has no TypeScript-style `unknown`
narrowing guarantee. See [save-format.md](save-format.md) for supported values and
empty-array/table behavior.

## Evidence and limits

The source was checked against `ActorAPI.cpp`, `BuiltinComponentAPI.cpp`,
`PhysicsAPI.cpp`, `InputAPI.cpp`, `SaveAPI.cpp`, `ApplicationAPI.cpp` and their
native headers. Representative compilation with pinned TypeScript 6.0.2 and
TypeScriptToLua 1.37.1 verified static/instance/constructor call emission. Negative
typing checks produced real errors for nullable Actor access, plain-object
Vector2 arguments, detached instance calls, readonly animation writes, wrong scalar types, missing GetUID
and unvalidated Save.Read data. Isolated generation checks detect stale outputs
and invalid type references; the embedded header compiles as C++26.

These checks establish declarations and generated call shape. The separate
runtime contract test checks actual host names and representative behavior; the
gameplay replay compares compiled TS with a handwritten Lua reference. Final
results are recorded in [status.md](status.md). LuaLS server diagnostics, full
host API parity, runtime source-map traces, hot reload, unrestricted npm code and
cross-platform production readiness are not established by this slice.
