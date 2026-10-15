# Transactional scene selection and per-attempt failures

Phase 8 strengthens the boundary between validating a requested scene and
replacing a running world. It does not make arbitrary script or host side effects
reversible after commit begins. The editor and agent use the same Engine scene
preflight/commit path and retain different responsibilities for presentation.

## Preparation before old-world mutation

A candidate scene's structure, component types, script declarations, native
property constraints, templates and referenced Tilemap/SpriteAnimation assets
are prepared before old actors/components, input, audio or physics are changed.
This asset preflight covers those two native systems. Other resources loaded
lazily, including ordinary sprite textures, audio and fonts, can still be resolved
later during runtime use.

Ordinary rejection returns false and preserves the current scene, actor
generations, live component
state, native inventories/bodies, named subscriptions and pending lifecycle work.
Direct `LoadSceneAsset` does not advance gameplay time.

Trusted startup component sources execute once, after the initial scene's source
preference is known. Script filenames matching the six native component names
`Rigidbody`, `ParticleSystem`, `Transform`, `SpriteRenderer`, `Tilemap` and
`SpriteAnimation` are skipped. Other selected script filenames that collide with
host or standard-library globals captured after API registration, such as
`Inventory`, `Vector2` or `math`, are rejected. The check covers declaration
filenames; trusted runtime code retains its ordinary Lua permissions.

Ordinary scene transitions reuse a known exact source path; editing that file
does not silently execute it again against the old live world. Use explicit `reload_scripts` for a
code edit, or reset for a fresh VM. A new declaration or a different preferred
same-stem source is prepared in isolation and only installed on successful scene
commit. Returning to a known source uses its cached declaration. New declarations
must fit the bounded staging contract; host effects belong inside runtime
callbacks, not candidate top-level evaluation.

This separation prevents a failed missing-component scene from re-running old
top-level code that changes globals, consumes RNG, writes saves, teleports a body
or destroys another actor. Existing `OnStart` is not re-run during rejection.
New scene instances receive their normal first start after a successful commit.

Each `BuildRuntimeActors` preparation resolves template names into a local cache
keyed by canonical template path. Repeated references within that preparation
use one template snapshot, and each resulting actor receives independent
component specifications. A subsequent preparation reads a fresh snapshot from
disk. These local snapshots neither read from nor replace `Actor::LoadTemplate`'s
separate live-spawn cache. Invalid candidates therefore cannot refresh or pollute
that cache. An actor-template JSON root must be an object; an array root is
rejected explicitly. The local cache is an implementation observation, without a
claimed file-read measurement or performance benchmark.

`Scene.DontDestroy` remains actor-level. Incoming UID collisions with retained
actors are rejected. A retained actor whose parent would be removed is rejected
instead of implicitly marking descendants, renumbering identities, reparenting or
changing world pose. Authored parent links must resolve within the incoming
scene, rather than attaching to retained actors from the previous scene. A
preferred source change for a type used by retained actors is also rejected.
Preservation is explicit rather than inferred from a tree.

## Editor document transaction

`EditorSceneSession::OpenSceneFromPath` first builds a candidate SceneDocument.
It asks Engine to prepare/load that candidate before replacing the current
authoring/play documents or mode flags. An invalid candidate keeps the previous
selection, dirty authoring draft, play draft and paused state intact.

Successful Open retains the existing outgoing-draft autosave policy through a
synchronous host `before_commit` gate. Engine invokes the gate only after all
candidate preflight succeeds, and before input/audio/world mutation. The gate
saves the old dirty authoring document; a failed save rejects the transition.
Invalid target scenes never invoke that save gate. Only a successful Engine
commit swaps documents and leaves play mode. Enter Play and Stop likewise change
their document/mode state only after their target runtime load succeeds.
The editor host clears only actor selection after a successful open. Failed opens
keep selection and report the current failure; a file opened in an external
editor leaves the current selection alone. After a postcommit fatal failure,
restart the editor to create a fresh Engine; opening another scene cannot repair
the damaged Engine. Agent sessions recover through `reset`, which creates a fresh
worker. The notification itself does not clear independent map drafts or other
project UI caches.

