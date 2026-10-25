# Phase 12: executed Lua binding experiment

Executed 2026-10-15. The isolated baseline/candidate experiment is complete;
at that checkpoint production remained Lua 5.4.9 + LuaBridge 2.7. No production source, game fixture,
public API declaration or vendored dependency was changed in this phase. The
Pocket2D modernization and Village_Rim migration goal remains active. The subsequent
production migration and its corrected result-mode policy are now verified in
[Phase 13](checkpoints/phase13-checkpoint.md); the opt-in probe below is historical evidence,
not the current initialization policy.

The candidate is LuaBridge3 prerelease 3.0-rc12 at
`53e031b7df6a14d43f92a54fd792b76dbadcc970`. Its official source and license remain
under ignored build/dependency-sources. sol2 was researched but not acquired or
executed. See the [candidate research](binding-upgrade-experiment.md) for official
source links and the [real-TU inventory](binding-compatibility-inventory.md).

## What was executed

The standalone CMake project in tools/binding-experiment compiles the actual
vendored Lua 5.4.9 sources as C and the production ScriptBudget.cpp. It builds
separate baseline/candidate executables; no binary mixes the two LuaBridge
versions and no copy of the gameplay runtime is represented as the real engine.
Representative native values, handles and ownership probes exercise each actual
binding implementation. Their local adapters are explicitly marked experiments.

Candidate configuration: C++26, exceptions enabled, SAFE_LUA_C_EXCEPTION_HANDLING=1,
SAFE_STACK_CHECKS=1 and STRICT_STACK_CONVERSIONS=0. Existing explicit native
validators remain necessary. The evaluated candidate does not silently acquire
strict integer-only or noncoercing conversions. Both configurations retain the
project's allocator, instruction hook and aborting panic policy.

| Verification | Result |
| --- | --- |
| Four real unmodified production TUs, baseline syntax | 4/4 pass, zero warnings |
| Same four TUs, candidate syntax | 0/4 pass, 23 diagnostics; expected evidence of incompatibility |
| Header resolution for all eight TU/variant pairs | Correct root and detail headers; no mixed versions |
| Isolated Debug CTest | 4/4 pass, 0.48 s |
| Isolated Release CTest | 4/4 pass, 0.34 s |
| Isolated ASan/UBSan CTest | 4/4 pass, 0.63 s, ASAN_OPTIONS=detect_leaks=0 |

These four isolated cases are **not** four migrated production-engine tests. The
production 65-case matrix remains the unchanged Phase 11 checkpoint; it was not
rerun merely for adding independent experiment tooling. LeakSanitizer is unavailable
on this Mac. Candidate Expected.h emits C++26 is_trivial_v deprecation warnings;
no vendor patch or warning-to-error suppression was added to conceal them.

## Observed semantics and deliberate adaptations

- Current two-argument member property registration permits writes. Candidate
  registration makes it readonly; explicit addPropertyReadWrite preserves native
  fields and engine key injection while getter-only fields remain readonly.
- Candidate casts return TypeResult. In the actual IsComponentEnabled expression,
  a successful conversion of Lua false becomes contextual success/true unless
  explicitly unwrapped. Checked extraction restores false/true behavior. Wrong
  Lua types still need the project's declared-type guards.
- Baseline LuaRef calls accept zero, one or multiple return values and retain only
  the first. Candidate operator() expects no return values and reports a failed
  result when a callback returns one or more. An explicit protected host call
  with nresults=0 preserves gameplay's discard-return contract. Typed candidate
  tuple calls work when callers actually request multiple values.
- Baseline callback failure throws by default and leaves an error value for the
  host's stack scope. Candidate default failure returns a generic error category,
  does not throw, and restores the stack. Keeping only the old catch blocks would
  lose error accounting and original diagnostics. Explicit exception opt-in plus
  a host reporting boundary preserves nested callback errors and sentinels.
- enableExceptions replaces the allocator owner's panic callback in both
  libraries. The probes restore the original panic after opt-in/registration.
  This must be part of initialization adaptation, not assumed from a successful
  compile. Native raw C-function exception handling uses candidate's enabled safe
  wrapper; baseline uses an explicit catch before lua_error, with no C++ locals
  crossing a Lua longjmp.
- Raw reference equality does not invoke user metamethods. Value copies, borrowed
  pointers, representative UID/generation handles, stale-handle rejection,
  nested C++ scope unwinding and exactly-once owned/borrowed destruction pass.
  LuaRefs die before VM close and tracked Lua allocations return to zero.
- The real allocator/hook tests preserve nested request budgets, coroutine charges,
  sticky instruction/heap failures even when Lua catches errors, and close/new-state
  recovery. This does not establish a process-memory cap, blocking-native timeout,
  sandbox or entire-engine OOM rollback.
