# Native Tilemap performance boundary

The first local comparison uses `tests/tilemap_benchmark.cpp` in Release on the
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
