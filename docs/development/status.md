# Pocket2D verified checkpoint

Updated: 2026-10-16. Revision 16. World instancing and Box2D 3.1.1 on top of the
verified engine/two-world checkpoint; local commits, no push. This checkpoint is not
SOTA certification or completion of the Village_Rim migration.

## Owner direction

C++26, native SDL3, multiple rendering backends, Lua and multithreading, with
agent-native gameplay. Improve the Lua embedding and evaluate TypeScript as a
possible second authoring language. Existing OMP + GLM/DeepSeek may be used for
bounded external-agent development evaluations. Full scope: `roadmap.md`.

2026-10-16: 2D only; agent-first, agent-native gameplay; the Lua runtime and C++
host stay. Plan: `agent-native-modernization.md`. First slice: World instancing and
Box2D 3.1.1 (`world-instancing-plan.md`, ADR 008).

## Revision 16: World instancing and Box2D 3.1.1

- A `World` owns one simulation's mutable state (scripting runtime, EventBus,
  physics, input, particles, frame draw queues, frame clock, budget, templates).
  Several Worlds run in one process, interleaved on a thread or on separate
  threads. Renderer caches, the audio device and the particle pool stay
  process-wide; headless Worlds never touch the audio mixer.
- `world_isolation_test` checks the defense and two-world fixtures: replayed,
  interleaved and two-thread runs match a sequential digest of actors, exact
  float poses and all supported properties every ten frames. ThreadSanitizer
  reports no races.
- Box2D 3.1.1 replaces the vendored 2.4, fetched by tag and linked statically.
  Lua sees an engine `Vector2`. Contact and trigger callbacks now run after the
  step from copied events; exits caused by disabling or destroying arrive after
  the next step. The 2.4 trigger rule (one dynamic body, both enabled) is kept by
  the host. ADR 008 lists all semantic differences, the rotation precision
  choice and the world-registry lock.
- All **74 CTest cases pass in Debug, Release and ASan/UBSan**. Metal captures of
  the default game, village defense and two-world village were inspected.
- Measured cost: on the 10k-actor animation benchmark, step p50 rose about 10%
  (native) and 8% (Lua loop) from thread-local World lookups; see ADR 008.
  Box2D's multithreaded solver is not connected: it needs an asynchronous task
  API that `TaskExecutor` lacks.
- Editor play runs in its own World: Play builds an embedded Engine on the
  editor's window and renderer and loads a snapshot of the authoring scene;
  Stop discards it. The edit World is not reloaded unless the draft changed
  during play, and a script quit or fatal play failure ends play, not the
  editor. `editor_play_world_test` covers isolation, host survival and the
  reload rule; the GUI Play button itself has not been exercised by a test.
- Engine diagnostics go through `Log` (text or JSON lines, per-World recent
  records reported in agent observations); optional Tracy zones via
  `POCKET_ENABLE_TRACY`.
- Sprite submission: per-sprite metadata and pose lookups removed from the
  render-phase loop. 65,536 individual sprites drop from about 77 to 10–13 ms
  per Metal frame with identical pixels; 16,384 reach the same ~8 ms floor as
  the native Tilemap path (`tilemap-performance.md`).
- Lua is now compiled with `LUA_USE_POSIX` off Windows, so protected calls use
  `_setjmp`/`_longjmp` and no longer save and restore the signal mask. Seven
  interleaved Release pairs (10k actors, 8 warmup, 180 samples): Lua loop step
  p50 6.07–6.51 ms before, 3.89–4.22 ms after; native unchanged within noise
  (4.16–4.54 / 4.22–4.32 ms). The macro also switches `io.popen` from an error
  to POSIX `popen`, decodes `os.execute`/pipe-close exit statuses, uses
  `getc_unlocked`, `fseeko`/`ftello`, `gmtime_r`/`localtime_r` and `mkstemp`
  for `os.tmpname`. It does not enable `LUA_USE_DLOPEN`. Trusted runtime
  scripts already had `os.execute`; reload candidates still block `io`/`os`.
  74 CTest cases pass in Debug, Release and ASan/UBSan.
- Physics API: circle/box overlap queries, circle casts, gravity, collision
  layers and masks, capsule and convex polygon colliders, impulses and torque,
  six joint kinds with motor/limit/spring controls, and a kinematic character
  mover built on Box2D's plane solver. Solid tiles now form one closed chain per
  connected outline per chunk, so bodies slide across cell seams without
  catching; chains are one-sided, so a body embedded inside solid tiles is not
  pushed out. The curated script contract, `.d.ts` and LuaLS files include the
  new API. `physics_api_test` covers it; **77 CTest cases pass in Debug,
  Release and ASan/UBSan.**
- Per-frame component dispatch no longer creates Lua registry references.
  Records carry a serial, a cached native pointer for built-in components, and
  a process-wide destruction epoch; callback snapshots keep record pointers and
  re-find them by serial only after some record was destroyed. Plain-table
  `enabled`/callback reads stay on the Lua stack; metamethods still take the
  protected, revalidating path. The animation pass resolves each renderer once
  per snapshot instead of three keyed lookups per tick. Release, 10k actors, 8
  warmup, 180 samples, four interleaved runs: native animation step p50
  4.06–4.31 ms before, 0.65–0.77 ms after; Lua loop 3.70–4.16 ms before,
  1.10–1.19 ms after. 77 CTest cases pass in Debug, Release and ASan/UBSan;
  under TSan the World isolation and editor play tests pass, while
  `agent_two_world_village` exceeds its agent deadline from instrumentation
  slowdown without race reports.
