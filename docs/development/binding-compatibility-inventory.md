# LuaBridge3 compile compatibility inventory

Executed 2026-10-15 against the unmodified production translation units below.
**The LuaBridge 2.7 baseline passes; LuaBridge3 rc12 is not a source-compatible
header replacement.** This inventory does not claim that an adapted candidate
has compiled or that candidate runtime behavior has passed.

Candidate: LuaBridge3 pre-release `3.0-rc12`, commit
`53e031b7df6a14d43f92a54fd792b76dbadcc970`, acquired separately by the experiment
owner under `build/dependency-sources`. The inspected include root is its
`Source` directory. Production `thirdparty/LuaBridge`, Lua 5.4.9 and all production
translation units were unchanged by this inventory.

## Reproduce

```sh
python3 tools/binding-experiment/compile_inventory.py \
  --repo . \
  --build build/ninja-debug \
  --candidate build/dependency-sources/LuaBridge3-53e031b7df6a14d43f92a54fd792b76dbadcc970/Source \
  --output build/phase12-compatibility
```

The reusable script reads the four exact entries from `compile_commands.json`,
parses their argv without a shell, removes object/dependency-output flags, and
runs syntax-only compilation. It preserves the configured C++ standard, platform,
include paths and definitions. Candidate `-I` is inserted before all existing
include options. Separate `-E -H` preprocessing verifies both the main LuaBridge
header and every observed LuaBridge detail header; a run with mixed or unproven
headers is not a valid compatibility inventory. Compiler transcripts, include
traces, argv and bounded JSON summaries stay in the ignored output directory.

A candidate `#error` is an incompatibility, not an invalid experiment, when its
header provenance is proven and the baseline passes. Exit 0 means the inventory
completed with a passing baseline and verified headers; the JSON field
`candidate_syntax_compatible` reports the separate candidate outcome. Baseline or
header-verification failure returns 2. The script currently supports Clang/GNU
compile databases, requires explicit argv rather than response files, and caps
each compiler invocation at 90 seconds.

## Executed result

Configuration: Apple clang 21.0.0 (`clang-2100.3.34.2`), arm64, C++26, current
`ninja-debug` compile database, syntax-only with the diagnostic error limit removed.
All eight header-resolution checks passed; no baseline/candidate header mixing
was observed. All baseline preprocessing and syntax checks passed with zero
warnings. Candidate diagnostics also contained zero warnings.

| Unmodified production TU | Baseline errors / exit | Candidate errors / exit | Observed incompatibilities |
| --- | --- | --- | --- |
| `src/engine/scripting/luaapi/ActorAPI.cpp` | 0 / 0 | 5 / 1 | One include-order `#error`; four accesses to uid/generation on a result-wrapped reference rather than ActorHandle. |
| `src/engine/scripting/luaapi/RigidbodyAPI.cpp` | 0 / 0 | 3 / 1 | Result-wrapped double/string/bool cannot be returned or assigned as their contained values. |
| `src/engine/scripting/manager/Shared.cpp` | 0 / 0 | 10 / 1 | Two removed `LuaException::pcall` calls; eight pointer/value/string result-conversion errors. |
| `src/engine/scripting/manager/SceneComponents.cpp` | 0 / 0 | 5 / 1 | Native-pointer casts are result objects; existing pointer initialization and member access no longer have the same type. |

The total is **23 compiler diagnostics, not 23 independent defects**. ActorAPI
also fails preprocessing at the candidate's explicit include-order check. The
other three candidate preprocessing invocations succeed. Full diagnostics are
local in `build/phase12-compatibility`; no error-hiding shim, forced Lua include,
production edit, or substituted demonstration TU was used.

## Adaptation inventory

