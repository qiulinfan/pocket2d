# Phase 10: whole runtime actor preparation

Local working-tree implementation; no commit or push. The complete Pocket2D and
Village_Rim migration goal remains active. This slice fixes runtime generation
and duplication without claiming that every editor mutation path is transactional.

## Delivered

- Lua Instantiate and native runtime duplication share a move-only prepared-actor
  token. Every component, default graph, native specification, root pose and
  relevant peer/resource relationship is checked before a candidate joins the
  world. Rejected detached records never execute gameplay OnDestroy.
- Tilemap data and SpriteAnimation assets initialize inside detached native objects.
  They do not replace outer scene-preparation caches or create physics bodies.
  Publication binds all peers and applies prepared animation poses without reading
  those resource files again. Tests delete source assets after preparation.
- Successful publication gives the actor a generation, registers stable identities
  and schedules existing lifecycle behavior. Instantiate remains immediately
  findable with deferred activation; host duplication retains immediate index
  registration. Published-before-start destruction still invokes OnDestroy.
- Hash-based UID checks and incremental index updates avoid full-world binding
  rebuilds. Monotonic actor-order reservations plus conditional ordered insertion
  preserve update, late-update and animation/physics traversal order when deferred
  and immediate publication are mixed, including after earlier actors are removed.
  Host copy-name selection retains its prior scan; no universal constant-time
  duplication claim is made.
- Failed template-cache misses do not publish bad template data. Repairing an
  uncached rejected file can succeed on retry; previously successful cached
  templates retain their lifetime policy. UID and generated-key gaps are allowed.
- Host duplication returns false and a bounded diagnostic on normal rejection,
  preserving its output UID and editor selection. The Hierarchy panel can show
  that diagnostic. Unexpected post-publication failures stop the runtime and use
  the agent actor_publish_failed/requires_reset boundary. Main-VM/native allocator
  failure is not advertised as a full rollback transaction.

## Verification

All **59 CTest cases pass in Debug, Release and ASan/UBSan**, comprising 57 core
cases and two optional TypeScript authoring cases. Final suite durations are
38.63 s / 32.60 s / 62.06 s respectively; these are verification durations, not
performance measurements. macOS LeakSanitizer remains unavailable, so the sanitizer
run uses ASAN_OPTIONS=detect_leaks=0. New cases are
runtime_actor_preparation, runtime_actor_assets, runtime_actor_spawn and
agent_actor_spawn.

The former late-failure reproductions now assert no partial actors/records/queues,
no callback side effects and unchanged old Actor/Lua/native/body identity both at
return and on later ticks. Valid cases cover immediate lookup, frame-zero resource
state, generation separation, nested/moved GC guards, source repair/retry, creation
from OnStart/OnUpdate/OnDestroy, both host-idle scheduling modes and mixed ordering.
Existing UID wrap, hierarchy, lifetime, asset, reload and gameplay regressions
remain enabled. A synthetic host fatal notification verifies that later callbacks,
frame phases and future steps stop; it is not allocator fault injection or proof
of every catastrophic failure path.

A 180-frame Release/Metal village-defense smoke also completes successfully;
the inspected capture contains the player, village landmarks and two spawned
invaders. This proves the bounded draw/start workload, not human game feel.
Logs and captures remain ignored under build/phase10-*. The actual outer editor
failure interaction and diagnostic readability still require a manual UI check.

## Verified remaining editor work

A separate read-only harness linked to the current Debug engine/editor libraries
reproduced three distinct authoring-path defects, preserved locally under
build/phase10-editor-mutation-audit:

- Engine scene-edit CreateActor with a later rejected component still publishes
  a partial actor; its next-tick OnStart changed old source health 100→90.
- Editor AddComponent rejection leaves an authoring spec without a live component.
  The real EditorSceneSession fallback reload rejects the now-invalid document;
  the dirty mirror flag then clears, leaving the document/runtime mismatch.
- Engine and SceneDocument two-mutation commands can return false after retaining
  an earlier rename. Their atomic/rollback comments overstate current behavior.

These are separate entry points from the fixed runtime APIs. The next slice must
address document and runtime command preparation/commit together, retain valid
incremental-edit behavior and define irreversible callback failure handling. It
must not claim repair by merely changing the comments or discarding the user's
authoring draft. Broader scripting, gameplay, packaging, human testing and matched
cross-engine performance/completeness gates remain open.

See [ADR 005](adr-005-runtime-actor-publication.md),
[runtime actor contract](runtime-actor-preparation.md), and [status](status.md).
