# Editor command transactions

This slice coordinates scene-backed editor commands across `SceneDocument`, `EditorSceneSession` and `Engine`. A rejected new edit must leave the previous document, including unrelated unsaved work, and its healthy runtime mirror unchanged. It is separate from runtime-only actor manipulation, map-asset drafts, script reload, scene-file selection and saving to disk.

## One command, one coordinated commit

`SceneDocument` applies a complete `SceneEditCommand` to a detached candidate. Structural failures or preparation exceptions reject the whole command. Its optional precommit hook receives the candidate and command before publication. If the hook rejects or throws, the document keeps its old records, dirty state and allocation state; successful publication swaps the candidate into the document. A valid candidate is not persisted to disk automatically.

The session binds this hook to its documents. For the active document with a synchronized runtime mirror, the hook asks `Engine::ApplySceneEditCommand` to prepare and commit the runtime change first. A normal runtime preparation failure rejects the document candidate too. Existing live actors/components that do not require replacement retain their identity and gameplay state; a valid rename does not restart `OnStart`, advance time, or rebuild the whole scene.

Engine commands are ordered mutation batches. Validation must consider their candidate result rather than publish the first mutation and hope later ones succeed. In particular, late invalid component defaults must not leave a ghost specification, a partially registered actor or a queued `OnStart`. The same rule applies when an earlier rename precedes a later failing mutation.

The guarantee covers ordinary validation/preparation rejection. Unexpected failures after a runtime commit begins are a reset boundary, not a guarantee of arbitrary allocation rollback. Likewise, a later gameplay callback on a successfully published component is outside preparation.

## UI delivery and receipts

Hierarchy, scene-backed Inspector and SceneView mutations all use the bound document entry points. SceneView must not independently commit their generated commands to Engine a second time. The old range of commands marked “already runtime synced” is replaced by coordinator receipts.

`SceneEditCommand::coordinator_receipt` is session-owned delivery metadata. The hook stamps an accepted command, including an accepted pending-draft repair. The bootstrap can pass that command, or a copy of it, to `HandleSceneEditCommand`; a receipt from the current coordinator epoch acknowledges the completed operation rather than executing it again. Command payloads are immutable after delivery. A caller that intentionally invokes the document's `ApplyEditCommand` again requests a new edit and resets the receipt for that attempt.

A zero-receipt command passed directly to `EditorSceneSession::HandleSceneEditCommand` is a new programmatic edit: the session applies it through the active document and its hook. It is not a runtime-only patch to a document that the caller must secretly mutate first. A nonzero receipt from another coordinator epoch is rejected, including delayed delivery after entering/leaving play or opening a scene; it is never silently replayed as a fresh request.

`HandleSceneEditCommands` acknowledges commands already committed in the current epoch and combines all fresh zero-receipt commands, in order, into one mutation batch. That fresh batch is prepared and committed once. Any expired receipt rejects the delivery before the fresh commands execute. UI commands that already committed separately through their document hooks remain acknowledgments; delivering their receipts together does not retroactively turn those edits into one transaction.

Some UI gestures currently produce several commands, such as an image drop updating resource, row and column, or a drag updating X and Y. Per-command atomicity does not claim that an entire multi-command gesture is one transaction.

## Existing invalid or unmirrored drafts

An editor can load a document that is not yet representable by the healthy runtime. Rejecting every edit until all of its errors are fixed in one operation would make that draft difficult to repair. The session therefore distinguishes pending-draft repair from a new edit to a synchronized baseline.

While a mirror is pending, structurally valid document edits may commit to the draft without changing the old live world. `IsRuntimeMirrorPending()` remains true. `SyncRuntimeMirrorIfDirty` attempts a complete prepared scene load; rejection preserves the draft and old runtime and exposes `GetLastEditError()`.

The session suppresses repeated attempts for the same failed draft generation. A new edit, an explicit dirty notification or `RetryRuntimeMirror()` permits another attempt. Suppression does not clear the pending/error state. Fixing two independent errors in successive edits is supported: the first repair can leave the draft pending; the final valid draft can synchronize. Synchronizing does not save the document or clear its unsaved-work flag. `ClearEditError()` dismisses an ordinary non-pending error; it does not clear an unresolved pending mirror or its diagnostic.

