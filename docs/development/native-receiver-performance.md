# Native receiver validation: matched performance

## Status

Completed on 2026-10-15 on Darwin 27.0.0 / arm64 after the coordinator confirmed
that shared builds, tests and other benchmarks were idle. Both comparisons ran
three interleaved pairs: all 12 processes exited successfully with empty stderr.
All 24 runtime rows and 168 measured microbenchmark samples passed their built-in
correctness checks. Receiver correctness is covered separately by the real-worker
and binding regressions; these timings do not replace those gates.

The runtime measurements are noisy, with overlapping before/after p50 and p95
ranges in most groups. The microbenchmark's native-method median increases 3.5%,
but its run-median ranges overlap too. These data do not establish a consistent
material valid-path regression, a performance improvement, or zero validation
cost. The tables retain the observed changes and variability.

Both sides use Lua 5.4.9 and LuaBridge3 3.0-rc12 with the Phase 13 identical-state
fastpath. The before artifact and headers are retained in
`build/binding-baseline-phase13`; the after artifact uses the receiver/property
lookup guards in the current tree. This is not another LuaBridge 2.7 comparison.
Earlier migration results remain in
[binding runtime performance](binding-runtime-performance.md).

## Real-runtime workload

Run the retained Phase 13 Release `sprite_animation_benchmark` against
`build/sdl3-release/tests/sprite_animation_benchmark`, using the same generated
fixtures and explicit settings: 1,000/10,000 actors, 8 warmup ticks, 60 measured
ticks and a 30,000 ms cooperative budget per case. Three pairs alternate order
before/after, after/before, before/after.

The benchmark times full headless Engine::StepSimulation calls. Each actor has a
Transform, SpriteRenderer and either native SpriteAnimation or a minimal real Lua
loop. Native animation retains its production event bookkeeping; the Lua loop is
not a feature-complete substitute. Compare each mode across versions. There is
no rendering or physics-body workload in these scenes.

Every timing row must be Release, complete and report all requested ticks and
actors verified, with equal final native/Lua states. Runtime errors, partial
workloads and mismatched counts invalidate a comparison. Initialization and
final-state validation are outside the timed step. Report the median and range
of three run-level p50/p95 estimates, not a pooled 180-step quantile.

The ignored runner is build/phase14-receiver-performance/run_runtime.py, with
raw CSVs, stderr, metadata and summaries under that directory's runtime folder.
It requires `--confirmed-idle` and refuses incomplete or incorrect output.

### Runtime results

Each cell shows median [minimum–maximum] of the three per-run quantiles. Positive
changes mean slower. No slower or partial sample was silently dropped; all rows
were complete and verified.

| Actors | Mode | Before p50 ms | After p50 ms | Median change |
| ---: | --- | ---: | ---: | ---: |
| 1,000 | native | 0.4198 [0.4031–0.4355] | 0.4025 [0.3992–0.4168] | -4.1% |
| 1,000 | lua_loop | 0.6298 [0.5785–0.6715] | 0.6128 [0.5951–0.6315] | -2.7% |
| 10,000 | native | 4.7834 [4.1214–5.5776] | 4.5516 [4.5418–4.7272] | -4.8% |
| 10,000 | lua_loop | 7.2633 [6.0431–7.7356] | 6.8211 [6.2565–8.1640] | -6.1% |

| Actors | Mode | Before p95 ms | After p95 ms | Median change |
| ---: | --- | ---: | ---: | ---: |
| 1,000 | native | 0.6281 [0.5526–0.6379] | 0.4394 [0.4312–0.4500] | -30.0% |
| 1,000 | lua_loop | 0.7508 [0.6305–1.2227] | 0.6629 [0.6437–0.7406] | -11.7% |
| 10,000 | native | 5.2878 [4.5439–7.1042] | 5.9300 [5.9192–6.0620] | +12.1% |
| 10,000 | lua_loop | 9.4942 [9.1006–17.8166] | 8.1537 [6.8748–23.1610] | -14.1% |

All four p50 ranges overlap. The nominal p50 changes of roughly -3% to -6% should
not be presented as a receiver-validation speedup. The 10,000-actor native p95
median rises 12.1%, but its before/after ranges also overlap. The Lua-loop
10,000-actor p95 spans 9.10–17.82 ms before and 6.87–23.16 ms after, illustrating
why a strong tail-performance conclusion is not supported by these three runs.
The 1,000-actor native p95 ranges do not overlap in this sample, in the direction
of lower after time; the broader noisy results still preclude a general speedup
claim. Startup medians/ranges are retained separately in runtime/summary.csv;
they are outside the step timing and were not used to explain a causal guard cost.

