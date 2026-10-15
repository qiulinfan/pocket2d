# Pocket2D development guidance

- Read `docs/development/roadmap.md` for the owner's current direction and
  `docs/development/status.md` for verified progress and open work.
- Target C++26, SDL3, multiple rendering backends, Lua gameplay, and explicit
  multithreaded CPU work. Preserve a runnable editor and runtime while migrating.
- Agent-native gameplay means structured actions, observations, deterministic
  stepping, reset/replay, and diagnostics using the real gameplay runtime.
- Keep SDL window/render operations, Lua, and Box2D world mutation on their
  owning thread. Parallel work must consume owned immutable inputs and join
  before deterministic application of results; never share LuaRefs across workers.
- Use Village_Rim as a requirements source and integration workload. A small
  defense fixture proves only that slice, not migration of the full game.
- Build and test using CMake/CTest. Benchmark Release builds with warmup and
  workload counts; record timings locally under ignored `build/`, not in Git.
- Do not claim SOTA from an isolated microbenchmark. Comparable published
  baselines, representative workloads and platform coverage are still required.
- Keep README owner-authored. Development plans, ADRs, and handoffs belong here
  or in linked development documents. Preserve user work and source licenses.
- Parallel agents own disjoint files; one coordinator owns build configuration
  and integration.
- The owner authorizes local feature commits after review and relevant tests pass.
  Keep each commit cohesive and exclude unfinished features. Push only on an
  explicit request.