The gate is an owner-thread host operation, not a Lua callback or a general
transaction plugin. Reentrant gameplay advancement is rejected while the scene
transaction is active. A nested `LoadSceneAsset` also fails; a gate returning that
failed result rejects the outer operation. These precommit failures leave frame,
world and reset-required state unchanged, and the guard is released so a normal
later tick can proceed. The gate avoids moving a potentially failing outgoing
file save after an irreversible world replacement. Explicit editor save/shutdown
behavior outside scene switching is unchanged.

## Deferred gameplay loads and agent responses

`Scene.Load(name)` queues a transition for the next frame boundary. A failed
preflight leaves the old world available to execute that ordinary gameplay tick;
it is not a promise to freeze movement for the whole requested tick. Integrity
checks compare its motion/update count with one normal old-world tick.

Engine exposes a monotonic `GetSceneLoadAttemptRevision()` for actual attempts
and retains `GetLastSceneLoadError()` as the last diagnostic. The agent compares
the revision before/after each step. Only a new failed attempt interrupts the
current request with:

```
{"ok":false,"code":"scene_load_failed","error":"...","frame":4,
 "requires_reset":false}
```

The frame reports the real completed boundary/tick. Later legitimate requests can
run their full frame count; an old nonempty diagnostic is not thrown again on
every tick. `observe` still reads the preserved world. A new successful load
clears the last load error through the Engine's ordinary success path.

Unexpected errors after commit starts, including reported teardown failures,
do not claim rollback. Engine stops the damaged runtime, clears unsafe indexes
and exposes `RequiresSceneReset()`. The agent reports `scene_load_failed` with
`requires_reset:true`, ends that worker, and requires an explicit reset. Tests use
a finite throwing `OnDestroy` callback to verify this classification and recovery;
they do not claim rollback under destructive allocator or process failure.

## Regression workloads

`scene_transition_test` creates an isolated project and directly checks failed
asset loads preserve frame, generation, live Lua/native identity, health, RNG,
subscriptions and save-file absence. A following tick checks for delayed actor
destruction. It also verifies edited known source paths are cached, and exercises
real EditorSceneSession open/enter/exit flows with dirty and paused documents.
A deterministic file-path obstruction makes outgoing autosave fail, checking
that the host gate rejects before replacing the old world. The same test covers
`StepSimulation` and nested scene-load attempts from `before_commit`, including
normal gameplay continuing after rejection.

Its template cases seed the live-spawn cache with health 11, change disk content
to 22, and build 1000 actors from the current preparation snapshot. Mutating one
actor's specifications must leave all others unchanged. Another preparation sees
a later disk value of 33, while live spawning still sees its cached value of 11.
A rejected late missing-component candidate preserves both cache and old world;
an array-root template must fail before commit.

`scene_persistence_test` exercises queued loads with a parent, child and victim.
It rejects child-only persistence with a removed parent, incoming UID collisions
and incoming authored parent links to retained actors. A valid merge retains
actor generations, parent links, Lua/nested state and native physics components,
destroys only the nonpersistent victim and starts the incoming actor once. A
preferred same-stem script change is rejected while retained actors depend on
the old source; clearing their persistence flags permits the valid transition.
The test also calls data-only `Scene::LoadScene` and checks that it returns records
without running/registering scripts, changing source preference or advancing a
runtime attempt/tick.

`agent_scene_transition_test.py RUNTIME` uses real supervisor/worker processes and
Lua `Scene.Load`. It checks exactly one failure response followed by a full
three-frame request, isolated late failures in new declaration batches, preferred
same-stem sources, explicit versus implicit code reload, initial source preference,
single startup evaluation, native-reserved filenames, RNG and native state. A
postcommit teardown error must require reset, and a repaired script must recover
in a fresh worker. All projects, corrupt candidates and save roots are temporary.

The real Session regression covers document/runtime failure preservation. The
outer Project-panel double-click → external editor/scene-open selection flow is
not automatically exercised by that headless test; actor-selection retention and
failure-notice readability remain a specific manual UI check.

These sections describe implementation boundaries and regression workloads.
Final Debug/Release/sanitizer results are reported separately; these workload
descriptions do not by themselves establish a completed validation matrix.

A fatal stopped runtime can exit the editor loop before the next overlay draw
when no map draft keeps it open. The fatal scene-open diagnostic is also emitted
through SDL logging; persistent graphical fatal-state presentation remains an
editor usability task. Reopening a scene in the same failed Engine cannot repair it.
