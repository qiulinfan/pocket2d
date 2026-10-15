# Phase 14: native instance receiver validation

The scoped receiver patch and two regression suites are delivered and verified
in the final Debug, Release and ASan/UBSan matrix. Lua 5.4.9, C++26, SDL3, production
LuaBridge result mode and existing error/recovery categories remain unchanged.

## Delivered

- `thirdparty/LuaBridge/detail/CFunctions.h` rejects null instance receivers across
  typed/const/member-C methods, class-bound proxies/functors and property paths.
  Typed receiver results finish their lifetime before a Lua error/native callback;
  proxy nil rejection precedes owning argument construction. Raw C callbacks use
  one bound C closure, without an extra Lua call layer.
- `detail/Namespace.h` marks only instance fallback registrations as class-bound.
  Namespace/static functions and ordinary nullable arguments remain supported;
  `Userdata::get` is not globally restricted.
- Raw index/newindex validate receiver shape and required structures. A concrete
  `setmetatable({}, {})` read that still aborted after the first nil fix motivated
  exactly three additional property-get table guards. Existing lookup order,
  valid namespace/static tables and registered-userdata controls are preserved.
- `UPSTREAM.md` records the receiver patch while retaining the earlier independent
  `equalstates` optimization and upstream licensing/provenance.
- New `lua_receiver` and `agent_receiver` targets cover actual registered patterns
  and 35 isolated real-worker cases. No existing assertions were weakened.

[Native receiver contract](native-call-receivers.md) documents the exact entry
paths, original SIGSEGV/SIGABRT evidence, nullable argument distinction, and caught
versus uncaught behavior. An uncaught rejection is an ordinary completed-tick
script error: pre-error script changes remain, later requests can continue, and
no rollback of the tick is claimed. This is not an arbitrary metatable/upvalue
sandbox or full LuaBridge constructor/coroutine/GC feature certification.

## Acceptance evidence

Private current-header generic probes passed, including raw C method/property,
derived/const/nullable and empty-metatable table controls. The isolated structural
reproducer changed from an `__index` SIGABRT to `pcall false` with normal process
exit; the already-safe `__newindex` rejection stayed safe. Focused integration runs
are separate from the final complete matrix below.

The configured full suite has **68 tests: 66 core tests and 2 optional TypeScript
authoring/replay tests** when the pinned Node toolchain is available. The final matrix below ran against the frozen receiver changes.

| Final gate | State |
| --- | --- |
| Debug: all 68 tests | 68/68 passed, 51.23 s |
| Release: all 68 tests | 68/68 passed, 42.55 s |
| ASan/UBSan: all 68 tests | 68/68 passed, 84.77 s; `ASAN_OPTIONS=detect_leaks=0` |
| Matched native and Lua-heavy full-runtime benchmark | 12 successful processes; 24 runtime rows and 168 micro samples verified |

Suite durations describe verification work, not engine speed. LeakSanitizer is
unsupported on this macOS host. Final logs are ignored `build/phase14-*-tests.log`.
The last focused Debug run also passed all five receiver/boundary/API/reentry tests
in 10.60 seconds. Both Release/Metal fixture smokes exited successfully with empty
logs: defense at 180 frames and inventory at two frames. Their captures under
`build/phase14-captures` were inspected; human game feel remains unverified.

[Matched performance](native-receiver-performance.md) reports all samples and
limits. All principal p50 ranges overlap; the native-method micro median changes
42.097 to 43.560 ns (+3.5%) with overlapping run ranges. These measurements do not
establish a general speedup, stable material regression, or zero guard cost.
Earlier migration timings remain historical Phase 13 evidence. Routine logs and
measurements remain in ignored `build/`. [Status](status.md) owns the cross-slice
outcome. No commit or push was made. The next gameplay slice is specified in
[two-world integration plan](two-world-integration-plan.md).
