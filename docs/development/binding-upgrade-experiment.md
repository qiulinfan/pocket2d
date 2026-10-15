# Lua binding upgrade: isolated experiment

Date: 2026-10-15. **The isolated LuaBridge3 experiment has now executed; see
[binding experiment results](binding-experiment-results.md).** This document
retains its candidate research and experiment design. The production
baseline for this experiment was Lua 5.4.9 (compiled as C), LuaBridge 2.7, C++26 and SDL3.
The subsequent production integration is recorded in [Phase 13](phase13-checkpoint.md).
This is a binding comparison; it does not change the VM or add a JavaScript VM.

## Candidates and evidence

| Candidate | Exact source pin | Why test it |
| --- | --- | --- |
| LuaBridge3 `3.0-rc12` **pre-release** | `53e031b7df6a14d43f92a54fd792b76dbadcc970` | Closest registration/reference model to the current code; explicit conversion/call results, configurable stack checking and error handlers. Lower expected migration scope than replacing every LuaRef with another library's object type. |
| sol2 tag `v3.3.1` | Peeled commit `dca62a0f02bb45f3de296de3ce00b1275eb34c25` | A comparison for protected-call diagnostics and explicit userdata ownership. Its broader API does not itself replace generation handles, isolation, budgets or transactional publication. |

Both pins were verified with read-only `git ls-remote` against the official
repositories. The sol2 annotated tag object is
`8c3fe95166818bf9cf8890fe5568f574b10a7d9e`; use the peeled commit for source.
The official sol2 Releases page currently labels `v3.3.0` latest, while the
`v3.3.1` tag exists and its `version.hpp` still reports `3.2.3`. LuaBridge3's
rc12 header reports `301`. These macros are not reliable provenance identifiers.
Do not substitute a moving branch for either pin.