1. **Make Lua header order explicit.** `ActorAPI.cpp` includes the private
   `LuaBridge/detail/ClassInfo.h` before `lua.hpp`; rc12's Config enforces the Lua
   header requirement at that earlier include. Move the actual source include
   order in a future migration, rather than mask it with a compiler `-include`.
   The private registry-key use itself did not produce a missing-symbol diagnostic
   in this run; its ActorHandle equality/lifetime behavior still needs testing.
   [Pinned Config](https://github.com/kunitoki/LuaBridge3/blob/53e031b7df6a14d43f92a54fd792b76dbadcc970/Source/LuaBridge/detail/Config.h)

2. **Unwrap checked results at host boundaries.** `cast<T>()` and `Stack<T>::get`
   now return `TypeResult`; reference getters return a wrapped reference.
   Preserve explicit failure reporting, nullable-pointer policy, and exact
   userdata checks before accessing the contained value/reference. In particular,
   `TypeResult<T*>` is not `T*`: merely changing `.` to `->` is not a valid general
   repair. Do not globally replace casts with `unsafe_cast` to make the build pass.
   [Pinned LuaRef](https://github.com/kunitoki/LuaBridge3/blob/53e031b7df6a14d43f92a54fd792b76dbadcc970/Source/LuaBridge/detail/LuaRef.h),
   [pinned result type](https://github.com/kunitoki/LuaBridge3/blob/53e031b7df6a14d43f92a54fd792b76dbadcc970/Source/LuaBridge/detail/Result.h)

3. **Adapt protected calls together with their failure policy.** The candidate
   suggests free `luabridge::pcall` for the two Shared.cpp calls. That syntactic
   change alone does not establish the existing throwing/reporting behavior:
   exception enablement, handler mode, stack restoration and the runtime-error
   sink must be verified. Keep errors bounded and UTF-8 safe, and preserve nested
   callback continuation. [Pinned invocation code](https://github.com/kunitoki/LuaBridge3/blob/53e031b7df6a14d43f92a54fd792b76dbadcc970/Source/LuaBridge/detail/Invoke.h)

## Compile success would still miss these semantics

These are **source-inspected risks, not candidate runtime results**:

- `Shared.cpp::IsComponentEnabled` returns
  `!enabled.isBool() || enabled.cast<bool>()`. A candidate result's contextual
  bool means conversion success, not the stored boolean; successful conversion
  of Lua `false` would therefore make this expression return true.
  This expression did not generate a diagnostic in the candidate run. The
  standalone probe must include enabled=false and invalid-type cases, not only
  compilation of explicit bool assignments.
- Current member-pointer `addProperty(name, &T::field)` is writable in 2.7; rc12's
  two-argument registration is readonly. A migration must explicitly preserve
  writable Transform/SpriteRenderer fields and engine-owned key assignment,
  without making truly readonly animation/map state writable.
- Current lifecycle/EventBus code discards LuaRef call returns. Candidate calls
  have result/error and return-arity semantics that need explicit adaptation;
  successful compilation must not hide a Lua error or change the acceptance of
  callbacks that return values. Anonymous closures, exact callback identity and
  FIFO subscriptions must remain intact through reload and key remapping.

The property and call findings are detailed with official sources in the
[binding experiment plan](binding-upgrade-experiment.md). They are outside what a
syntax-only command can validate.

## Decision and next gate

Do not replace production headers on the strength of API similarity. The isolated
boolean/property/error/ownership probes have subsequently executed; see
[binding experiment results](binding-experiment-results.md). Their passing explicit
adaptations enable the full integration selected in ADR 007, not automatic
production acceptance. Keep Lua 5.4.9, its allocator/hook, native numeric validators,
ActorHandle generations and the existing Lua-visible host contract fixed.

A later adaptation must retain `runtime_actor_lifetime`, `component_isolation`,
`lua_host_reentry`, `event_bus`, `component_key_rename`, native validation,
scene/component preparation and edit-transaction tests. Budget/recovery and
reload tests must still pass. The generated API contract, TypeScriptToLua
receiver/constructor conventions and `agent_typed_gameplay` replay/reload parity
must not be rewritten just to accommodate a binding default. See the experiment
plan for the complete relevant test groups; final integration evidence remains
in [status](status.md).
