# Phase 13: checked LuaBridge3 production integration

Implementation is delivered at fixed Lua 5.4.9, C++26 and SDL3. **All three final 66-test production matrices pass.** Runtime benchmark variants
are recorded separately; this checkpoint does not claim a universal speedup or
reuse isolated Phase 12 timings as full-engine evidence.

## Delivered implementation

- Production `thirdparty/LuaBridge` now contains LuaBridge3 `3.0-rc12`
  (pre-release), commit `53e031b7df6a14d43f92a54fd792b76dbadcc970`. The upstream
  license and source provenance are retained in `LICENSE.txt` and `UPSTREAM.md`.
  Lua remains 5.4.9 compiled as C; no second VM or language runtime was introduced.
- `LuaBoundary.h/.cpp` owns checked conversions, result checks, protected Lua
  calls and discard-return callbacks. Invalid conversions produce the project's
  `LuaBoundary::Error`; a successful conversion of Lua `false` returns false,
  rather than the success status of a result object. There is no blanket
  `unsafe_cast` adaptation.
- Protected calls preserve the preexisting Lua stack, retain requested results on
  success and restore the base on error. Lifecycle/EventBus callbacks may return
  zero, one or multiple values; their return values remain intentionally ignored.
  Source/traceback text survives the host boundary. Diagnostics escape invalid
  UTF-8/NUL and are bounded to 4096 bytes, including actor-name prefixes.
- Runtime, lifecycle, property access and EventBus use the same error type and
  retain existing error counters/recovery categories. Default userdata copying
  checks Stack get/push results and retains its explicit value whitelist. Native
  lifetime checks, detached preparation and scene-edit transactions use checked
  casts while preserving existing handles, references and commit boundaries.
- Native registrations explicitly distinguish 39 writable member properties,
  existing getter/setter properties and truly readonly properties. Actor/Vector2
  stack reads and manually installed metamethods were adapted without changing
  Lua-visible constructors, receivers or the curated TypeScript host contract.

Main changed areas are `src/engine/scripting/manager`, `EventBus.h/.cpp`,
`src/engine/scripting/luaapi`, native component consumers and
`src/engine/core/SceneEdits.cpp`. Tests now extract checked result values while
retaining their behavioral assertions. No existing regression was weakened to
accept a candidate binding default.

## Real integration finding: keep library throw mode disabled

The full host-contract test exposed a behavior the isolated experiment did not
establish: calling a method with the wrong receiver, for example
`body.SetVelocity(Vector2(1,0))`, allowed LuaBridge's argument checker to throw
`LuaException` outside its registered-call catch scope when the registry-level
exception opt-in was enabled. Lua `pcall` could not catch that C++ exception.

Production therefore **does not call `luabridge::enableExceptions`**. The temporary
`LuaBoundary::EnableExceptions` API and runtime initialization call were removed.
This explicitly supersedes the Phase 12 assumption that opting in and restoring
the panic handler was the production policy.

C++ exception support and `LUABRIDGE_SAFE_LUA_C_EXCEPTION_HANDLING=1` remain enabled.
The library's ordinary argument failures stay Lua errors; checked host operations
and protected-call diagnostics belong to `LuaBoundary`. The host's original panic
handler is never replaced by this boundary. Safe stack checks remain enabled and
strict stack conversions remain at the inspected default of zero; native numeric,
userdata and persistence validators continue to enforce the project contract.

The real `lua_boundary` test covers result-mode policy, disabled components,
callback return tolerance, nonzero stack preservation, error recovery, diagnostic
source/traceback and invalid UTF-8/NUL/oversized messages. It is the new 66th
production test, not one of the four isolated Phase 12 checks.

## Native exception and allocation scope

`luaapi/RawMetamethod.h` guards four manually installed Actor/Vector2 metamethods,
which bypass the library's registration trampoline. Ordinary C++ errors are copied
into fixed storage, fully unwound, then raised with `lua_error`. The C boundary
never retains an owning C++ local across that longjmp. A native `std::bad_alloc`
in these guards aborts without allocating an error string; it is not reported as
recoverable invalid script input.

That abort policy is scoped to these guards. It is not a new native-memory limit
or proof of arbitrary allocation-failure rollback. In agent mode, the existing
supervisor handles worker exit and explicit reset; a graphical process has no
promise of in-process recovery from an abort. Lua allocator quotas, sticky failure,
request deadlines and their documented limitations remain unchanged.