- Agent protocol v2: one schema (`scripting/api/agent-protocol.json`) defines
  the commands and generates the embedded schema and command reference;
  requests may carry an echoed `id`; `run_until` stops on published events,
  property comparisons or actor presence. `--record`/`--replay` write and
  verify JSON-lines replays and report the first divergent record and JSON
  pointer; a Debug recording replayed identically in Release over 1,020 frames
  of two fixtures. `--mcp` serves the commands as MCP tools for both the
  2026-07-28 per-request era and legacy `initialize` clients. 81 CTest cases
  pass in Debug, Release and ASan/UBSan. Not yet done: snapshot/branching,
  SyncTest-style rollback checks, multi-World handles over MCP, and an external
  coding agent completing an authoring task through MCP.
- Agent observations and actions: `step`/`run_until` can stream published
  events with bounded payloads; `observe`/`step`/`run_until` can select actors
  by name, UID, component type and region, and attach a headless PNG rendered
  by the software renderer that matches graphical mode pixel for pixel.
  Projects may declare semantic actions in `actions.json`, read in Lua through
  `Input.GetAction*` and set directly by agents. Scenario files and the
  standard-library Gymnasium-style Python client run three two_world_village
  tasks (harvest, defend a wave, close the route) solved by scripted key
  policies in CTest. Not yet done: vectorized and multi-agent environments,
  game-declared observation functions, and LLM-player evaluation.
- Authoring loop: `tools/pocket` checks projects (JSON Schemas for every asset
  format, runtime startup diagnostics, optional LuaLS/EmmyLua), runs scenario
  scripts and replays as tests, serves JSONL or MCP, renders frames and
  scaffolds projects with `AGENTS.md`. `scripting/api/schemas/` holds the
  asset schemas and `tools/validate_schemas.py` validates all 41 fixture
  assets in CTest. A root `llms.txt` indexes the agent-facing documentation.
  No Lua checker is installed here, so that path is untested.
- Per-frame systems (sprite submission, animation, tilemap commit/queue,
  Rigidbody body types) now walk cached lists of actors owning each built-in
  component kind, rebuilt only when records are created or destroyed or the
  actor order changes, and look up primary built-ins by native kind instead
  of type-name strings. Rigidbody body types are applied in scene order rather
  than hash-table order. Release, 65,536 sprites on the `gpu` renderer: main
  thread CPU p50 about 6.3 ms before both changes, 4.4–4.5 ms after; the
  native tilemap path 0.30 → 0.19–0.21 ms. Wall time stays at the ~8 ms
  display pacing floor.
- Sprite draw requests carry an interned image id instead of a copied name,
  textures are found by id, and per-frame spritesheet/scene values are read
  once per frame. Six interleaved Release runs, 65,536 sprites, `gpu`
  renderer, main-thread CPU median: fixed 4.03 → 3.74 ms, scrolling 4.65 →
  4.04 ms. A further attempt to cache actor and component pointers in the
  per-kind lists showed no measurable gain and was not kept.
- Text draws through SDL_ttf 3's renderer text engine: laid-out `TTF_Text`
  objects share one glyph atlas per renderer instead of rasterizing a surface
  and creating a texture per string, and the text cache evicts in one pass.
  `text_benchmark` (280 `Text.Draw` per frame, 240 changing each frame,
  Release, two runs per backend): main-thread CPU p50 5.29/5.47 ms
  (gpu/metal) before, 3.48–4.08 / 3.15–3.30 ms after. Against the old path,
  26 of 518,400 pixels differ at glyph edges by at most 18/255; no pixel test
  covers text.
- Box2D's multithreaded solver runs on the shared `TaskExecutor` through new
  asynchronous `Submit`/`Wait` calls, for worlds with at least 2,048 awake
  bodies. `physics_benchmark` (Release, 7 pool workers, two runs each):
  5,050-body pyramid collapsing 2.40–2.60 → 1.60–1.65 ms per step; 15 and 210
  bodies unchanged (inline). Final positions match one thread exactly.
- Dear ImGui is 1.93.0 (docking). Dear ImGui Test Engine 1.93.0 is vendored
  under its own license (free for individuals and open-source projects) and
  compiled into ImGui when `POCKET_ENABLE_IMGUI_TEST_ENGINE` is on (default).
  `editor_ui_test` drives the real editor overlay: clicking the viewport Play
  and Stop buttons, the Runtime menu items and F5 each start and stop a
  separate play World. The editor app's own dock layout and window are still
  not exercised by these tests.
- Lua execution model: component types may define `OnUpdateBatch(instances)`
  for one call per type per frame, and `Transform:SetLocalPosition(x, y)` sets
  both coordinates in one call. `bunnymark_benchmark` (Lua per-bunny gravity
  and bounce, `gpu` renderer, Release): 50,000 bunnies main-thread CPU p50
  19.1 ms with property writes, 15.0–15.4 ms with `SetLocalPosition`, and a
  median of 14.2 ms with batching (six interleaved runs; thermal throttling
  makes single runs vary by up to 4 ms).

## Completed and verified in this slice

- C++26 editor/runtime build in Debug and Release. Native SDL 3.4.18, image
  3.4.8, ttf 3.2.2 and mixer 3.2.4; coherent Dear ImGui SDL3 backends. Lua is now
  official 5.4.9, with pinned LuaBridge3 3.0-rc12 integrated. A documented
  local same-state comparison fast path retains distinct coroutine/VM checks.
