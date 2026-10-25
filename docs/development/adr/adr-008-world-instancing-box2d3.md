# ADR 008: World instancing and Box2D 3.1.1

Date: 2026-10-16. Status: implemented; Debug, Release and ASan/UBSan suites pass,
and ThreadSanitizer reports no races in the multi-World tests.
Plan: [world-instancing-plan.md](../world-instancing-plan.md). Direction:
[agent-native-modernization.md](../agent-native-modernization.md) (M1, M2a).

## World

Runtime state used to live in globals and static members, so a process could
hold one simulation. Agent-native work needs several: batched environments,
branching, and an editor world beside a headless one.

`World` owns one simulation's mutable state. Subsystems keep their static APIs
and resolve state through `World::Current()`, as Dear ImGui resolves its
context. `Current()` is thread-local; each thread lazily owns a default World,
and `WorldScope` binds another. `Engine` holds a World and binds it in its
public entry points. One World is used by one thread at a time; different
Worlds may step on different threads.

State lives in typed slots (`World::State<T>()`), created on first use and
destroyed in reverse order, so modules keep their state types private and
World depends on none of them. Moved into World: input, particles, frame draw
queues, the frame clock, the scripting runtime and its settings, EventBus,
Lua API registration, the Lua budget, the save namespace, actor templates and
generations, the active scene directory, and physics.

Process-wide on purpose:

- texture, font and text caches, used only on the thread owning the renderer;
- the audio device. A per-World `audio_output_enabled` flag gates every
  AudioManager entry point, so headless Worlds never touch the mixer;
- the particle thread pool, which accepts concurrent submitters;
- the project root (`ResourcePath`); one process hosts one project.

An Engine releases only what it created: the SDL subsystems it initialized
(not `SDL_Quit`), and the renderer caches and recording helpers only if it
made a window.

### Cost

Thread-local lookups cost time on hot paths. The hottest scripting helpers
take an explicit `RuntimeState` and look the World up once. On the 10k-actor
animation benchmark (five interleaved Release runs, 180 measured ticks), step
p50 went from 4.19–4.36 ms to 4.60–4.91 ms (native) and from 6.15–6.44 ms to
6.73–7.03 ms (Lua loop). A non-thread-local pointer recovered about half of
it; the rest is slot lookup and code layout. Profiling the same benchmark
showed a larger existing cost: every Lua `pcall` saves and restores the signal
mask (`sigprocmask`, `sigaltstack`), because Lua was built without
`LUA_USE_POSIX` and used `setjmp` on macOS. A follow-up change defines
`LUA_USE_POSIX` for `lua_runtime` off Windows, so `ldo.c` uses
`_setjmp`/`_longjmp`. Seven interleaved Release pairs on the same benchmark:
Lua loop step p50 went from 6.07–6.51 ms to 3.89–4.22 ms; native stayed within
noise (4.16–4.54 ms before, 4.22–4.32 ms after). The Lua loop now more than
recovers the World lookup cost; native does not.

## Box2D 3.1.1

Box2D 2.4 had no determinism guarantee, one world per program in practice,
and synchronous contact callbacks that ran Lua inside the solver. Box2D 3.1.1
gives world handles, cross-platform determinism for the same binary
(with `-ffp-contract=off`, which its CMake applies), and events read after
the step.

It is fetched by tag through `cmake/GameEngineBox2D.cmake` and linked
statically. The vendored 2.4 copy is removed. Lua sees only engine types:
`Vector2` replaced `b2Vec2` before the switch, with the same arithmetic.

### Event semantics

- Collision and trigger callbacks run after `b2World_Step`, from a copy of the
  step's events. Lua therefore sees post-step state and may move bodies inside
  callbacks; the old locked-world errors are gone.
- While callbacks run, direct `Rigidbody`/`Tilemap` `OnDestroy` and scene edits
  are still rejected with the existing messages; `RemoveComponent` and
  `Actor.Destroy` stay available.
- Contacts ended by an operation between steps (disable, retype, destroy) are
  reported after the next step, not synchronously. Shape records keep the
  owner's handle until then, so a survivor still receives `OnCollisionExit`;
  if the other actor is gone, `collision.other:IsValid()` is false.
- Triggers detect only triggers. The host tracks trigger overlaps and reports
  a pair while both bodies are enabled and one is dynamic, the rule 2.4
  contacts followed (Box2D 3 sensors ignore body type and enablement).
- Relative velocity in contact events is measured after the step.
- Boxes have no 0.01 m skin radius, so resting distances differ slightly.

### Rotations

`b2MakeRot` is a deterministic approximation off by up to about 0.095°
(measured over ±360°), visible in authored rotations. Rotations are built and
read with double-precision `cos`/`sin`/`atan2` rounded to float. This relies on
the platform libm as Box2D 2.4's `b2Rot` did; cross-platform bit-exactness of
angle conversion is not claimed. A correctly rounded library such as CORE-MATH
would remove that dependency if cross-platform replays show divergence.

### Threads

`b2CreateWorld` and `b2DestroyWorld` scan one process-wide array without a
lock. Under ThreadSanitizer, two Worlds starting on two threads raced at
`world.c:108`/`129`, and an ASan run crashed. The engine serializes world
creation and destruction with a process-wide mutex; operations inside a world
need none.

Box2D's own multithreaded solver was not connected in the first slice (plan
step B3). Its solver enqueues one task per worker and the tasks spin on shared
stage counters, so they must run concurrently; `TaskExecutor::ParallelFor`
runs a task to completion before returning and would deadlock.

Update (2026-10-16): `TaskExecutor` gained `Submit`/`Wait`. Submit queues a
task's ranges on the shared pool; Wait executes the group's still-queued ranges
on the waiting thread before waiting for the rest, so the stepping thread can
always run the solver's coordinating task even when every pool worker is
spinning in another task. Physics worlds use this pool when they have at least
2,048 awake bodies and run tasks inline below that, where queueing measured
slower (`physics_benchmark`). `POCKET_PHYSICS_WORKERS=0` forces one thread.
Final body positions are identical with one thread and with the pool
(`physics_threads` CTest, also run under TSan).

## Verification

- `world_isolation_test` runs the defense and two-world fixtures with two input
  scripts. Each World matches a sequential reference digest (actors, exact
  float poses, every supported component property) every ten frames when
  replayed, interleaved with another World on one thread, and in parallel on
  two threads.
- All 74 CTest cases pass in Debug, Release and ASan/UBSan. One ASan run of the
  full suite had a failure that did not recur in later full runs; the
  two-world isolation test then crashed once in six runs before the world
  registry lock, and passed twenty runs after it.
- Metal captures of the default ball game, village defense and two-world
  village were inspected.

Not verified: other platforms, cross-platform replay, Box2D 3.2 snapshots,
and physics feel compared with 2.4 by a human player.
