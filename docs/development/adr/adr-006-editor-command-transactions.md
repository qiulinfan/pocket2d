# ADR 006: coordinate editor document and runtime command transactions

## Status and context

Accepted under the owner's autonomous Pocket2D development direction, 2026-10-15.
Implementation is in the local working tree; all 65 Debug, Release and ASan/UBSan
cases pass as recorded in [the Phase 11 checkpoint](../checkpoints/phase11-checkpoint.md).

The Phase 10 editor audit reproduced three failures outside the fixed runtime
Instantiate/duplicate paths: rejected component addition left an authoring spec,
rejected actor creation left a findable partial actor whose next OnStart damaged
existing gameplay, and a failed batch retained an earlier rename. The document
could already contain the bad edit, so fallback scene loading could not repair it.

This decision concerns Pocket2D's C++26 editor/runtime, existing Lua component
records and Box2D bodies. It builds on [stable lifetimes](adr-002-lifetimes-and-parallelism.md),
[scene preparation](adr-004-scene-preparation.md) and
[detached actor publication](adr-005-runtime-actor-publication.md). It introduces
no engine replacement, new dependency, ECS or asset/save format migration. Its
product requirement is reliable editing of the existing playable fixtures and
migration work, not a new gameplay rule or a claim of completed Unity migration.

## Decision

Prepare each complete SceneEditCommand before changing the live world. Engine
simulates its nine supported mutation kinds in order on a command-local candidate,
validates the resulting hierarchy/configuration/assets and prepares new or
replacement records while retaining existing identities where appropriate.
Only then does it commit. Ordinary rejection returns false with
GetLastSceneEditError and leaves actors, authoring specs, Lua/native records and
lifecycle queues unchanged. Successful incremental edits do not reset the VM or
restart unaffected gameplay.

SceneDocument likewise prepares a detached candidate for every public edit entry.
A precommit hook lets EditorSceneSession commit the runtime transaction before
publishing the document through a noexcept state swap. Hook rejection preserves
the old document, including unrelated dirty work. Copies/moves into new document
objects detach hooks; assignment preserves the destination's hook. The session
rebinds its own document roles on scene/mode changes.

A coordinator epoch receipt prevents duplicate UI delivery. Zero means a fresh
request, the current epoch means acknowledge, and a different nonzero epoch means
reject. A raw HandleSceneEditCommands call combines its fresh commands into one
ordered batch; commands already committed by document hooks are acknowledgments.
SceneView no longer applies the same generated command directly to Engine, and
the bootstrap no longer guesses synchronization from a range of vector indices.
Receipts are delivery metadata, not persisted IDs or permission tokens; delivered
command payloads must remain immutable.

Existing unmirrored drafts have a separate repair mode. Structurally valid edits
can update that draft while the old runtime remains intact. A full prepared load
attempts synchronization, retains errors/pending state on failure and suppresses
retries for an unchanged failed generation. New edits or explicit Retry permit
another attempt. Repairing several errors does not require fixing them all in
one operation or discarding unrelated unsaved work.

Play uses its active mutable snapshot. Explicit edits to inactive authoring data
are draft-only and never patch play. Failed exit retains both drafts and pause
state so the authoring snapshot remains repairable. Independent document copies
cannot accidentally commit into the session's Engine.

## Pose and subscription consistency

Transform pose fields remain local to their parent; Rigidbody pose fields are
world coordinates. Explicit Transform/body pose edits and hierarchy changes
synchronize the affected subtrees' body specifications and live body positions.
A world-space body edit derives the corresponding local Transform; reparenting
preserves world pose. Deleting a parent retains the existing local-Transform
policy for detached children and updates their resulting world body pose. This
prevents a later width/configuration rebuild from teleporting a body back to a
stale authored position. Unrelated legacy actors are not globally normalized.

Renaming a component preserves its Lua instance. A prepared EventBus identity
mapping updates active subscriptions and pending operations at commit, matching
component identity, actor UID and old key. It does not replace a bus snapshot or
change callback references/operation order, so subscriptions or cancellations
added during teardown are retained. Preparation requires an idle dispatch boundary;
post-publication failures remain fatal rather than claiming rollback. The direct
ComponentManager::RenameComponentKey entry uses the same mapping with a GC pause
and dispatch rejection. Its post-publication failures use the actor_publish_failed
reset boundary. Renaming a deferred actor does not prematurely rebuild its
lifecycle entries: frame-end activation must append Update/LateUpdate exactly once.

## Alternatives and consequences

- Rebuild the entire runtime after every edit: simple, but destroys live state,
  native identities and useful incremental editing; invalid drafts still cannot
  reload. Full replacement remains explicit for a pending mirror.
- Apply mutations and undo after failure: cannot reliably reverse gameplay
  callbacks, native teardown or external I/O. Rejected candidates must not be
  published merely to destroy them afterward.
- Roll back the whole Lua heap/engine: substantially broader than this problem
  and still cannot undo arbitrary external effects. Use bounded preparation and
  an explicit fatal commit boundary instead.

The selected design costs candidate copies, validation and command-local indexes.
It preserves state but does not make every command proportional only to its changed
actors. Runtime hierarchy validation, world-pose lookup and affected-ancestor
checks memoize within a virtual mutation; caches are discarded when the candidate
changes. Document body-pose synchronization builds a local UID/parent graph and
iteratively memoizes affected poses. These changes remove repeated ancestor walks
without retaining stale caches across edits. The document optimization does not
replace its general hierarchy/world-query implementation.

Unexpected commit callback/setter/allocation failures require a fresh runtime;
they do not promise a document-and-world rollback after irreversible work starts.
Existing body reconfiguration can legitimately rebuild a native body. Runtime-only
Inspector/drag APIs, Tilemap asset drafts, saving, and multi-command UI gestures
remain separate contracts. Per-command safety does not make a whole XY drag or
resource/row/column gesture atomic after its individual edits already committed.

## Verification and rollback

Use scene_edit_runtime, scene_document_transaction, editor_command_transaction,
scene_transition, event_bus and component_key_rename regressions, plus the existing lifetime, hierarchy,
asset, reload and agent suite. Cover failed late components, no delayed callbacks,
retained identities/dirty drafts, receipt copies/staleness, pending repair,
paused authoring/play separation, linked body poses after rebuild, ordered key
swaps, deferred activation after direct key rename and event operations created
during teardown. The separate editor_edit_status test exercises actual ImGui
Dismiss/Retry clicks; it does not certify every editor interaction. Measure the bounded Release
workloads separately from CTest correctness. Final results belong in the checkpoint.

Do not revert only the hook or only the SceneView delivery change: that would
restore partial document/runtime publication or double execution. Any rollback
must preserve a single command delivery path, retain drafts and diagnostics, and
run the same regressions. The narrower [command contract](../editor-command-transactions.md)
records the public behavior and remaining limits.