- With optional TypeScript checks enabled, all **72 CTest cases pass in Debug and Release**.
  ASan/UBSan pass all **72 cases**, including software rendering and the actual editor overlay. The
  core suite has 70 tests and no npm requirement; two frontend tests explicitly
  require the pinned local toolchain. The latest matrix includes generated API
  conformance, real TS compilation/replay, map activation reentrancy, numeric preflight
  preservation, reset-startup timeout recovery, native animation and Lua getter
  callback reentrancy. Phase 8 additionally covers prepared scene transitions,
  module source identity/cache lifetime, detached native validation, editor
  document/autosave preservation, persistent actor conflicts and nonsticky
  scene-failure responses. Phase 9 covers direct Rigidbody/ParticleSystem input
  guards, force accumulation, particle capacity/lifetime and actual batch pixels.
  Phase 10 adds complete runtime-actor preparation, resource-cache isolation,
  source repair/retry and mixed publication/lifecycle ordering. Phase 11 adds
  coordinated document/runtime commands, retained-draft recovery, linked body
  poses, prepared native adoption, key-renamed events and deferred activation.
  Phase 13 adds the real LuaBoundary contract and state/coroutine identity cases
  while retaining the complete reload, typed gameplay and native validation suite.
  ThreadSanitizer
  previously passed the unchanged task-executor/particle determinism target.
  LeakSanitizer is unsupported on this local macOS setup;
  no leak-sanitizer success is claimed.
- Software, Metal and OpenGL renderer tests pass on this Mac, with actual pixel
  readback, fractional/rotated draws, nearest sampling and invalid-driver
  rejection. The original default game and visible defense fixture were rendered.
- Agent JSONL host uses the real simulation: fixed stepping, seeded reset,
  keyboard/mouse actions, ordered observations, component-property discovery,
  structured failures and clean quit. UID fields use lossless decimal strings.
- Stable native Actor storage and generation-checked Lua handles prevent live
  references from dangling during destruction/churn and reject stale identities
  after removal, replacement or scene reset. UID allocation wrap is tested.
- Lua component defaults are instance-owned, including nested graphs, aliases,
  cycles, static prototype inheritance and known native value types. Protected
  functions/upvalues and intentional globals keep their documented sharing.
- Particle arithmetic has a real joined CPU worker path. Serial/parallel outputs
  match; Lua, SDL and physics mutation stay on the owner thread.
- Scene/config validation rejects malformed data without a success exit, preserves
  valid legacy missing-UID input, and avoids truncating files for invalid values.
  Actual editor ParticleSystem save semantics are covered.
- Village defense fixture runs real Lua/Box2D movement, pickup, three-wave
  defense, source destruction, unattended failure and restart. Its geometric
  sprites/bitmap text are locally authored test art; no Unity art was imported.
- OMP 18.8.7 + GLM 5.3 Flash independently implemented a repair component in
  about 100 seconds. Independent checks pass 16/16 scenarios, 96 checkpoints,
  including a final Release/Lua-5.4.9 rerun. No human answer repair, second model
  call, credential/config change or automatic production integration occurred.
- Agent execution now uses a supervised worker process with Lua instruction and
  heap budgets, correlated bounded JSONL responses, a per-request wall deadline,
  worker termination/reaping and fresh-process reset. Initialization, nested
  callbacks and coroutines are accounted for. Timeout/native blocking/OOM,
  malformed output, EOF and failed-reset recovery are tested. Exact boundaries
  are in `script-budgets.md`; this is not an OS sandbox or process RAM cap.
- Native Tilemap resources support ground/solid layers, frame-zero cell queries,
  deferred edits, 32×32 CPU chunks, visible-cell culling and row-run Box2D walls.
  Scene preflight rejects invalid data/placement/render scale before destroying
  the previous world. Activation commits consistently across rendering and bodies,
  including reentrant EndContact writes. Locked-world direct destruction rejects
  safely; deferred component/actor removal clears bodies and fixture metadata.
- The 64×64 farming fixture exercises planting, 120-tick growth, harvesting,
  replanting, real wall collision/removal/passage and bounded agent cell queries.
  TilemapDocument/Inspector/Overlay support authoring preview, save, undo/redo,
  explicit memory discard and dirty guards. Fresh scene instances do not receive
  stale clean cached data. Actual UI drawing and mapless-scene draft access pass.
- Tilemap pixel readback passes on software, Metal and OpenGL, including atlas
  selection, alpha, ordering ties, culling, 2× target scaling and zoom. Both game
  fixtures also launch and capture successfully from copies containing only
  Git-visible source/resource files, with no ignored editor metadata.
- Native SpriteAnimation now shares immutable clip resources, tracks integer
  ticks per player, supports loops/one-shots, idempotent Play, explicit Restart,
  pause/resume/stop and frame/completion events. Transient renderer poses retain
  authored fields, tint, pivot, mirroring and sorting. Clearing an authored
  resource returns to an inert placeholder and can be configured again.
- Animation scene preflight, target rebinding, invalid/readonly writes, stale
  generations and callback stop/restart/remove/destroy are covered. The event
  dispatcher snapshots recipients before executable Lua field reads, protects
  those reads and rechecks liveness/playback identity before calls. A dedicated
  seven-case real-worker regression covers the reproduced getter-add-components
  crash, getter errors and enabled-getter reentry. This does not assert every
  older Lua metadata/lifecycle entry point has received the same audit.
- The animation fixture runs real movement, idle/walk and one-shot attack return.
  Animation pixel readback passes software, Metal and OpenGL, including frozen
  views, atlas rows/columns, tint/alpha, pivot, mirroring and resource clear/restore.
  Its source-visible-only copy also launches and captures successfully.