## Provenance, tooling and pending gates

There is one documented local change to upstream `detail/LuaHelpers.h`: equalstates
returns immediately for identical non-null state pointers; distinct coroutine/VM
pointers retain the upstream registry comparison. This responds to a sampled
Release assertion cost, without disabling assertions or stack checks. The library
pin/license and patch provenance are in UPSTREAM.md. Upstream C++26 deprecation
findings and unrelated trailing whitespace remain unchanged.

Explicit nil method receivers are an existing flaw: retained 2.7 Release and
migrated Debug workers both crash on Actor/body member and class free-proxy calls.
Supervisor responses require reset. The debug-metatable nil-index case also fails
in both builds (different abort mechanisms). This is not a new migration regression
or a solved boundary. Correct future self guards must preserve legitimate nullable
pointer arguments, including SetParent(nil). Local comparisons are in
build/phase13-api-probes/nil-receiver-summary.json.

The standalone experiment now uses an explicit retained LuaBridge 2.7 include
root (`POCKET_LUABRIDGE2_SOURCE`, defaulting to the ignored Phase 11 header copy).
Its baseline cannot accidentally select the newly migrated production headers.
The old vendor snapshot and candidate/measurement artifacts remain under ignored
`build/`; no routine compiler transcript or timing dump is tracked.

Final evidence on the same-state fast-path sources:

| Gate | State |
| --- | --- |
| Nil receiver behavior | Reproduced in retained baseline and migrated Debug; existing debt, not fixed |
| Production Debug: all 66 tests | 66/66 pass, 41.54 s |
| Production Release: all 66 tests | 66/66 pass, 33.39 s |
| Production ASan/UBSan: all 66 tests | 66/66 pass, 70.42 s; ASAN_OPTIONS=detect_leaks=0 |
| Matched full-runtime benchmark and interpretation | Five three-pair comparisons complete, 30 processes / 120 verified workload rows; full initial/follow-up/attribution data retained |

The single-read `IsEnabled` predicate preserves false/true/nil/table behavior and
nonzero stack state; focused runtime tests pass. The final pointer fast path has
focused state-identity/coroutine/reference-alias checks. The final matrix above was run after both changes, rather than reusing the earlier
passing Debug run. LeakSanitizer is unavailable on this Mac; no leak-sanitizer
success is claimed. Timings here describe test duration, not engine performance.

The final production gates include native validation, ownership/lifetime,
EventBus/key remapping, reload, spawn/edit transactions, budget recovery and
TypeScriptToLua replay/reload parity. See [ADR 007](adr-007-checked-lua-binding.md),
[the experiment plan](binding-upgrade-experiment.md),
[compile inventory](binding-compatibility-inventory.md), and
[status](status.md) for the decision, compatibility evidence and final integration
state. No commit or push was performed for this checkpoint.

## Final runtime evidence and remaining scope

The final old-2.7 versus migrated-fast-path comparison measures native p50 at
1k/10k actors about 19%/24% lower. Lua-loop p50 remains about 1.7%/0.9% higher;
10k ranges overlap. A direct enabled-v3 versus fast-path-v3 comparison improves
Lua p50 about 6–7%, after the earlier one-read change alone failed to remove the
original 9–11% regression. All historical measurements remain visible in
binding-runtime-performance.md. These are headless animation CPU workloads, not
GPU/frame-wide or cross-engine/SOTA results. A single sampled Lua-only profile
under build/phase13-lua-profile guided the change; sampled timings are not used
as benchmark evidence.

Final Release/Metal defense (180 frames) and inventory (two frames) smokes exit
successfully with empty logs. Inspected captures are under build/phase13-captures.
They prove these rendered fixture states, not human controls, audio quality or
complete game migration. The unchanged GLM Repair.lua candidate again passes
16/16 scenarios and 96 checkpoints with the final Release runtime, without another
model call; output is build/agent-authoring/phase13-release-validation.

Source/tests are local and uncommitted. Full matrix logs remain ignored under
build/phase13-*-tests.log; upstream whitespace and C++26 warnings are retained.
Non-vendor diff whitespace checks pass. The explicit nil-self native crash is the
next bounded correctness task; wider gameplay integration, packaging, Lua5.5
assessment and comparable SOTA evaluation remain open.
