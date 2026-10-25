# Phase 9: native runtime numeric boundaries

Local working-tree implementation; no commit or push. Pocket2D's full engine and
Village_Rim migration goal remains active. This slice strengthens actual native
mutation entry points, beyond the scene preparation introduced in phase 8.

## Delivered

- Rigidbody numeric Lua inputs are checked before float narrowing; booleans and
  body/shape enums enforce their declared types. Scalar configuration checks are
  shared with native preparation. Full configuration checks include actual shape
  mass/inertia, their reciprocals, combined/centered quantities and gravity-force
  intermediate products from the bundled Box2D implementation.
- Live methods reject invalid vectors, positions, angular/linear speeds, gravity
  and force before changing native state. Force bookkeeping accounts for multiple
  calls within a physics step and resets with Box2D's normal force clearing.
  Own-body momentum, material and lever-arm products have conservative bounds;
  this is not certification of all heterogeneous collisions or joints.
- The per-frame effective-type no-op path avoids repeated shape validation and
  ignores pending author configuration. Actual type changes validate installed
  fixtures. Position/rotation methods compare the live pose, so a prior raw field
  write cannot incorrectly suppress the corresponding explicit live operation.
- Particle numeric setters reject invalid types, float overflow and fractional or
  out-of-range integer values. Cross-field ranges remain configurable in either
  order and are validated before consumption. NaN end_scale and legacy color
  clamp/disabled-end-channel behavior remain supported.
- Particle capacity uses checked arithmetic with a one-million live/reserve limit
  per emitter. Lifetime motion envelopes reject unsafe derived states before
  emission. A whole burst stages validated requests and reserves SoA capacity
  before publication, preserving RNG state on validation rejection. Manual cap
  overflow throws; automatic cap overflow drops the entire wave. Temporary burst
  records share an image ID rather than copying image strings per particle.
- Bounded particle ages replace signed global frame subtraction; ordered lifetime
  bookkeeping and joined workers remain. The update path adds no per-particle
  configuration-validation pass. Initialization snapshots versus live origin,
  color, image and cadence behavior are explicitly documented.
- Particle batch rendering skips nonfinite projected rectangles and computes its
  culling radius without float-square overflow. Pixel tests verify ordinary
  particles draw and overflowed projections leave the target unchanged.

## Verification

All **55 CTest cases pass in Debug, Release and ASan/UBSan**, comprising 53 core
cases and two optional TypeScript authoring tests. Final suite durations were
31.94 s / 31.58 s / 60.26 s; these are verification durations, not performance
benchmarks. macOS LeakSanitizer is unavailable, so the sanitizer run uses
ASAN_OPTIONS=detect_leaks=0. Software, Metal and OpenGL Release pixel checks also
pass, including the new particle batch assertions. The original default project
launches for three frames with Metal, and its capture was visually inspected.
New cases are rigidbody_validation, particle_validation and agent_native_validation;
render_backend_test additionally checks actual particle pixels.

Tests cover rejection before native mutation/allocation, repeated force accumulation
and cancellation, recovery on later ticks, tiny-mass reciprocal failure, heavy/fast
momentum combinations, minimum dimension acceptance, raw versus live pose semantics,
strict Lua value types, particle expiry and recycled ages, whole-wave capacity rejection
without RNG drift, color compatibility and exact serial/parallel particle states.
The real supervisor regression checks ordinary callback-error recovery, finite
observations, valid movement and cached references after actor destruction. Large
allocation probes are rejected before attempting their requested allocation.

Run output and screenshots remain ignored under build/phase9-*. No run transcript,
checksum inventory or benchmark result file is added to version control.

## Local update-performance check

The phase-8 Release benchmark binary was retained before rebuilding. Three
interleaved before/after runs use the same compiler, 20 warmup ticks, 120 measured
ticks, seven workers and identical particle counts/configuration. The table shows
medians of the three per-run medians, in microseconds per update.

| Particles | Serial before | Serial after | Parallel before | Parallel after |
| ---: | ---: | ---: | ---: | ---: |
| 1,024 | 5.042 | 4.958 | 5.166 | 7.042 |
| 8,192 | 372.792 | 330.250 | 82.167 | 84.833 |
| 16,384 | 724.584 | 650.833 | 148.291 | 148.084 |
| 65,536 | 2882.209 | 2696.209 | 484.583 | 453.917 |
| 262,144 | 12092.458 | 11735.708 | 1878.500 | 1841.417 |

The 1k case executes serially even when an executor is provided. Its after-run
medians range from about 5 to 7.4 microseconds, so the percentage difference is
not a useful broad conclusion. At 8k–256k, observed parallel changes range from
about -6.3% to +3.2%; serial changes range from -11.4% to -3.0%. This measures the
existing update workload only, excluding spawn validation, whole bursts, physics,
rendering and whole-frame work. It supports no cross-engine or SOTA ranking.
Raw measurements remain ignored under build/phase9-particle-*.

## Remaining boundaries

These changes do not validate every Transform/SpriteRenderer field or every native
API. They are not an OS sandbox, aggregate native RAM budget, a general allocator
rollback mechanism or proof of all extreme Box2D solver combinations. Direct C++
state writes outside these APIs are not automatically mediated. Particle integration
still advances one tick per Update and intentionally ignores dt.

A separate read-only runtime audit reproduced a late InstantiateActor failure
that still runs a never-started component's OnDestroy on the next tick, changing
an existing actor's health 100→99. A late DuplicateRuntimeActorByUID failure
leaves a findable partial actor whose OnStart runs. AddComponent failure leaves
no component/lifecycle residue but consumes a generated key; monotonic gaps are
not by themselves a resource leak. The next slice must prepare the entire actor
before publishing components and must not execute gameplay teardown for rejected
unpublished candidates. Local evidence remains in build/phase9-spawn-audit. Maintained binding/VM evaluation,
source-aware diagnostics, full gameplay integration, platform packaging, human
playtesting and matched cross-engine benchmarks remain open requirements.

See [native runtime contract](../native-runtime-validation.md),
[scene transaction contract](../scene-transitions.md), and [status](../status.md).