- Versioned SaveStore and Lua Save.Write/Read persist explicitly selected pure
  data with signed64/UTF-8 preservation, raw table conversion, cycle/type/size
  limits and structured errors. An explicit game namespace plus platform prefs
  or --save-dir keeps slots separate from source and survives agent reset.
  Exclusive temporary writes, file sync and atomic replacement preserve old
  bytes on validation, real kernel write and immutable-target rename failures.
  Directory fsync/power-loss guarantees and adversarial filesystem races are
  outside this verified contract; see `save-format.md`.
- The persistent village workload survives four independent supervisor processes:
  crop stages/remaining ticks, harvested supplies, four removed wall cells,
  player position/facing and a permanent irrigation upgrade affecting actual
  120→60 tick growth. Corrupt/future formats and game schemas do not partially
  mutate the world; reset leaves disk data intact. Its source-visible-only copy
  launches and captures without ignored metadata. Full original-game inventory,
  altar/tree/boss and multi-world save migration remains incomplete.
- Older Lua lifecycle/metadata/teardown paths now snapshot recipients and protect
  executable getters, recheck registered identity and restore the caller's stack.
  Tests reproduce the former OnDestroy-add64 crash and cover contacts, metadata
  failures, nested events, self-removal and forged identities. Normal hierarchy
  ordering and same-tick particle synchronization remain intact. Pure table
  prototype chains use bounded raw lookup; executable/complex cases keep the
  protected fallback and error semantics.
- A single curated `host-api.json` now generates TypeScript declarations, LuaLS
  metadata and the embedded agent `describe.script_api` contract. Native tests
  verify declared names and actual API behavior. Nullable results, native nominal
  values, static/instance receivers, true readonly and engine-owned fields are
  distinguished; unsafe uint64-as-number identity and unverified APIs are excluded.
- The optional local authoring tool pins TypeScript 6.0.2 / TSTL 1.37.1, reproduced
  with `npm ci --ignore-scripts`. It reports source diagnostics and promotes good
  staged output with ownership/lock/rollback checks. Its 27-case suite covers real
  typing/transpiler errors, detached receiver misuse, unmanaged/late user entries,
  promotion failure and trace-free JSON. The engine needs no Node or second VM.
- A TypeScript controller plus local SaveSchema module bundles into one Lua
  component with required lualib and license. Handwritten Lua and freshly compiled
  TS pass 102 complete real-engine comparisons on Debug/Release/ASan: movement,
  native animation, health/death, input edges, pause, identical saves, bad-schema
  refusal, reset and new processes. Its source-visible fixture plus compiled bundle
  also launches and captures graphically. This is authoring-slice evidence, not
  full JS/npm compatibility, verified runtime TS source maps or measured AI gains.

- Native inventory values and immutable item catalogs are now implemented. The
  model supports bounded slots, stack-first accepted/remainder adds, exact removes,
  targeted merge/swap, split, transfer, equipment filters and atomic group restore.
  Instance IDs survive movement and persistence; failed/no-op mutations preserve
  revisions. A seeded 5000-operation test checks conservation across two 100-slot
  containers. Lua userdata and raw snapshot parsing have lifetime/type checks.
- The inventory_village fixture uses 30 bag slots, a 12-slot chest, nine hotbar
  references and four equipment slots. Real mouse/keyboard operations cover
  preview/cancel/stale-revision rejection, partial pickups, food healing, measured
  boots movement, weapon damage, armor and exact fresh-process save restoration.
  Missing/malformed world candidates and a later native restore rejection preserve
  inventories, scalars, pose and actor lists both immediately and next frame.
- Inventory rendering passes software, Metal and OpenGL pixel checks for visible
  actors, equipped icons, cleared source slots, synchronized hotbar and mouse drop.
  All 61 source-visible fixture files independently launch and render without
  ignored editor metadata. The typed host contract includes inventory APIs; its
  TS inventory probe is compile-time interop coverage, not a full TS inventory game.

- Explicit component-script reload now stages selected sources in a separate
  32 MiB/one-million-instruction Lua VM, then transfers validated code/data graphs
  without publishing live globals. It preserves candidate cycles, aliases and
  shared upvalue cells while rebinding future runtime calls to the live host.
  Unsupported top-level capabilities, stdlib mutation and unknown dependencies
  reject; the pinned TSTL local loader has an explicit require=nil exception.
- The live reload transaction retains original instance and native object identity,
  runtime data, start status and actor activation timing. New missing defaults
  preserve self/nested aliases and known table-key correspondence. Lifecycle
  caches and active/pending named EventBus handlers switch together; deleted
  handlers are removed. Ambiguous aliases, instance closures, custom prototypes
  and anonymous subscriptions reject before changing the running world.
- The agent reload_scripts command and editor Tools reload window share the same
  manager entry point. Actual ImGui-click tests retain frame, OnStart count and
  dirty scene/map drafts, display persistent failures and recover after fixing
  source. Candidate loop/OOM and ordinary errors keep the worker usable. Large,
  malformed-byte and Chinese error diagnostics are UTF-8-safe and bounded to 4096
  bytes; unexpected host exceptions explicitly require reset rather than claim
  rollback. Editor hard wall-clock preemption and main-VM OOM recovery are not claimed.
- The compiled TypeScript bundle additionally passes 31 exact Lua-vs-TS comparisons
  through real hot reload and subsequent movement, animation and save/load helper
  execution, on top of the existing 102 comparisons. Arbitrary package/global
  closure state remains outside the reload migration contract.

