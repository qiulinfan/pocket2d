# SpriteAnimation update comparison

The first local Release measurement compares complete headless Engine steps,
not an isolated C++ arithmetic loop. Each actor has the same Transform and
SpriteRenderer, the same generated two-cell atlas/grid, and a two-frame loop with
two ticks per frame. The reference Lua component caches its SpriteRenderer in
OnStart and updates its column from an integer timer. The native path uses the
production SpriteAnimation component, shared immutable asset and normal event
production/dispatch checks; neither path has animation event subscribers.

```sh
./build/ninja-release/tests/sprite_animation_benchmark
./build/ninja-release/tests/sprite_animation_benchmark --actors 10 --warmup 2 --samples 4
```

The default is 1,000 and 10,000 actors, eight warmup ticks and 60 measured ticks
per path. All actors are checked against the tick formula after measurement;
paths at the same completed tick count are also compared directly. A 30-second
per-case cooperative budget includes initialization, warmup and timed steps.
Partial runs are labelled and differing tick counts are not declared comparable.
The budget does not interrupt a single native constructor or Engine step.

Initial local results (Apple Clang 21, Release, SDL3):

| Actors | Native median / p95 ms | Lua loop median / p95 ms | Final state |
| --- | ---: | ---: | --- |
| 1,000 | 0.493 / 0.541 | 0.474 / 0.506 | 1,000 equal |
| 10,000 | 5.090 / 5.549 | 4.951 / 5.326 | 10,000 equal |

All four cases completed 68 ticks. These measurements do **not** establish a
native speedup. The simple Lua reference implements only the same loop state,
not the native clip validation, pause/stop/one-shot/marker/lifetime feature set.
It is a practical low-overhead reference for update cost, not proof that full
systems are interchangeable. Native runs first in this initial comparison;
cache/order/frequency effects and small timing differences need repeated,
order-balanced measurements before drawing a performance ranking.

This is headless: no SDL rendering/presentation, animated-actor GPU cost or human
input latency is timed. Pixel readback tests independently verify the actual
visual path. No heap-allocation or GPU profiler was attached. Later optimization
should profile the real manager/ownership/event costs and preserve all correctness
tests, rather than replace the workload with a cheaper private playback loop.
Raw CSV stays under ignored `build/verification/animation-benchmark-*.csv`.

## Phase 4 host-boundary changes

The Lua lifecycle/getter reentry fixes add protected dispatch and identity
rechecks. A subsequent local run measured the 10k Lua reference at 8.124 ms
median / 9.672 ms p95. A bounded raw-table prototype lookup path then reduced it
to 5.687 / 6.229 ms, retaining protected Lua fallback for executable getters,
userdata and cycles. Final actor states still matched for both 1k and 10k cases.
Native 10k medians were 5.271 ms before that fast path and 5.551 ms afterwards;
these small differences do not establish a native improvement. The original
phase-3 timings above remain historical measurements with different host checks.

The complete Debug/Release/ASan suites cover prototype shadowing, live prototype
changes, long/cyclic chains, getter errors and getter-driven mutations. This is
a narrow reduction of measured safety overhead, not permission to remove those
checks or a SOTA ranking. Raw output is kept in ignored
`build/verification/animation-benchmark-{protected-baseline,prototype-fastpath}.csv`.
