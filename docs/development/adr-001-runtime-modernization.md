# ADR 001: Native SDL3 and a shared deterministic simulation

Date: 2026-10-15. Status: accepted for implementation by owner direction.

The C++17/SDL2 engine already has a Lua component lifecycle and Box2D fixed
1/60 s step, but frame numbering advances during presentation, there is no
headless host, and native rendering/audio/editor integration depends on SDL2.
The owner explicitly requires C++26, SDL3, multiple backends and multithreading.

Use native SDL3 APIs and its platform rendering backends, keeping the existing
engine's scene and Lua contracts where they represent useful working behavior.
Expose one simulation step used by the graphical runtime and the agent host;
presentation must not advance simulation. The host receives bounded JSONL
commands and emits structured observations. Resets recreate the Lua/physics
runtime, and seeded Lua randomness is initialized before component loading.

Do not build a separate simplified simulation for the agent or retain an SDL2
compatibility implementation. Do not replace the engine with a new ECS before
workload measurements justify that architecture. SDL3's renderer is an initial
native-backend implementation; custom GPU pipelines remain possible when
profiling or required capabilities demonstrate a concrete need.

CPU parallelism has explicit data ownership and synchronization. SDL rendering,
Lua state and physics world mutation stay on the simulation/main thread.

Validation: build both executables, CTest real-runtime fixtures, command protocol
errors/reset/replay, backend rendering smoke, Release hierarchy benchmarks,
and thread teardown/determinism tests. Platform coverage must be named. Preserve
the last verified baseline in ignored local build output while migrating; Git
history remains unchanged and the original default project is retained.

References: [SDL3 migration](https://wiki.libsdl.org/SDL3/README-migration),
[SDL3 CMake integration](https://wiki.libsdl.org/SDL3/README-cmake).