- Scene preparation now constructs actor/template data, selected declarations,
  detached Lua/native components and Tilemap/Animation resources before old-world
  mutation. Rejected candidates preserve actor generations, Lua/native identity,
  subscriptions, pending lifecycle work, physics and global state. Template reads
  use a local canonical-source cache without touching the live Actor spawn cache.
- Trusted component bootstrap executes once with the initial scene preference.
  Known canonical source variants retain prototypes/private module state until
  reset; new declarations use isolated staging. Native shadow filenames are
  skipped and other host/stdlib filename collisions reject before evaluation.
  Persistent source ownership prevents a later alias from reusing another type's
  cached prototype. Explicit reload rejects redirected registered canonical paths
  and updates the matching cache on success, preventing stale variant revival.
- Component preparation discovers core lifecycle membership through raw/static
  inheritance, rejects default graphs with finalizer/close/weak behavior and owns
  nested GC guards until prepared references are released. Native host spec edits
  validate before replacement, including derived gravity/transform bounds and
  checked particle capacity. Phase 9 subsequently extends these checks to the
  direct Rigidbody/ParticleSystem routes described below; other native surfaces
  remain outside that validation scope.
- Queued scene persistence rejects retained-parent loss, incoming UID collisions,
  cross-scene authored parent links and changed source variants of retained types.
  Successful transitions retain explicit DontDestroy actor/native identities and
  start incoming components once. C++ Scene::LoadScene is now a data-only helper.
- Editor documents and Play/Stop flags change only after successful runtime load.
  Its outgoing dirty draft autosaves through a precommit gate after target
  validation; failed saves leave the world and draft intact. Actor selection
  clears only on successful open. The outer UI flow still has a manual check.
- Agent scene-load errors are tied to the current attempt: ordinary rejection
  reports once, and later requests can complete their full frame count. A failed
  queued load still permits the old world's ordinary tick. Postcommit teardown
  failure stops the damaged runtime and requires a fresh agent worker/editor
  restart; arbitrary external IO and main-VM OOM are not rollback guarantees.
- Final phase-8 Debug/Release/ASan matrices all pass 52 cases after the historical
  alias and redirected-source fixes. The original default game also launches and
  renders with final Release/Metal; the capture was inspected. Human interaction,
  fatal-state graphical presentation and audio quality are not certified by it.

- Direct Rigidbody Lua numeric inputs now validate before narrowing; logical
  flags and enums require actual declared Lua types. Native configuration checks
  include shape/combined mass and inertia, their reciprocals, gravity-force
  intermediates and supported domains. Live force/velocity/gravity methods check
  same-step accumulated forces and own-body point-speed/momentum products before
  mutation. Extreme heterogeneous contacts and joints are not fully certified.
- Rigidbody's no-op effective-type synchronization stays inexpensive and does not
  consume pending raw configuration. Actual retypes check installed fixtures.
  Explicit position/rotation controls compare the real body pose, preserving the
  difference between raw configuration and immediate movement.
- ParticleSystem numeric setters reject invalid types, fractional/out-of-int values
  and nonfinite/overflowed floats. Whole configurations and lifetime motion bounds
  are checked at consumption. Checked capacity and staged whole bursts preserve
  RNG and particle counts on validation rejection; manual over-cap calls reject,
  automatic over-cap emission skips the wave. One million particles per emitter
  is not an aggregate RAM limit. NaN end_scale, color clamp/disabled interpolation
  and initialized snapshot versus live-field semantics remain supported.
- Particle lifetime uses bounded ages with ordered recycling and joined worker
  integration. Configuration validation is outside the update hot loop. Burst
  staging shares image IDs and pre-reserves SoA storage. Validation atomicity is
  not a promise of rollback after allocator exhaustion or unrelated host effects.
- The new native and actual-agent regressions preserve object identity/state after
  rejection, continue valid physics ticks and verify stale references cannot
  recreate resources. Existing 16,384-particle serial/parallel snapshots still
  match exactly. Software/Metal/OpenGL pixel checks draw valid particle batches
  and skip nonfinite projected rectangles. Final Debug/Release/ASan suites pass
  all 55 cases; the original default project also launches and renders on Metal.

- Lua Instantiate and native runtime duplication now prepare every component,
  default graph, root pose and relevant peer/resource relationship before making
  the new actor visible. Rejected unpublished candidates never invoke gameplay
  OnDestroy; old health, queues and Actor/Lua/native/body identities stay intact.
  Legitimately published actors destroyed before their first start retain their
  existing destruction behavior.
- Detached Tilemap and SpriteAnimation instances hold fully prepared data without
  touching outer scene-preparation caches or creating physics bodies. Publication
  binds all peers and applies the prepared initial animation pose without further
  asset reads. Tests delete resource files between preparation and publication.
- Runtime publication updates only the new actor's indexes. Hash UID checks,
  monotonic actor-order reservations and conditional ordered insertion preserve
  mixed deferred/host-immediate traversal without full-world binding rebuilds.
  Host duplicate-name selection still scans names. Instantiate remains immediately
  findable with deferred activation; host duplication retains its prior idle-step
  OnStart/update timing. Failed UID/key reservations may leave opaque gaps.
- Template-cache misses are committed after complete preparation, so a newly
  rejected template can be repaired and retried. Successful cached templates
  retain their existing lifetime policy. Host duplicate rejection returns false,
  leaves the output UID untouched and exposes a bounded diagnostic; the Hierarchy
  panel can display it. Ordinary rejection does not require reset.
