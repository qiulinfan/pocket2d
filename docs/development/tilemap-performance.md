# Native Tilemap performance boundary

The first local comparison uses `tests/benchmarks/tilemap_benchmark.cpp` in Release on the
same Mac and SDL3 backend. The baseline instantiates one SpriteRenderer actor
per cell. The native path owns cell arrays and submits only visible tiles.
Both use the same generated atlas, cell IDs, camera path, 640×360 render target,
nearest filtering and final presentation. Each scenario compares exact final
RGBA pixels before reporting a successful result.

Commands, from the repository root:

```sh
./build/modern-release/tests/tilemap_benchmark 64 metal 21
./build/modern-release/tests/tilemap_benchmark 128 metal 21
./build/modern-release/tests/tilemap_benchmark 256 metal 21
./build/modern-release/tests/tilemap_benchmark 64 software 21
```

There are three warmup frames and 21 timed frames per fixed-camera, scrolling,
single-cell edit and 64-cell edit scenario. VSync is disabled. A scenario has a
30-second workload budget and explicitly labels partial samples; the native
path uses the same completed sample count as its baseline. All recorded runs
above completed their sample count and passed image comparison.

Local Metal fixed-camera median, milliseconds per render-and-present frame:

| Map dimensions | Individual sprites | Native Tilemap | Queued sprites / native tiles |
| --- | ---: | ---: | ---: |
| 64×64 | 8.515 | 8.879 | 4,096 / 32 |
| 128×128 | 21.215 | 8.893 | 16,384 / 32 |
| 256×256 | 87.297 | 8.894 | 65,536 / 32 |

Across all four 256×256 Metal scenarios, native medians were 7.88–8.89 ms and
baseline medians 87.17–87.50 ms. One or 64 ground edits within one chunk rebuild
one render chunk per changed frame and no collision chunks. Software at 64×64
was mixed, around 8.0–8.6 ms; the small case does not establish a universal win.

This comparison demonstrates the scaling effect of avoiding one actor and draw
submission per offscreen cell. It is ground-only and does not benchmark physics,
Lua simulation, animation, streaming or a representative mixed village. The
camera sees a small fixed world region; these values are not full-map throughput.
Timing includes renderer submission and presentation, not an isolated GPU timer.
RSS is a same-process snapshot with retained allocator/reference buffers, so it
cannot support an independent memory-reduction claim. No cross-engine comparison
or SOTA claim follows from this result.

Raw samples and CSV summaries stay in ignored `build/verification/`; this page
records the reproducible workload and bounded engineering conclusion. Functional
render tests separately cover atlas IDs, solid-layer alpha, sorting ties,
viewport clipping, 2× target scaling and zoom on software, Metal and OpenGL.

## Sprite submission path (2026-10-16)

Profiling the 256×256 individual-sprite case showed about 77% of main-thread
samples in `SpriteRenderer::QueueDraw`, mostly spritesheet metadata validation
and key building with `std::filesystem` for every sprite, plus a new LuaRef per
sprite to find its Transform. Actual draw submission was about 9%.

The change resolves each sprite's grid once per metadata revision, checks the
project root by comparing its native string in place (also in the texture
cache), resolves the pose in the render-phase loop from the component records,
orders draws by sorting 64-bit (sorting order, submission index) keys instead
of stable-sorting the requests, and writes root Transforms' world pose without
the hierarchy walk. Draw order and pixels are unchanged; the benchmark's
per-scenario sprite/native pixel comparison passed in every run.

Five interleaved before/after Release runs on the same Mac, Metal, 21 timed
frames each, p50 ranges in milliseconds per render-and-present frame:

| Individual sprites | Before | After |
| --- | ---: | ---: |
| 64×64 (4,096) | 8.54–8.75 | 8.08–8.56 |
| 128×128 (16,384) | 18.88–19.84 | 8.05–8.59 |
| 256×256 (65,536) | 76.63–77.83 | 10.23–12.83 |

Native Tilemap results were unchanged (7.81–8.92 after). Every case at or
below 16,384 sprites now sits at the same roughly 8 ms per-frame floor as the
native path with 32 tiles. At 65,536 sprites the remaining CPU time is mostly
per-actor component lookup and Lua userdata casts in transform resolution and
queueing, the target of native component pools.

### The 8 ms floor is display pacing

Sampling the native 64×64 case put about 92% of main-thread samples in
`SDL_FlushRenderer → METAL_ActivateRenderCommandEncoder`, waiting for the next
`CAMetalLayer` drawable; `RunRenderFrozenFrame` was about 8%. The software
renderer shows the same floor, and `POCKET_RENDER_VSYNC=0` does not change it.
The OS hands out drawables at the display refresh rate (120 Hz here, 8.33 ms),
so wall time per presented frame cannot fall below it.

The benchmark therefore also reports `cpu_p50_ms`/`cpu_p95_ms`, the main
thread's CPU time per frame, which excludes that wait. Fixed-camera Metal
medians from one run after the sprite change:

| Case | Wall p50 | CPU p50 |
| --- | ---: | ---: |
| Native Tilemap, any size (32 tiles) | 8.04 | 0.14 |
| 4,096 sprites | 8.61 | 0.89 |
| 16,384 sprites | 8.53 | 2.99 |
| 65,536 sprites | 11.45 | 11.44 |

Comparisons with other engines should use CPU (and GPU) time or uncapped
offscreen rendering, not presented frame rate on a display-paced window.