## Play and authoring documents

The active document during play, including paused play, is the play snapshot. Scene-backed edits there affect that snapshot and its live runtime; they do not overwrite the authoring document. Editing does not unpause play or advance a frame.

The explicit mutable `GetAuthoringSceneDocument()` accessor remains available during play for draft-only work. Its edits never patch the play runtime or change the active play mirror's pending state. This permits repairing the authoring snapshot when it cannot yet be restored. `ExitPlayMode` validates the full authoring candidate: failure preserves both drafts and the current play/pause state; successful exit restores the authoring scene.

Independent document copies have detached hooks, so editing a copy cannot accidentally commit into the session's Engine. Session document replacement and mode changes must bind the intended document again rather than inherit a callback that points to a different owner or mode.

## Linked poses and component-key subscriptions

Transform coordinates remain local to their parent; Rigidbody coordinates are world-space authoring data. Explicit pose changes and hierarchy edits synchronize the affected subtrees' body specs and live positions so a later width/configuration rebuild does not reuse a stale position. Editing a body world axis derives the corresponding local Transform. Reparenting keeps world pose; deleting a parent retains the existing local-Transform detach rule and updates the resulting world body pose. This is scoped synchronization, not a normalization pass over unrelated legacy actors.

Component key changes retain the same Lua instance. A prepared EventBus mapping updates matching active subscriptions and pending operations by component identity, actor UID and prior key. It preserves callback references and operation order, including changes enqueued during teardown. Mapping preparation requires idle event dispatch; a failure after commit begins uses the fatal/reset boundary. The direct `ComponentManager::RenameComponentKey` path shares the mapping and holds the main-VM preparation GC pause. It skips lifecycle-list rebuilding for actors still awaiting activation, preventing duplicate Update/LateUpdate entries. Unexpected direct-path publication failures use `actor_publish_failed` with reset required.

## Explicit boundaries

Runtime-only Inspector properties and SceneView movement still use direct component APIs. Hierarchy operations on transient runtime actors use the runtime duplicate/delete APIs. They do not create authoring commands and are not covered by the document transaction guarantee. The separate Tilemap asset document also has its own draft/save/discard contract.

Loading/saving a document, opening another scene, resource I/O and runtime gameplay callbacks have their own contracts. A successful coordinated edit is not a disk save, a full Lua heap rollback facility, or proof that all editor interactions are transactional.

## Verification

`tests/editor_command_transaction_test.cpp` uses real Engine, SceneDocument and EditorSceneSession objects with a temporary project and real Box2D body. It checks:

- failed AddComponent and late-failing CreateActor leave no specs, records, actors or delayed lifecycle effects;
- failed batches keep the old name and native configuration, while a valid in-place rename retains Actor/Lua/nested/native-body identities;
- a failed new edit preserves an already dirty document;
- copied receipts do not duplicate creation, expired receipts cannot mutate a later play/authoring world, and raw session commands update document and runtime together;
- paused play edits preserve the authoring draft, explicit inactive-authoring edits remain draft-only, and an invalid authoring snapshot can be repaired after a failed exit;
- a multiply invalid initial draft stays pending with a persistent diagnostic, avoids automatic retry loops, supports explicit retry and successive repair, then synchronizes without discarding unsaved work.

The lower-level document and runtime transaction tests cover their own mutation and hook contracts. The integration test exercises public coordination APIs; it does not substitute a copied coordinator implementation or itself claim automated GUI click coverage. Separate `editor_edit_status` coverage clicks the actual ImGui Dismiss and Retry preview buttons with SDL software rendering, checks repair/status transitions, and has an inspected capture at ignored `build/phase11-editor-edit-status.png`. This bounded UI evidence does not certify every editor interaction. `component_key_rename` checks the direct manager path, including deferred activation without duplicate updates. Final Debug, Release and sanitizer matrix results belong to the coordinated validation run and are not implied by this document.