- Unexpected publication failure after CommitStarted stops subsequent frame phases
  and requires a fresh runtime; agent handling reports actor_publish_failed with
  requires_reset. A synthetic host notification tests callback/frame stopping,
  not actual allocator exhaustion. The normal failure regressions and existing
  gameplay/typed/reload suites pass all 59 cases in all three configurations.
- A final 180-frame Release/Metal village-defense smoke completes and renders two
  spawned invaders alongside the player and landmarks. The capture was inspected;
  interactive editor error presentation and human game feel remain manual gates.

- Complete scene-edit commands now prepare ordered mutations before publication.
  Ordinary rejection preserves previous specs, Actor/Lua/native state and queues;
  commit callback failures stop the runtime with an explicit reset diagnostic.
  SceneDocument stages a detached candidate, and Session's hook coordinates its
  publication with the runtime. Epoch receipts prevent duplicate UI delivery.
- Failed new edits preserve unrelated dirty work. Existing invalid drafts remain
  repairable in multiple edits, with persistent pending status and explicit retry.
  Unchanged failed generations do not retry every frame. Paused play and inactive
  authoring drafts remain separate, and failed play exit preserves both.
- A real SDL software/ImGui test clicks Dismiss and Retry preview, then repairs a
  draft and verifies status clears without saving. The captured error panel was
  inspected. This does not certify every gesture, panel or human editing feel.
- Explicit linked-pose and hierarchy edits keep Transform local and Rigidbody
  authored/live coordinates in world space, preventing later body reconstruction
  from restoring a stale position. Only affected subtrees are synchronized.
  Prepared Tilemap/Animation adoption preserves native identity and callback writes.
- Component key renames preserve active/pending EventBus callbacks, including
  anonymous closures, swapped keys and operations queued during teardown. Direct
  manager renames reject during dispatch and preserve deferred actor activation;
  the reproduced double Update/LateUpdate now executes once. Command-local and
  subtree pose memoization remove measured repeated deep ancestor traversal.
  See phase11-checkpoint.md and ADR 006 for contracts, measurements and limits.

- The isolated LuaBridge3 rc12 experiment now runs against the same C-built Lua
  5.4.9 and actual ScriptBudget. Four standalone checks pass Debug, Release and
  ASan/UBSan; this is separate from the unchanged production 65-case matrix.
  Four actual production TUs pass baseline syntax and fail the candidate with
  23 diagnostics, with eight verified header-isolation checks.
- Executed probes establish required changes to writable fields, checked bool
  extraction, callback return tolerance, explicit error propagation and panic
  restoration. Raw identity, representative generation handles, owned/borrowed
  userdata, nested callbacks, budgets and new-state recovery pass with explicitly
  identified local adapters. Those were isolated tests; the later production integration
  is described below.
  The candidate has C++26 deprecation warnings and remains a prerelease. ADR 007
  selected full integration, subsequently completed in Phase 13.
  See phase12-checkpoint.md and binding-experiment-results.md.

- LuaBridge3 is now integrated into the production engine. LuaBoundary checks
  conversions, preserves nonzero stacks and requested results, ignores lifecycle
  callback return values, and retains error counters with bounded UTF-8 diagnostics
  and tracebacks. All 39 writable member fields remain writable; explicit setter
  and readonly contracts, Lua/TS constructors and generated APIs remain intact.
- The real API test disproved the isolated opt-in assumption: library-wide throwing
  conversion can bypass Lua pcall for a wrong receiver. Production leaves that
  registry mode off, retains C++/safe-C exception support and keeps the allocator's
  panic hook untouched. Manual Actor/Vector2 C metamethods have explicit guards.
- A one-read enabled predicate retains false/true/nil/table policy. A sampled
  Release hotspot then justified the one local equalstates patch: identical live
  state pointers return true immediately, while distinct coroutine/VM pointers
  use the original registry comparison. No assertions or stack checks were removed.
  Same-state, cross-coroutine alias and independent-VM predicate cases pass.
- Final Release/Metal defense and inventory captures were inspected after successful
  runs with empty logs. The unchanged GLM repair component again passes 16 scenarios
  and 96 checkpoints without a new model call. This does not certify complete game
  migration, all editor feel or audio quality. Explicit nil self-receiver crashes
  were also reproduced in the retained 2.7 baseline; Phase 14 resolves the bounded
  ordinary instance-receiver paths described below.
  See phase13-checkpoint.md, ADR 007 and binding-runtime-performance.md.

- Phase 14 guards native instance receivers before member/property/proxy/raw-C
  invocation and converts malformed raw index/newindex shapes to Lua errors.
  Ordinary nullable arguments, static/namespace functions and valid inherited
  receivers retain their semantics. Nontrivial checked-result lifetimes end
  before Lua error jumps. Two new suites cover generic registered bindings and
  35 isolated real-worker cases, including caught/uncaught recovery, no side
  effects from rejected calls and legitimate `SetParent(nil)`. An uncaught script
  error preserves pre-error script changes; no tick rollback is claimed.
  All 68 checks pass in three configurations. Final Metal defense/inventory
  captures were inspected. Arbitrary debug/metatable/upvalue tampering and full
  constructor/coroutine/GC certification remain outside this bounded repair.
  See phase14-checkpoint.md and native-call-receivers.md.

- Phase 15 connects farming, native inventory/equipment, real enemy damage/food,
  paired portals and persistent invasion closure in two resident regions. Crops
  and invasions continue underground; invaders target crops before the altar.
  Clearing a wave schedules another; source destruction permanently cancels the
  route, including active invaders. Checkpoints survive fresh processes and reject
  busy/corrupt/inconsistent input before live mutation. Keyboard/mouse parity,
  full bags, seedless planting, failed-world recovery, missing portals and actual
  incompatible-catalog restore rejection pass through real player input/runtime.