- Protected getter/callback tests retain a nonzero outer stack and real source
  lines, tracebacks and sentinels. A clearly local host adapter escapes invalid
  UTF-8/NUL before the C-string traceback interface and caps emitted text at 4096
  bytes without splitting codepoints; the suffix after an embedded NUL survives.
  That diagnostic policy is demonstrated, not yet installed in production.

## Matched local cost measurements

Apple clang 21.0.0, arm64 macOS, Release -O3, matching Lua and ScriptBudget objects.
Each process performs three warmups and seven measured samples of 200,000
operations per workload. Three interleaved baseline/candidate runs yield 21
samples per row. No compiler or other probe was running concurrently. Every
sample verifies its final counter/native value and stack depth outside timing.
Instruction accounting is disabled for this cost probe, matching trusted-mode
configuration; it still uses the actual allocator. This is a binding workload,
not a rendered frame, gameplay replay, whole-engine ranking or SOTA comparison.

| Workload | Baseline median ns/op | Candidate median ns/op | Baseline / candidate p95 ns/op |
| --- | ---: | ---: | ---: |
| Lua native property read + write pair | 100.225 | 74.589 | 102.284 / 77.741 |
| Lua → native method | 38.921 | 40.927 | 40.443 / 43.514 |
| Native library call to zero-return Lua callback | 214.907 | 222.494 | 217.211 / 224.690 |
| Same host lua_pcall discard-return callback control | 213.940 | 218.507 | 225.824 / 353.233 |

The library-call row uses each library's native return handling (baseline requests
one discarded result, candidate checks zero returns), so it is not an identical
return protocol. The host-control row uses the same nresults=0 boundary. Its
candidate tail outlier and small median difference do not establish a general
callback slowdown. The property result is a useful isolated gain; other rows do
not demonstrate universal improvement.

Warm-cache compilation of the **single benchmark TU** to a fresh object has median
wall times 0.325 s baseline / 0.528 s candidate (one warmup, three interleaved
measured compiles each). This is not clean full-engine build time. The unstripped
Release benchmark executables are 340,472 / 364,312 bytes. Binary size includes
common linked Lua code; neither number is a game bundle size. Full argv, compiler
version, sample CSVs and distributions stay ignored in build/phase12-measurements.

## Decision and next implementation

Proceed to a complete, separately verified LuaBridge3 integration at the fixed
Lua 5.4.9 VM version. The motivation is explicit checked results, maintained
binding APIs and the demonstrated native-property path, not a blanket speed claim.
The prerelease status, changed defaults, compilation cost and C++26 warnings are
known costs. Do not promote a partial integration or treat isolated probes as
proof that Actor/physics/editor/reload/TypeScript behavior has migrated.

The engine must own its protected call, result conversion, diagnostic and panic
policies explicitly. Preserve callback return tolerance, readonly/writable fields,
raw identities, generations and existing numeric validators. Adapt all production
and test uses coherently; do not use unchecked casts or weaken regression assertions
to hide result-type changes. Retain the current baseline for a matched whole-runtime
comparison, then require the full applicable Debug/Release/sanitizer suite,
compiled TypeScript replay/reload parity and representative render/gameplay checks.
Lua 5.5 and alternative VMs remain separate experiments after that boundary is sound.

The original 2.7 headers and seven Phase 11 Release executables are retained under
ignored build/binding-baseline-phase11. The copied game passed a real headless
three-tick village-defense step and quit through its supervisor. These binaries
still depend on the current local SDL build rpaths; this is a local comparison
baseline, not a distributable bundle. During migration, point the experiment's
baseline include path at these retained headers rather than the replaced vendor
folder, and keep the baseline/candidate compile definitions in separate binaries.

## Reproduce

Acquire the pinned official source separately; retain its LICENSE.txt:

```sh
curl --fail --location https://codeload.github.com/kunitoki/LuaBridge3/tar.gz/53e031b7df6a14d43f92a54fd792b76dbadcc970 \
  -o build/dependency-sources/LuaBridge3-53e031b.tar.gz
tar -xzf build/dependency-sources/LuaBridge3-53e031b.tar.gz -C build/dependency-sources
cmake -S tools/binding-experiment -B build/binding-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DPOCKET_LUABRIDGE3_SOURCE="$PWD/build/dependency-sources/LuaBridge3-53e031b7df6a14d43f92a54fd792b76dbadcc970/Source"
cmake --build build/binding-release --parallel 6
ctest --test-dir build/binding-release --output-on-failure
python3 tools/binding-experiment/measure.py --build build/binding-release \
  --output build/phase12-measurements
```

Use distinct build/binding-debug and build/binding-sanitize trees for Debug;
the latter adds -DPOCKET_BINDING_SANITIZE=ON and runs CTest with
ASAN_OPTIONS=detect_leaks=0 on this macOS setup. The standalone configuration
requires the candidate path and never downloads dependencies automatically.
The Clang/GNU compile inventory's separate command is in its linked document.
