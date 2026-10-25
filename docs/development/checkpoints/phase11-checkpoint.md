# Phase 11: editor command transactions and draft preservation

Local working-tree implementation; no commit or push. The broader Pocket2D and
Village_Rim migration goal remains active. This slice addresses the separate
editor mutation failures recorded at the end of Phase 10.

## Delivered

- Engine stages the complete ordered scene-edit command before publishing actor,
  component, property or hierarchy changes. Late preparation rejection leaves no
  ghost specs, partial actors or delayed starts. Unaffected Actor/Lua/native
  identities remain live; commit-time callback failures have an explicit fatal
  reset boundary instead of an unsupported rollback claim.
- SceneDocument prepares a detached candidate and publishes only after its
  precommit hook succeeds. Failed new edits preserve existing dirty drafts.
  Session hooks coordinate runtime and document, while epoch receipts distinguish
  fresh requests, acknowledgments and expired delivery. SceneView's duplicate
  direct commit and the bootstrap's synchronization-range bypass were removed.
- Existing invalid/unmirrored drafts remain editable through pending repair.
  Diagnostics persist, unchanged failed generations do not retry every frame,
  and explicit retry or a new edit schedules another attempt. Successful mirror
  synchronization does not silently save or clear unsaved work.
- Paused play edits affect the play snapshot. Explicit inactive-authoring edits
  stay draft-only; failed play exit preserves both documents and pause state.
  UI error handling exposes retry for pending work and dismissal for ordinary
  non-pending errors. Runtime-only property/drag paths remain separate.
- Linked pose edits retain local Transform and world Rigidbody conventions on
  the affected subtrees. Reparenting preserves world pose; parent deletion keeps
  the previous local-Transform detach rule and updates resulting body coordinates.
  Subsequent body configuration rebuilds consume the updated authored world pose.
  Unrelated legacy poses are not normalized as incidental cleanup.
- Command-local hierarchy/world/affected caches and document subtree pose memo
  traversal avoid repeated deep ancestor walks. EventBus component-key remapping
  operates on identity deltas over current active/pending entries, preserving
  callback references, order and operations added during teardown. Its final
  shared-build integration passes the complete validation matrix below.
- Direct ComponentManager key renaming uses the same EventBus mapping with a GC
  pause, idle-dispatch requirement and post-publication fatal handling. A real
  deferred-activation regression previously produced starts=1, updates=2, late=2
  after Instantiate → Rename → Finalize → Step. The direct path now leaves pending
  actors out of premature lifecycle-list rebuilding so activation appends them
  once; component_key_rename includes this regression.

## Measured bounded workloads

The following are saved same-machine Release comparisons, not cross-engine
benchmarks, whole-frame timings or CI performance thresholds. The baseline is the
first Phase 11 transaction implementation before its memoization fixes, not the
pre-modernization engine. Raw CSVs, old source
snapshots and local executables remain ignored in build/phase11-scene-edit-benchmark
and build/phase11-doc-profile.

| Workload | Before median (ms) | After median (ms) |
| --- | ---: | ---: |
| Engine command, 1,000-actor chain, rename leaf | 111.086 | 2.318 |
| Engine command, 1,000-actor chain, reparent leaf | 109.973 | 2.259 |
| Engine command, 1,000-actor flat hierarchy, rename leaf | 2.620 | 2.437 |
| Document root Transform edit, 1,000-actor chain, body every 10 actors | 68.876 | 0.296 |
| Document root Transform edit, 1,000-actor chain, body on every actor | 156.708 | 0.680 |

The runtime benchmark measures the complete Engine::ApplySceneEditCommand call,
including snapshot, preparation and commit, with three warmups and seven measured
samples. Fixture construction/startup is excluded; only the before/after SceneEdits
translation unit differs against the same Release dependencies. Its tracked
workload is tests/scene_edit_benchmark.cpp.

The document profile measures three complete standalone SetComponentProperty calls
on root Transform.x and reports their median. It has no live session hook, runtime
commit or GUI rendering. Before/after document sources were compiled with Release
optimization against the same dependencies. These improvements target hierarchy
traversal; general document queries, snapshot costs and GUI workloads remain
separate measurement questions.

## Verification

The final shared sources pass all 65 CTest cases in each configuration: 63 core
cases and two optional TypeScript cases. All three incremental builds also passed.
Logs stay ignored under build/phase11-*-tests.log.

| Gate | Final result |
| --- | --- |
| Debug CTest matrix | 65/65 passed, 40.67 s |
| Release CTest matrix | 65/65 passed, 33.79 s |
| ASan/UBSan matrix | 65/65 passed, 67.48 s; ASAN_OPTIONS=detect_leaks=0 |
| Bounded status-window GUI evidence | Actual SDL software ImGui Dismiss/Retry clicks and visually inspected capture |

LeakSanitizer is unavailable on this macOS setup; no leak-sanitizer success is
claimed. Suite durations are verification costs, not performance benchmarks.

New dedicated correctness cases are scene_edit_runtime,
scene_document_transaction, editor_command_transaction, prepared_native_adoption,
editor_edit_status and component_key_rename. Existing event_bus and scene_transition
coverage was adapted to first require rejection of a bad edit on a synchronized
document, then explicitly construct a detached invalid draft for the original
failed Enter/Exit/scene-selection/save-gate checks. Those old assertions were not
removed to make the new hook pass.

The tests inspect real runtime/document/session behavior, native bodies, Lua/nested
identities, pending queues, callback counts, key-remapped events and draft state.
UI source inspection found no retained document-record/property borrow across
candidate swaps in the reviewed panels; this is not a GUI lifetime stress test.
Separately, editor_edit_status drives actual ImGui Dismiss and Retry preview
buttons on SDL's software renderer and checks repaired-draft status. The inspected
capture at ignored build/phase11-editor-edit-status.png shows the retained-draft
message, concrete native validation error and visible Retry action. This is bounded
click/readback evidence, not coverage of all panels, window sizes or human editing
feel. The standalone component-key regression first reproduced duplicate updates
against the old direct path, then passed with deferred activation preserved.

See [ADR 006](../adr/adr-006-editor-command-transactions.md),
[the command contract](../editor-command-transactions.md), and
[the preceding checkpoint](phase10-checkpoint.md). Full gameplay migration,
packaging, broader native/API coverage and matched cross-engine performance and
completeness evaluation remain open.
