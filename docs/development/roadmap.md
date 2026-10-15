# Pocket2D modernization roadmap

Owner direction, 2026-10-15: independently develop Pocket2D toward leading
performance and completeness, with agent-native gameplay. The required stack
is C++26, SDL3, multiple rendering backends, Lua, and multithreading. Village_Rim
is the intended eventual migration target. The owner authorized planning here
because no earlier Pocket2D charter was located; this does not adopt Amoris's
separate roadmap.

Additional owner direction: evaluate a better Lua embedding design, and plan
for TypeScript as a possible second authoring language to improve agent
development. A second language does not by itself require a second VM. The
owner also authorized using existing OMP with GLM/DeepSeek for bounded, isolated
game-authoring evaluations; model self-reports require independent runtime tests.

## Required end state

1. C++26 is required at configure time. SDL3 is used natively by runtime and
   editor, with pinned, reproducible dependencies and retained licenses.
2. Backends are selectable and observable. Metal, Vulkan and Direct3D-class
   hardware paths plus a software validation path are the intended platform
   matrix. Passing one macOS backend does not certify other platforms.
3. The real Lua/physics gameplay loop supports bounded step/action/observation,
   seeded resets, explicit failures and replay. It can run without a window or
   audio device; the editor and interactive runtime use the same simulation.
4. Multithreading serves measured CPU workloads. Thread ownership, deterministic
   merge, cancellation and shutdown are tested. Lua and physics remain serial
   until a concrete ownership design proves safe parallelism.
5. Representative workloads cover dense sprites/tilemaps, animation, collision,
   particles, scene queries, streaming, audio, save/load, editor workflows and
   agent-driven simulation. Release benchmarks report distributions and workload
   correctness, comparing the same hardware, resolution and visual features.
6. Village_Rim migration eventually covers exploration, farming, inventory,
   equipment, elemental combat, enemies, invasions, portals/world persistence,
   UI/input/audio, and authoring. Original Unity assets and gameplay remain
   intact until a verified replacement exists.

## Execution order

- Establish the existing build and deterministic runtime tests; remove measured
  hierarchy-query scaling bottlenecks.
- Migrate toolchain/platform/render/audio/editor integration to C++26/SDL3.
- Ship the structured agent host and a village defense fixture, including
  movement, pickup, invasion victory/failure and permanent threat removal.
- Add a bounded CPU executor and integrate one measured data-parallel workload.
- Strengthen script instance isolation, native lifetime safety, diagnostics and
  resource budgets. Compare VM/binding changes against preserved real-game tests;
  provide a typed host API description before choosing a TypeScript backend.
- Evaluate external agents on authored gameplay tasks with frozen requirements,
  independent tests and preserved local traces; use failures to improve APIs.
- Expand rendering capabilities and workload benchmarks across native backends;
  optimize only after profiling correct representative scenes.
- Build asset import, tilemap/animation, save/load, tools and complete gameplay
  slices against the Village_Rim capability inventory.
- Compare matched workloads with current established 2D engines; publish only
  claims supported by comparable measurements and actual functional coverage.

These are cumulative requirements, not a definition of success limited to the
first slice. Broad performance/completeness claims remain unproven until the
full matrix is exercised. Stable fixtures/test code are tracked; run logs and
measurement output stay in ignored build directories.