- Integration exposed stale physics activation caches and consumed-but-unrun
  startup. Disabled components now retain pending Start, and existing bodies
  suspend/resume participation without reconstruction. Real cross-owner contact
  callbacks also reproduced two worker crashes. A shared native mutation scope
  rejects direct Rigidbody/Tilemap destruction during those traversals while
  retaining deferred removal and queued cell edits. New activation regressions
  cover lifetime, collisions, identity and subsequent-frame usability.
- All 72 tests pass in Debug, Release and ASan/UBSan. Final software, Metal and
  OpenGL gameplay readbacks pass; village, underground and closure captures were
  inspected. A 78-file source-visible-only copy also passes Metal launch/readback.
  The self-authored test geometry and fixed pool are a bounded game workload,
  with human feel and complete original-game content still unverified. See
  phase15-checkpoint.md, rigidbody-activation.md and the two_world_village CONTRACT.

## Measurements and limits

An isolated, matched-compiler comparison of the hierarchy optimization slice at
5,000 actors measured all-root queries at about 1,662 ms before / 0.85 ms after,
and deep-chain subtree query at about 200 ms before / 0.43 ms after. That comparison
preceded later lifetime/VM edits. The final Release build measures approximately
0.78 ms / 0.41 ms respectively. These are specific former quadratic hotspots,
not whole-frame or cross-engine results.

The earlier Release particle benchmark, with equal chunk tiling and seven workers,
measures about 5.1–6.7x throughput at 8k–256k particles on this machine. The 1k case
remains serial and its difference is noise. Workload counts, medians and p95 are
local under `build/`; no result establishes performance on other CPUs or games.

The matched 256×256 ground-only Tilemap benchmark measures Metal render/present
medians around 87.17–87.50 ms for individual SpriteRenderer actors and 7.88–8.89 ms
for native Tilemap across fixed/scroll/1-edit/64-edit scenarios. Exact final RGBA
pixels match. A fixed small viewport submits 32 visible tiles instead of 65,536
sprites. Small-map/software results are mixed; physics, mixed gameplay and GPU
timestamps are not part of that benchmark. `tilemap-performance.md` gives the
reproducible workload, smaller scales and limits. No cross-engine claim follows.

The initial complete headless animation comparison verifies 1k/10k final actor
states against the same integer loop timeline. At 10k actors, median Engine step
time was about 5.09 ms native and 4.95 ms for a minimal cached-reference Lua loop.
This does not demonstrate a native speedup. The reference omits the broader
native event/lifetime feature set; initial order/cache effects also limit small
differences. `animation-performance.md` documents the workload and next profiling
questions; no rendering or cross-engine claim is made from this test.

After broader host reentry protection, the same 10k Lua loop initially measured
8.124 ms median. A tested raw-table prototype fast path reduced that to 5.687 ms
without removing identity checks or protected executable getters. The matching
native medians were 5.271 / 5.551 ms; small differences do not establish a native
speedup. These are local single-run workload results, not a universal ranking.

Phase 9 retains the phase-8 Release benchmark executable for three interleaved
before/after comparisons (20 warmup / 120 measured ticks, seven workers). At
8k–256k particles, median parallel update changes range from -6.3% to +3.2%, and
serial changes from -11.4% to -3.0%. The 1k serial-fallback case varies around
5–7.4 microseconds and does not support a broad percentage claim. This measures
update work only, excluding creation/burst validation, physics and rendering;
see phase9-checkpoint.md for medians and limits. It establishes no SOTA ranking.

Phase 11 compares the initial transaction implementation against its memoized
version, using matching Release dependencies. A 1,000-actor chain rename changes
from 111.086 to 2.318 ms; reparent from 109.973 to 2.259 ms. A standalone document
root pose edit with a body on every chain node changes from 156.708 to 0.680 ms.
These are bounded command measurements, not net gains over the original engine,
whole-frame timings or cross-engine rankings. Raw profiles remain ignored; see
phase11-checkpoint.md for sampling and workload boundaries.

The Phase 12 isolated binding measurement uses 21 samples per operation across
three interleaved processes, with the same Release Lua/allocator configuration.
Native property read/write pairs measure 100.225 ns baseline / 74.589 ns candidate
median. Other call paths do not demonstrate general improvement; single-TU compile
time and probe executable size increase. Exact distributions, configurations and
limits are in binding-experiment-results.md; no full-engine speed claim follows.

Phase 13's final matched headless animation comparison verifies every actor after
68 ticks in each mode. Native p50 improves about 19–24% at 1k/10k actors; the Lua
loop retains roughly 1.7%/0.9% overhead, with overlapping 10k ranges. Initial 9–11%
Lua regressions, the one-read follow-up, profiling evidence and direct fast-path
attribution are all retained in binding-runtime-performance.md. Five three-pair
batches total 30 successful processes and 120 complete verified workload rows.
These are specific CPU workloads, not rendered frame or cross-engine SOTA results.

Phase 14 compares the final Phase 13 binary/headers with the receiver guards in
three interleaved pairs. All 24 runtime rows and 168 micro samples pass correctness.
Principal p50 ranges overlap; the native-method micro median changes +3.5%, also
with overlapping ranges. These data establish neither general improvement nor
zero cost. See native-receiver-performance.md for the full retained variability.

