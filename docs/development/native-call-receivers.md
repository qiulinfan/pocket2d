# Native instance receiver contract

Pocket2D uses LuaBridge3 rc12 with project patches recorded in
[`thirdparty/LuaBridge/UPSTREAM.md`](../../thirdparty/LuaBridge/UPSTREAM.md).
Lua remains 5.4.9 compiled as C. This document describes native instance-call
receivers and the tested raw index/newindex boundaries; it does not certify every
LuaBridge feature or make trusted local Lua a sandbox.

## Receiver versus ordinary argument

An instance method/property needs a non-null receiver of an accepted registered
type, including supported derived/const instances. Nil or missing self must raise
a Lua error before the native method, getter, setter or user functor executes.
Ordinary wrong userdata/table receivers likewise fail rather than changing the
native object. The error is catchable by Lua `pcall`.

```lua
local body = actor:GetComponent("Rigidbody")
body:SetVelocity(Vector2(1, 0))                  -- valid receiver
assert(not pcall(body.SetVelocity, nil, Vector2(1, 0)))
assert(not pcall(body.SetVelocity, Vector2(1, 0))) -- detached call lost self
actor:SetParent(nil)                            -- valid self; nullable parent
```

Nil is not globally forbidden. Namespace/static functions and ordinary nullable
pointer parameters retain their conversion rules. In particular, `SetParent(nil)`
and the generic test's nullable second parameter continue to work. `Userdata::get`
has not been changed into a globally non-null conversion function.

## Implemented entry paths

The local patch covers instance member and const-member functions, member Lua C
functions, class-bound free proxies/functors, and direct/member/proxy/functor/raw-C
property getters/setters. Instance index/newindex fallback registrations carry
class context; namespace and static fallback registrations do not.

Typed receiver paths decode once. A short scope extracts the pointer or static
error message, destroys the non-trivial `TypeResult`, and only then raises a Lua
error or invokes native code. This matters with C-built Lua: a longjmp must not
skip that result object's destructor. Class proxy/functor nil checks occur before
argument tuples and their owning strings/containers are constructed.

Class-bound raw C callbacks use a bound C closure with receiver checking and the
configured C++ exception guard. They call the native function directly; there is
no additional `lua_call` wrapper layer. Normal typed receiver paths reuse the
existing decode and add the null-pointer check, rather than decoding a second
copy of the instance.

Production stays in **result mode**: it does not enable LuaBridge's registry-level
exception opt-in. Argument failures must remain Lua errors catchable by `pcall`;
checked host conversions/calls use `LuaBoundary`. C++ exception support and
`LUABRIDGE_SAFE_LUA_C_EXCEPTION_HANDLING=1` remain enabled. The Phase 13 panic,
budget, bounded diagnostic and native-allocation policies are unchanged.

## Raw index/newindex functions

Lua can obtain a real binding metatable and call its `__index`/`__newindex`
function explicitly. These entry points now reject invalid receiver shapes,
missing metatables and missing property-lookup tables with Lua errors before
assertions or invalid table access.

One reproduced case was ordinary Lua data, not forged userdata:

```lua
local meta = debug.getmetatable(body)
local unmanaged = setmetatable({}, {})
assert(not pcall(meta.__index, unmanaged, "x"))
assert(not pcall(meta.__newindex, unmanaged, "x", 9))
```

Before the structural guard, the first operation aborted at the property-get
metadata assertion; the setter already returned an ordinary error. Three existing
property-table assertion sites now use checked error branches. Their lookup order
is retained. A valid namespace/class table or registered userdata is not rejected
merely because the generic metadata function came from another registration.
Tests preserve namespace absent-field lookup, valid Vector2/native field access,
static properties and existing registered-other-userdata behavior. This is not a
rule that all Lua tables are valid instance-method receivers.

Original real-worker evidence also reproduced nil Actor/native method calls
terminating with SIGSEGV and malformed raw metadata receiver shapes terminating
with SIGABRT. The patch converts the scoped cases into script errors; it does not
claim protection against arbitrary metatable/upvalue replacement, forged userdata,
or every constructor/destructor/coroutine/GC extension supported by LuaBridge.

## Caught and uncaught errors

With Lua `pcall`, rejected self does not enter the native body. The regression
checks existing actor/component identity, body configuration, velocity/position,
health and startup counts; later valid calls and normal ticks continue. Reset
replays the fresh scene and the same rejection behavior.

If the script does not catch the error, the established lifecycle boundary reports
an ordinary script failure. The real-worker test requests three frames, receives
`ok:false` at completed frame 1 without `requires_reset:true`, can still observe
that world, and completes a subsequent request without resetting. Existing ordinary
callback errors need not carry a `code` field; this patch does not invent a new
wire category. A source-bearing error message remains available.

This is **not tick rollback**. Lua assignments and counters executed before the
failed call remain changed; statements after it are not executed. The rejected
native operation itself has no side effect. Process crashes, quota exhaustion,
post-publication host failures and their separate reset policies are not redefined.

## Verification

- `lua_receiver_test.cpp` registers actual classes and binding patterns. It checks
  nil/missing/wrong receivers, native-call counters, unchanged fields, nonzero Lua
  stack preservation, nullable arguments, derived/const dispatch, raw C methods
  and properties, namespace/static controls and the empty-metatable table case.
- `agent_receiver_test.py` contains **35 isolated real-worker cases**, including
  caught and uncaught paths, later valid movement and reset/replay. It uses the
  actual agent protocol and temporary projects, without an eval endpoint or mocks.

Run the focused cases with:

```sh
ctest --test-dir build/sdl3-debug \
  -R '^(lua_receiver|agent_receiver)$' --output-on-failure
```

Final Debug/Release/sanitizer and performance evidence is owned by the integration
run and recorded in [phase14-checkpoint](phase14-checkpoint.md) / [status](status.md).
Source/diagnostic expectations are behavioral assertions, not timing thresholds.