## Bounded valid-call microbenchmark

Two byte-identical private copies of tools/binding-experiment/benchmark.cpp live
under ignored build/phase14-receiver-performance/micro-source. Both compile with
POCKET_BINDING_CANDIDATE=1; only their LuaBridge include root differs: retained
Phase 13 headers versus the current thirdparty headers. Both remove the original
explicit enableExceptions opt-in, keeping production result mode. Additional
checks outside timing require result mode and the original allocator-owned panic
callback after registration. No vendor guard is disabled.

Both link the same private build of the actual repository Lua C sources and
src/engine/scripting/ScriptBudget.cpp. The fixed configuration is C99 / C++26,
unsanitized Release `-O3 -DNDEBUG`, SAFE_LUA_C_EXCEPTION_HANDLING=1,
SAFE_STACK_CHECKS=1 and STRICT_STACK_CONVERSIONS=0. The actual compiler is Apple Clang 21.0.0
(clang-2100.3.34.2), targeting arm64-apple-darwin27.0.0 through /usr/bin/c++.
Both probe TUs used `-std=c++26 -arch arm64 -O3 -DNDEBUG`; their compile commands
have identical options except the private source/output filename and the intended
binding include root. The complete commands are retained in micro-results/runs.json.
Both copies verified result mode and the original panic callback at runtime.

Each process runs 200,000 iterations per sample, three warmup samples and seven
measured samples for each of four workloads:

- `lua_property_read_write`: a Lua loop reads and writes the native double field;
  one reported operation is the read/write pair plus loop/arithmetic work.
- `lua_native_method`: a Lua loop invokes a valid native `Add(1)` member.
- `callback_library_void`: C++ invokes a zero-return Lua callback through the
  checked library result interface.
- `callback_host_discard`: C++ invokes the same callback through the probe's
  protected raw-C-API call with zero results. This is a small adapter control,
  not the complete production lifecycle/error-reporting pipeline.

Every warmup and measured sample checks its final native value or Lua counter and
Lua stack depth; VM shutdown checks allocator accounting. The same three-pair
order is used. Report median [minimum–maximum] of the three per-process sample
medians, with all 21 measured samples per version/workload retained locally.
No malformed receiver is deliberately crashed inside a timed loop.

The private build source and run scripts are ignored local experiment artifacts,
not a new tracked benchmark framework. Raw data belongs under
build/phase14-receiver-performance/micro-results.

### Microbenchmark results

Values are nanoseconds per reported operation, shown as median
[minimum–maximum] of the three process medians. A property operation is a read/write
pair; callback controls are not native member calls.

| Workload | Before ns/op | After ns/op | Median change |
| --- | ---: | ---: | ---: |
| `callback_host_discard` | 219.920 [219.893–220.878] | 222.859 [218.093–224.775] | +1.3% |
| `callback_library_void` | 225.948 [224.656–228.580] | 222.378 [218.345–232.596] | -1.6% |
| `lua_native_method` | 42.097 [40.337–46.524] | 43.560 [40.692–43.841] | +3.5% |
| `lua_property_read_write` | 76.403 [74.688–78.509] | 76.186 [75.878–84.132] | -0.3% |

The native-method median increases by about 1.46 ns per loop iteration (+3.5%).
Property read/write is essentially unchanged (-0.3%); the library callback and
small raw-discard adapter change -1.6% and +1.3%. All four process-median ranges
overlap. All 21 individual measured samples per side/workload remain in the raw
CSVs, including outliers; none was removed when calculating these summaries.
This small experiment provides a bounded cost estimate, not proof that the checks
are free or that the nominal signs will repeat on another machine.

## Limits

These measurements quantify the valid-call cost of the bounded receiver changes
on one machine and fixed workloads. They do not measure crashes as performance,
prove a sandbox boundary, cover all class shapes, establish frame-rate behavior
for the whole game, or support a SOTA claim. Any observed method/property cost and
callback variability must be reported alongside the full-runtime result; neither
can substitute for the other. Correctness guards remain required regardless of
whether their valid-path cost is measurable here.
