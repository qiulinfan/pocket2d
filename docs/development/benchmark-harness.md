# Benchmark harness

`tools/run_benchmarks.py` runs Pocket2D's fixed workloads on a Release build
and writes one JSON file per run to `build/bench/` (ignored by Git), with the
machine, commit, backend and every benchmark row. `--quick` uses 20 samples
and is only a smoke test.

```sh
cmake --build --preset ninja-release
python3 tools/run_benchmarks.py --backend gpu
```

| Workload | Benchmark | Measures |
| --- | --- | --- |
| W1 static sprites | `tilemap_benchmark 256 BACKEND` sprite path | 65,536 SpriteRenderer actors, fixed/scrolling camera and edits; pixels checked against the native tilemap path |
| W2 bunnymark | `bunnymark_benchmark BACKEND N 30 SAMPLES MODE` | N Lua-moved sprites, `per_entity` (`OnUpdate`) and `batch` (`OnUpdateBatch`) |
| W3 Box2D | `physics_benchmark 100 0 SAMPLES` | 5,050-box pyramid collapsing, one thread and the shared pool |
| W4 tilemap | `tilemap_benchmark 256 BACKEND` native path | 256×256 tilemap, fixed/scrolling camera and edits |
| W5 mixed village | `two_world_village` over the agent protocol | headless frame time while walking, Lua gameplay, physics and tilemaps |
| text | `text_benchmark BACKEND SAMPLES` | 280 `Text.Draw` per frame, 240 changing every frame |

Rendered workloads report wall time (display-paced on macOS, about 8 ms or
16.7 ms) and main-thread CPU time; compare CPU time for engine work.

## Bunnymark rules for other engines

A port measures the same work if it follows these rules exactly:

- Output 960×540 pixels; one 26×37 opaque sprite per bunny, one texture.
- Bunny state in pixels per frame, simulated at 60 steps per second with no
  frame-rate scaling. Start at (0, 0). Bunny `i` (1-based) seeds
  `s = (i·1103515245 + 12345) mod 2³¹`; `vx = (s mod 1000)/100`; then
  `s = (s·1103515245 + 12345) mod 2³¹`; `vy = (s mod 1000)/200 − 2.5`.
- Each step: `x += vx; y += vy; vy += 0.75`. If `x > 960`: `vx = −vx, x = 960`;
  if `x < 0`: `vx = −vx, x = 0`. If `y > 540`: `vy = −0.85·vy, y = 540`; if
  `y < 0`: `vy = 0, y = 0`, in that order, as in `bunnymark_benchmark.cpp`.
- The per-bunny update runs in the engine's gameplay scripting language and
  object model (for example one script or node per bunny); a batched variant
  may update all bunnies from one script call. State which variant is used.
- Warm up 30 frames, then record 120 or more frames; report p50/p95 frame CPU
  time and wall time, the renderer backend, vsync off, and the machine.

Ports for Bevy, Godot, Defold and LÖVE are not part of this repository yet.
Until they run on the same machine under these rules, results here are
Pocket2D's own baselines and support no claim of leading another engine.
