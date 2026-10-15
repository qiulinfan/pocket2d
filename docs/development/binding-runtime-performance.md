# Real-runtime binding performance comparison

## Measurement status

Five separate three-pair comparisons completed on 2026-10-15 in coordinated idle
windows on Darwin 27.0.0 / arm64: old LuaBridge 2.7 versus initial integrated
result-mode LuaBridge3; old 2.7 versus the single-read enabled follow-up; initial
versus enabled LuaBridge3; old 2.7 versus the identical-state fastpath; and enabled
versus fastpath LuaBridge3. All 30 Release processes exited successfully with empty
stderr. All 120 workload rows were complete with 68 ticks, the expected actor
count checked and equal native/Lua final states. Binaries remained unchanged
within each comparison. Every earlier result is retained below.

The final fastpath variant reduces the original Lua-loop p50 regression from
approximately 9–11% to 0.9–1.7% against 2.7 in these workloads. Native animation
p50 improves by 19–24%. The direct enabled→fastpath comparison shows a 6–7% Lua-loop
p50 improvement at both actor counts. This closes most of the measured gap; it
neither proves Lua is faster than 2.7 nor establishes a universal speedup.
Functional acceptance and graphical smoke completion remain separate gates.

The retained baseline is the local Phase 11 Release engine with LuaBridge 2.7
(commit `223c03a1f31f5ad5acd44aa69b736871cf25464a`), not a rebuild from repository
HEAD. Its executable is `build/binding-baseline-phase11/sprite_animation_benchmark`;
local provenance is in that directory's baseline.json. The candidate is the
integrated Release runtime using LuaBridge3 3.0-rc12 at
`53e031b7df6a14d43f92a54fd792b76dbadcc970`, with the same Lua 5.4.9 VM. Its executable
is `build/sdl3-release/tests/sprite_animation_benchmark`; the initial integrated
result-mode binary was separately retained at
`build/phase13-initial-result-mode/sprite_animation_benchmark`. The enabled variant
was retained at `build/phase13-enabled-result-mode/sprite_animation_benchmark`.
The final variant adds the documented identical-nonnull-state fast return in
[LuaBridge's local patch record](../../thirdparty/LuaBridge/UPSTREAM.md); distinct
coroutine/VM comparisons retain the original fallback and assertions remain enabled.
The candidate uses the
checked LuaBoundary host calls without enabling LuaBridge registry-level throws,
and `LUABRIDGE_SAFE_LUA_C_EXCEPTION_HANDLING=1`.

This measures the production binding migration and its host adapters together.
It is distinct from the isolated binding-operation benchmarks in
[binding experiment results](binding-experiment-results.md).

## Matched workload and correctness

The tracked workload is tests/sprite_animation_benchmark.cpp. Each executable
creates its own identical geometric atlas and scenes, then runs full headless
Engine::StepSimulation calls with 1,000 and 10,000 actors. Settings are explicit
and identical: 8 warmup ticks, 60 measured ticks and a 30,000 ms cooperative budget
per case. The baseline/candidate workload differs only in the checked-cast syntax
needed to read final states outside the timed region.

Each actor has a Transform, SpriteRenderer and either native SpriteAnimation or
a small Lua animation component. Both select the same two atlas cells, advancing
every two ticks. The Lua component caches its sprite reference in OnStart, then
updates real Lua state and the native sprite property through the binding.
Native animation still performs production event generation/dispatch bookkeeping,
although the fixture has no animation-event subscribers. The small Lua loop does
not reproduce every native animation feature; compare each mode across binding
versions rather than interpreting their difference as feature-equivalent engines.

Timing excludes fixture generation, startup and final-state validation. Startup
is reported separately. After timing, the executable checks every actor's cell,
elapsed ticks, relevant native override state, total frame count and absence of
runtime errors. These are final-state checks, not a complete trace of every
intermediate callback. It also requires equal final native/Lua states for equal tick
counts. A complete matched row must report all requested ticks, all actors checked
and `paired_final_state=equal`. Partial or failed rows cannot establish a speedup.
The case budget is cooperative: an indivisible startup or step can overrun it.

## Repetition and summary method

The ignored script build/phase13-runtime-comparison/run_comparison.py prepares
three paired runs, alternating which executable runs first: baseline/candidate,
candidate/baseline, baseline/candidate. It checks the executable metadata has not
changed during measurement and rejects unmatched, incomplete, non-Release or
incorrect rows. Raw CSV, stderr and run metadata stay in that ignored directory.
The initial command is below. Later comparisons used separate copies of the
script in build/phase13-runtime-comparison-enabled,
build/phase13-runtime-comparison-enabled-attribution,
build/phase13-runtime-comparison-fastpath and
build/phase13-runtime-comparison-fastpath-attribution. The attribution scripts
substitute the appropriate retained LuaBridge3 binary for their baseline. Each directory retains its
own six raw CSVs, summary, metadata and stdout/stderr logs. A later rebuild at the
candidate path is a different artifact.

```sh
python3 build/phase13-runtime-comparison/run_comparison.py --confirmed-idle
```

For each mode/count, report the median and range of the three per-run p50 values,
and separately the median and range of the three per-run p95 values. These are
summaries of run-level quantiles, **not** pooled quantiles of all 180 steps. Report
startup separately and use the same inputs for every pair. Three runs provide a
bounded local comparison, not a statistical confidence interval or a universal
regression threshold.

### Initial results, before the enabled-field follow-up

Three-run summaries below show **median [minimum–maximum]** in milliseconds.
Percent changes compare the medians of the three run-level statistics; positive
means slower. The raw results are retained in the ignored comparison directory.

| Actors | Mode | Baseline step p50 | Candidate step p50 | Change |
| ---: | --- | ---: | ---: | ---: |
| 1,000 | native | 0.4935 [0.4825–0.5135] | 0.4220 [0.4189–0.4237] | -14.5% |
| 1,000 | lua_loop | 0.5815 [0.5798–0.5887] | 0.6348 [0.6219–0.6355] | +9.2% |
| 10,000 | native | 5.2505 [5.1567–5.2838] | 4.4530 [4.3520–4.5000] | -15.2% |
| 10,000 | lua_loop | 5.8886 [5.7010–5.9845] | 6.3690 [6.3060–6.6722] | +8.2% |

| Actors | Mode | Baseline step p95 | Candidate step p95 | Change |
| ---: | --- | ---: | ---: | ---: |
| 1,000 | native | 0.5396 [0.5152–0.5542] | 0.4506 [0.4462–0.4608] | -16.5% |
| 1,000 | lua_loop | 0.6202 [0.6122–0.6271] | 0.6675 [0.6527–0.6774] | +7.6% |
| 10,000 | native | 5.6446 [5.5818–5.7563] | 4.7556 [4.6345–4.9507] | -15.7% |
| 10,000 | lua_loop | 6.2750 [6.2393–6.3744] | 6.8599 [6.8085–7.5044] | +9.3% |

Native animation's p50 improved by about 15%, while the Lua loop's p50 regressed
by 8–9%. The before/after p50 ranges do not overlap in any of these four groups.
At 10,000 actors the Lua loop adds about 0.480 ms to its run-median p50 and 0.585 ms
to its run-median p95. This is a material local regression for the Lua-heavy
workload, even though the native path improves. It must remain visible in the
migration assessment; these results do not establish an all-workloads speedup or
identify which individual adapter/binding operation causes the difference.

Startup is outside the step timing. Its corresponding medians and ranges are:

| Actors | Mode | Baseline initialization | Candidate initialization |
| ---: | --- | ---: | ---: |
| 1,000 | native | 26.6687 [26.5790–34.1665] | 27.1508 [26.6195–32.9594] |
| 1,000 | lua_loop | 18.8260 [18.7703–19.3322] | 18.9865 [18.9619–20.2206] |
| 10,000 | native | 217.5736 [216.2949–218.4770] | 220.7230 [219.5717–222.9507] |
| 10,000 | lua_loop | 187.1621 [186.6028–187.9315] | 187.0166 [186.3805–187.8430] |

All runs, including the first process's startup measurements, are retained. No
partial run, slower repetition or cold-start sample was discarded. Init medians
changed by less than 2% in this small sample, with wider variation at 1,000 actors.
No extra tuning or source change was introduced between these six initial runs.
After reviewing the Lua-loop regression, a separate bounded optimization of the
three enabled-field reads was authorized. It performs one checked exact-type
read, preserves false booleans, and keeps the established non-boolean default of
true. The following comparisons measure that change without replacing initial
data or relaxing diagnostic/stack checks.

### Enabled follow-up versus the old 2.7 baseline

The settings and pairing order are identical to the initial run. These comparisons
use their own contemporaneous 2.7 measurements; do not subtract values from two
separate sessions to attribute the optimization.

| Actors | Mode | 2.7 step p50 | Enabled-v3 step p50 | Change |
| ---: | --- | ---: | ---: | ---: |
| 1,000 | native | 0.4957 [0.4872–0.4967] | 0.4156 [0.4135–0.4279] | -16.2% |
| 1,000 | lua_loop | 0.5775 [0.5642–0.5843] | 0.6288 [0.6232–0.6322] | +8.9% |
| 10,000 | native | 5.2774 [5.2108–5.4791] | 4.3457 [4.3400–4.4858] | -17.7% |
| 10,000 | lua_loop | 5.8281 [5.7843–5.9058] | 6.4683 [6.4323–6.6738] | +11.0% |

| Actors | Mode | 2.7 step p95 | Enabled-v3 step p95 | Change |
| ---: | --- | ---: | ---: | ---: |
| 1,000 | native | 0.5210 [0.5147–0.5485] | 0.4479 [0.4372–0.4652] | -14.0% |
| 1,000 | lua_loop | 0.6220 [0.6185–0.6364] | 0.6852 [0.6569–0.7066] | +10.1% |
| 10,000 | native | 5.6725 [5.5874–5.8799] | 4.5929 [4.5622–4.7221] | -19.0% |
| 10,000 | lua_loop | 6.3719 [6.2837–6.5160] | 6.9105 [6.8494–7.2316] | +8.5% |

The Lua-loop regression remains visible with non-overlapping before/after p50
ranges at both sizes. At 10,000 actors its median p50 is about 0.640 ms higher than
2.7. Native p50 improves by 16–18%. Initialization medians remain within 1.6% of
this comparison's baseline; full startup ranges are retained in summary.csv.

### Direct attribution: initial LuaBridge3 versus enabled follow-up

This extra three-pair run compares the two retained LuaBridge3 artifacts directly
with the same settings and validation. Both use the checked result-mode host
boundary; the bounded difference is the three enabled-field reads.

| Actors | Mode | Initial-v3 step p50 | Enabled-v3 step p50 | Change |
| ---: | --- | ---: | ---: | ---: |
| 1,000 | native | 0.4286 [0.4052–0.4314] | 0.4158 [0.4120–0.4286] | -3.0% |
| 1,000 | lua_loop | 0.6340 [0.6284–0.6453] | 0.6330 [0.6198–0.6331] | -0.2% |
| 10,000 | native | 4.5182 [4.4085–4.5887] | 4.4163 [4.3393–4.5056] | -2.3% |
| 10,000 | lua_loop | 6.6255 [6.5138–6.7481] | 6.4262 [6.3380–6.4767] | -3.0% |

| Actors | Mode | Initial-v3 step p95 | Enabled-v3 step p95 | Change |
| ---: | --- | ---: | ---: | ---: |
| 1,000 | native | 0.4547 [0.4368–0.4560] | 0.4502 [0.4306–0.4605] | -1.0% |
| 1,000 | lua_loop | 0.6754 [0.6749–0.6823] | 0.6805 [0.6527–0.6871] | +0.8% |
| 10,000 | native | 4.8182 [4.7568–6.0607] | 4.6358 [4.5933–4.7562] | -3.8% |
| 10,000 | lua_loop | 7.0582 [7.0507–7.0975] | 6.7710 [6.7093–6.8578] | -4.1% |

For 1,000 Lua actors the p50 change is negligible and the run ranges overlap. At
10,000 Lua actors the p50 improves about 3.0% with non-overlapping ranges in this
small sample; its p95 improves about 4.1%. Native p50 medians improve 2–3%, but their
ranges overlap. This supports a modest bounded benefit, not a claim that the
original Lua-heavy regression had been resolved by that enabled-read change.
The subsequent, separately measured fastpath follows below.

### Final identical-state fastpath versus 2.7

A source-backed profile identified repeated state-identity checks in Release.
The local patch returns immediately only for identical, non-null state pointers;
it does not remove assertions or bypass comparisons between distinct coroutine/VM
pointers. These measurements use the same counts, warmup, sample count, budget and
pairing order as the previous batches.

| Actors | Mode | 2.7 step p50 | Fastpath-v3 step p50 | Change |
| ---: | --- | ---: | ---: | ---: |
| 1,000 | native | 0.4958 [0.4922–0.5026] | 0.4027 [0.3994–0.4135] | -18.8% |
| 1,000 | lua_loop | 0.5860 [0.5805–0.5933] | 0.5961 [0.5949–0.6048] | +1.7% |
| 10,000 | native | 5.4216 [5.2482–5.4471] | 4.1283 [4.0475–4.2832] | -23.9% |
| 10,000 | lua_loop | 5.9479 [5.6668–6.2150] | 6.0000 [5.8772–6.0682] | +0.9% |

| Actors | Mode | 2.7 step p95 | Fastpath-v3 step p95 | Change |
| ---: | --- | ---: | ---: | ---: |
| 1,000 | native | 0.5380 [0.5369–0.5381] | 0.4253 [0.4155–0.4405] | -21.0% |
| 1,000 | lua_loop | 0.6360 [0.6325–0.6425] | 0.6543 [0.6458–0.6599] | +2.9% |
| 10,000 | native | 5.7085 [5.6792–5.7596] | 4.3485 [4.2682–4.7224] | -23.8% |
| 10,000 | lua_loop | 6.4943 [6.3039–6.5599] | 6.5512 [6.3486–6.6053] | +0.9% |

The remaining Lua-loop p50 difference is about 0.010 ms at 1,000 actors and
0.052 ms at 10,000 actors. At 10,000 actors the before/after ranges overlap for
both p50 and p95; at 1,000 there is a small non-overlapping residual difference.
The original 9–11% regression is substantially reduced, but exact parity or a Lua
speedup is not established by three runs. Native p50 improves about 19% and 24%.
Initialization medians are within roughly 3.1% of this batch's baseline; full
startup ranges remain in the raw summary.

### Direct attribution: enabled-v3 versus identical-state fastpath

Both artifacts already contain the checked result-mode boundary and the
single-read enabled helper. This batch isolates the documented state-identity
fastpath; the retained enabled binary is used directly rather than reconstructing
it from repository HEAD.

| Actors | Mode | Enabled-v3 step p50 | Fastpath-v3 step p50 | Change |
| ---: | --- | ---: | ---: | ---: |
| 1,000 | native | 0.4215 [0.4203–0.4390] | 0.4182 [0.4062–0.4200] | -0.8% |
| 1,000 | lua_loop | 0.6407 [0.6322–0.6437] | 0.5998 [0.5877–0.6087] | -6.4% |
| 10,000 | native | 4.3943 [4.3537–4.6567] | 4.2073 [4.2041–4.3064] | -4.3% |
| 10,000 | lua_loop | 6.5060 [6.3293–6.6946] | 6.0391 [5.8810–6.2231] | -7.2% |

| Actors | Mode | Enabled-v3 step p95 | Fastpath-v3 step p95 | Change |
| ---: | --- | ---: | ---: | ---: |
| 1,000 | native | 0.4610 [0.4465–0.4621] | 0.4361 [0.4292–0.4506] | -5.4% |
| 1,000 | lua_loop | 0.6864 [0.6802–0.6875] | 0.6471 [0.6408–0.6669] | -5.7% |
| 10,000 | native | 4.6533 [4.5604–4.9581] | 4.3720 [4.3647–4.5660] | -6.0% |
| 10,000 | lua_loop | 6.8886 [6.8275–7.1588] | 6.4953 [6.4017–6.6622] | -5.7% |

Lua-loop p50 improves 6.4% at 1,000 actors and 7.2% at 10,000, with non-overlapping
run ranges at both sizes. Its p95 improves about 5.7% at both sizes. Native p50
improves approximately 0.8% and 4.3%. This paired result supports the bounded
patch's contribution without attributing all migration gains to that one change.
No measurements were discarded, no validation was relaxed, and no assertions
were disabled between the retained enabled variant and the fastpath variant.

## Graphical smoke evidence and reproduction

The final fast-path Release build completed a 180-frame defense run and a two-frame
inventory run on Metal, both exit 0 with empty logs. Their captures were visually
inspected at build/phase13-captures/village-defense.png and inventory-village.png:
player/landmarks/two spawned invaders/HUD and bag/chest/equipment UI are visible.
The 500-frame command below remains an optional continuation, not an executed
acceptance claim. Example reproduction commands follow. Captures are taken before
the last frame is presented, then the runtime exits automatically.

```sh
mkdir -p build/phase13-runtime-comparison/captures build/phase13-runtime-comparison/saves
POCKET_RENDER_DRIVER=metal build/sdl3-release/game --project tests/fixtures/village_defense --frames 180 --capture build/phase13-runtime-comparison/captures/village-defense-180.png
POCKET_RENDER_DRIVER=metal build/sdl3-release/game --project tests/fixtures/village_defense --frames 500 --capture build/phase13-runtime-comparison/captures/village-defense-500.png
POCKET_RENDER_DRIVER=metal build/sdl3-release/game --project tests/fixtures/inventory_village --save-dir build/phase13-runtime-comparison/saves --frames 2 --capture build/phase13-runtime-comparison/captures/inventory-village-2.png
```

The defense captures exercise initial lifecycle, invasion spawning and unattended
progression; inventory checks its initial native-container/UI path. Inspect actual
images and stderr before reporting completion. These fixed-frame captures do not
exercise mouse transfer, consumption, combat input or save/load interaction; the
corresponding real-agent regressions and human playtests remain separate evidence.

## Limits

This is a headless CPU-step measurement, with no GPU rendering, VSync, physics
bodies or audio workload in its animation fixture. It does not measure memory,
packaging, every gameplay script, interactive editor cost or cross-engine SOTA.
Native mode is a useful production-path comparison, not a binding-free control.
A binding microbenchmark gain does not automatically predict this full-runtime
result, and any observed cost includes the chosen checked host-boundary behavior.