Sources: [LuaBridge3 release](https://github.com/kunitoki/LuaBridge3/releases/tag/3.0-rc12),
[LuaBridge3 pinned header](https://github.com/kunitoki/LuaBridge3/blob/53e031b7df6a14d43f92a54fd792b76dbadcc970/Source/LuaBridge/LuaBridge.h),
[sol2 releases](https://github.com/ThePhD/sol2/releases),
[sol2 pinned version header](https://github.com/ThePhD/sol2/blob/dca62a0f02bb45f3de296de3ce00b1275eb34c25/include/sol/version.hpp).

## Specific compatibility seams

1. **Writable native fields can silently become readonly.** Current 2.7
   `addProperty(name, &T::field)` defaults to writable. The rc12 two-argument
   overload installs a getter plus readonly setter; writable members require
   `addPropertyReadWrite` or explicit getter/setter. This affects Transform x/y,
   SpriteRenderer fields and engine-written component keys in
   `BuiltinComponentAPI.cpp` / `RigidbodyAPI.cpp`. Preserve the distinction between
   truly readonly resource/state properties and engine-owned fields that native
   construction must still assign. [Pinned registration implementation](https://github.com/kunitoki/LuaBridge3/blob/53e031b7df6a14d43f92a54fd792b76dbadcc970/Source/LuaBridge/detail/Namespace.h)

2. **Call success, conversion and return arity change.** rc12 `cast<T>()` and
   `Stack<T>::get` return `TypeResult<T>`; `LuaRef::operator()` returns a void
   result that checks zero returned values. Current 2.7 calls request one result,
   and Lifecycle/EventBus intentionally discard it. A Lua callback returning a
   value must not silently acquire a new failure policy. `Shared.cpp` also calls
   the removed class-level `LuaException::pcall`; rc12 provides a free `pcall`.
   Adapt these deliberately, not by replacing every cast with unchecked
   dereference or `unsafe_cast`. [Pinned reference implementation](https://github.com/kunitoki/LuaBridge3/blob/53e031b7df6a14d43f92a54fd792b76dbadcc970/Source/LuaBridge/detail/LuaRef.h)

3. **Error propagation needs executable proof.** The rc12 manual's exception
   sections disagree. In pinned `Invoke.h`, normal calls may raise only after
   `enableExceptions`; the explicit-handler path returns an error result instead.
   It pops the Lua error object before returning an error code, so the experiment
   must verify retention of the original diagnostic sentinel and traceback.
   Do not rely on the manual alone or leave existing catch-only EventBus and
   lifecycle sinks unchanged. Current bounded UTF-8 diagnostics and runtime-error
   counters must still receive failures. [Pinned invocation implementation](https://github.com/kunitoki/LuaBridge3/blob/53e031b7df6a14d43f92a54fd792b76dbadcc970/Source/LuaBridge/detail/Invoke.h),
   [manual](https://github.com/kunitoki/LuaBridge3/blob/53e031b7df6a14d43f92a54fd792b76dbadcc970/Manual.md)

4. **Identity and ownership are project contracts.** `ActorAPI.cpp` installs
   `__eq` via LuaBridge's private class-registry key and reads Stack references.
   `Binding.cpp` clones only approved vector/ActorHandle value userdata, preserving
   table aliases/cycles while rejecting owned inventory/body defaults. EventBus
   and reload retain exact function/component references; raw equality must not
   become a metamethod call. InventoryAPI already uses its own checked C API
   userdata boxes with idempotent destruction: changing the binding need not
   rewrite them. For sol2, raw pointers remain non-owning and all references must
   die before their Lua state. [sol2 ownership](https://sol2.readthedocs.io/en/latest/tutorial/ownership.html)

5. **Keep explicit native validators and the existing state allocator.** rc12's
   strict-conversion switch is off by default and, when enabled, rejects integral
   floats for integer Stack conversions. This differs from several current host
   APIs that validate finite, integral numeric values explicitly. Neither that
   switch nor sol2 safety settings replaces Rigidbody/Particle domains, exact
   Vector2 userdata checks, Save's signed-64/NUL rules or Lua-only heap accounting.
   sol2 must attach a `state_view` to the existing state, not create an unbudgeted
   second owner. Retain C++ exceptions and protected boundaries with C-built Lua;
   do not enable C++-Lua propagation assumptions. [LuaBridge3 configuration](https://github.com/kunitoki/LuaBridge3/blob/53e031b7df6a14d43f92a54fd792b76dbadcc970/Source/LuaBridge/detail/Config.h),
   [sol2 safety](https://sol2.readthedocs.io/en/latest/safety.html),
   [sol2 protected calls](https://sol2.readthedocs.io/en/latest/api/protected_function.html),
   [sol2 exceptions](https://sol2.readthedocs.io/en/latest/exceptions.html)

## Executed slice: a LuaBridge3 boundary probe

Start with LuaBridge3 while holding Lua 5.4.9 and the Lua-visible API fixed.
Use separate baseline/candidate executables and include paths; never mix the two
LuaBridge definitions in one binary. Candidate files belong to an isolated
experiment/build area with pinned sources and licenses retained, not the
production `thirdparty/LuaBridge` directory. No generic multi-VM abstraction is
needed. sol2 may run the same small probe as a later comparison, rather than
requiring an engine-wide sol rewrite to answer the first question.

The experiment delivers:

- A compile-only compatibility inventory for actual `ActorAPI.cpp`,
  `RigidbodyAPI.cpp`, `Shared.cpp` and `SceneComponents.cpp`, separating mechanical
  signature failures from changed semantics. Compilation is not runtime proof.
- A standalone probe using the current Lua allocator/hook and representative
  registered value/handle/owned types. Check writable/readonly/engine-owned
  properties; raw equality and invalid generations; value copy versus ownership;
  destructor-once behavior; GC/VM shutdown order; nested callbacks; callbacks
  returning zero/one/multiple values; native exceptions and Lua errors with a
  nonzero preexisting stack. Include a throwing getter and an error handler that
  must preserve source line, sentinel text and bounded UTF-8 output.
- Before/after measurements for native property access, host calls, Lua callback
  dispatch, clean C++ compilation time and binary size under matching Release
  settings. Report configuration and distributions; no SOTA or frame-budget claim.

A probe passes only when each deliberate adaptation is documented and no error
can be silently ignored. A full dependency migration is a separate decision.
Its acceptance gate is the complete applicable Debug/Release/sanitizer suite,
with these existing tests specifically retained:

| Contract | Actual regression entry points |
| --- | --- |
| Values, handles, ownership and native validation | `lua_vm`, `runtime_actor_lifetime`, `runtime_uid`, `component_isolation`, `lua_host_reentry`, `rigidbody_validation`, `particle_validation`, `agent_native_validation`, `inventory_lua`, `save_lua` |
| Callback identity, errors and publication | `event_bus`, `component_key_rename`, `physics_callback_lifecycle`, `animation_getter_reentry`, `script_reload_candidate`, `script_reload_runtime`, `script_reload_editor`, `agent_reload`, `scene_component_preflight`, `runtime_actor_preparation`, `runtime_actor_spawn`, `scene_edit_runtime`, `editor_command_transaction` |
| Budgets and recovery | `lua_budget`, `agent_budget`, `agent_protocol`, `agent_errors`; retain swallowed-error/coroutine/native-block/OOM/reset cases |
| Existing TypeScript authoring | `script_api_generated`, `script_api_contract`, `script_authoring_tools`, `agent_typed_gameplay` including compiled-bundle reload and Lua/TS replay parity |

TypeScriptToLua is already implemented with TypeScript 6.0.2 and TSTL 1.37.1.
Keep its Lua54 target, colon-call receivers, callable `Vector2(x,y)`, nominal
userdata distinctions and generated curated host contract unchanged. A binding
upgrade is not permission to rewrite compiled fixtures to a library's preferred
constructor or calling convention. Current reload and authoring behavior are
specified in [script-reload](script-reload.md) and
[script-api-contract](script-api-contract.md); older research notes are not the
current implementation contract. Budget boundaries remain in
[script-budgets](script-budgets.md). The isolated results and
[ADR 007](adr-007-checked-lua-binding.md) now define the full integration step.
Production acceptance evidence belongs to [status](status.md), not to the probe.
