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
- Layout: each `src/` module keeps its headers beside its sources (include
  roots `src` and `src/engine`). `api/` holds the host API contract, agent
  command schema and asset schemas, with generated declarations in
  `api/types/`. Tests sit in `tests/{engine,editor,agent,benchmarks}` and are
  all listed in `tests/CMakeLists.txt`; fixture projects in `tests/fixtures/`.
  ADRs and phase checkpoints go in `docs/development/{adr,checkpoints}/`.
  Builds write only under `build/`, never into the source root.
- pocket2d-games and bleach-vs-naruto-pocket use these paths directly:
  `clients/python`, `tools/pocket`, `tools/room_server`, `docs/lua-api/`,
  `docs/development/{game-completeness-tests,netplay}.md` and `.engine/`. Move
  them only together with those repositories. Their tools find the runtime with
  `pocket2d.cli.find_game` (`POCKET_GAME`, else the newest preset build), so
  build directories follow `CMakePresets.json`. Games and samples stay out of
  this repository.
- Do not claim SOTA from an isolated microbenchmark. Comparable published
  baselines, representative workloads and platform coverage are still required.
- Keep README owner-authored. Development plans, ADRs, and handoffs belong here
  or in linked development documents. Preserve user work and source licenses.
- Parallel agents own disjoint files; one coordinator owns build configuration
  and integration.
- The owner authorizes local feature commits after review and relevant tests pass.
  Keep each commit cohesive and exclude unfinished features. Push only on an
  explicit request.