Phase 15 initially measured a 5.02% 10k native animation p50 increase with
non-overlapping ranges. A no-world early return removes the added activation
scan in those scenes. Final timing ranges overlap in all four workload groups
and have substantial outliers; higher medians remain observed, so elimination
of the earlier regression is not confirmed. All 48 rows across both comparisons
pass correctness. The full results remain in rigidbody-activation.md and ignored
phase15 benchmark directories. No performance gain or SOTA conclusion is claimed.

## Remaining work after an owner-requested resume

0. Modernization slice follow-ups (ADR 008): build Lua with `LUA_USE_POSIX` so
   `pcall` stops saving the signal mask on macOS; add an asynchronous task API
   and connect Box2D's solver; consider Box2D 3.2 snapshots for branching; then
   the command schema, MCP and replay work in `agent-native-modernization.md`.

1. Profile representative mixed gameplay and agent observation costs; recheck
   activation overhead under controlled conditions before broader optimization.
   The two-world fixture now provides a connected workload. Complete scope and
   source rules are in the integration plan and fixture contract. No next slice
   has started: the owner requested this round to be the stopping point.
2. Continue broader world/game migration after that slice. Original item content,
   directional actions, timeline authoring and the complete Unity game remain
   open; see the migration matrix. Lua 5.5 is a separate later experiment;
   arbitrary heap/schema/continuation migration remains unsupported. Graphical
   mode is trusted and unlimited by default; native allocations and script-created
   process trees are outside the Lua allocator/direct-worker limits. Retained
   2.7 and Phase 13 v3 binaries/headers remain under ignored build baselines.
3. Extend typed API coverage against actual bindings and gameplay needs. Extend
   structured diagnostic context, TS source maps and broader authoring
   workflows. The optional TS→Lua slice exists; a separate JS VM and full language
   compatibility are not established requirements or completed capabilities.
4. Validate full interactive editor workflows and clean Windows/Linux packaging.
   Normal edit errors and retained-draft retry now have actual GUI evidence, but
   multi-command gestures, runtime-only property paths and all panels are not
   covered by the scene-command guarantee. A fatal scene-open error can still stop
   the editor before another overlay draw; graphical fatal recovery remains open.
   Vulkan/Direct3D, distributable bundles, audio listening and human game feel remain
   unverified. Existing unused SDL2 vendor files await packaging cleanup.
5. Establish matched representative benchmarks against current established 2D
   engines before making SOTA performance or completeness claims.

## Recovery and artifacts

The verified source/tests/docs form the modernization baseline. Subsequent
features are committed separately after review and appropriate tests. Runtime logs, screenshots,
benchmark output, original-Lua rollback files and external-agent traces stay in
ignored `build/`. `build-and-test.md` and `agent-protocol.md` contain reproducible
commands; `village-defense-playtest.md` describes controls.
`tilemap-farm-playtest.md` and `sprite-animation-playtest.md` cover the new fixtures.
`tests/fixtures/persistent_village/CONTRACT.md` covers saved gameplay and F5/F9.
`typescript-authoring.md` and the typed_gameplay CONTRACT cover optional compilation.
`inventory-playtest.md` covers the latest controls and manual acceptance.
`script-reload.md` and ADR 003 cover the explicit agent/editor hot-reload contract.
`scene-transitions.md` and ADR 004 define prepared scene replacement and persistence.
`native-runtime-validation.md` describes direct native input domains and limits.
`runtime-actor-preparation.md` and ADR 005 cover complete runtime publication.
`editor-command-transactions.md` and ADR 006 cover coordinated authoring edits.
`phase2-checkpoint.md` through `phase15-checkpoint.md` link the implementation contracts.
`binding-experiment-results.md` and ADR 007 record the executed isolated comparison
and the verified integration contract. `binding-runtime-performance.md` records
matched initial/final costs, the local patch and scope limits.
Current local build trees
are `build/sdl3-debug`, `build/sdl3-release` and `build/sdl3-sanitize`; final full-suite
logs use `build/phase15-*-tests.log` and remain ignored. All three builds pass 72
cases, including the 27-case compiler suite and 102+31 exact compiled gameplay
comparisons. Final suite durations were 77.83 s Debug, 51.35 s Release and
162.19 s ASan/UBSan; these are check durations, not performance claims. Final Metal fixture captures are under
`build/phase15-captures`; the earlier editor status capture remains
`build/phase11-editor-edit-status.png`. Prior backend/particle measurements
remain under build/phase9-*. The isolated binding builds use
build/binding-debug, build/binding-release and build/binding-sanitize; their final
four-case logs are build/phase12-*-tests.log, with durations 0.48 / 0.34 / 0.63 s.
Their local measurements and compiler inventory remain under build/phase12-*.
The standalone Debug cases pass again after selecting retained 2.7 headers in
Phase 13. The final GLM revalidation output is under
build/agent-authoring/phase13-release-validation. Raw profiles/comparisons stay
under build/phase13-*. Upstream C++26 deprecation warnings and whitespace are
preserved; non-vendor diff whitespace checks pass.
Autonomous development stops here at the owner's request. The broader goal is
unfinished. The owner authorizes reviewed feature commits after appropriate
tests; no push was requested.

Public-repository hygiene untracks `.vscode` files and the generated root
compilation database while retaining their local copies and ignoring them.
Those removals are included in the repository hygiene commit. Village_Rim source,
its original uncommitted project state and its art experiments remain untouched.
The fixture BMPs/PNGs have narrow ignore exceptions so their self-authored geometry
is included in reviewed feature commits. No generated run transcript is tracked.
