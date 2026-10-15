# Runtime actor preparation

This slice covers Lua `Actor.Instantiate(template)` and the host/editor runtime operation `Engine::DuplicateRuntimeActorByUID`. They share a detached preparation path before publishing an actor to the live scene. It also regression-tests the existing Lua `Actor:AddComponent` preparation failure path. It does not make all editor scene mutations transactional; the separate `Engine::applyAddComponentMutation` authoring path remains a follow-up boundary.

## Preparation and publication

`PrepareRuntimeActor` owns a temporary actor node and freshly prepared component records. It checks registered types, unique component keys, cloned Lua defaults, native configuration, applicable pose constraints, and Tilemap/SpriteAnimation resource and peer requirements. Unsupported userdata in Lua default graphs is an ordinary preparation failure. The token keeps the main Lua VM's preparation GC pause alive until its records are released or published.

At this stage the candidate is absent from `Actor.Find`, live actor/component indexes, activation queues and lifecycle lists. No gameplay `OnStart`, `OnUpdate` or `OnDestroy` callback runs. Disposing a rejected token destroys only its detached records; it must not route through ordinary live `Actor.Destroy` cleanup. Otherwise an earlier valid component's `OnDestroy` could mutate the original world despite the actor never having been published.

Tilemaps load and validate their data, origin, extent and collision configuration while detached; Box2D bodies are created on the normal subsequent startup path. Animation preparation checks the target SpriteRenderer and competing animators, loads the atlas/clip data, and prepares the initial timeline. After publication binds every component to the new actor, it applies the prepared animation pose without reading files again. These steps do not replace the global resource preparation caches used by an enclosing scene transition. Ordinary SpriteRenderer images, audio, fonts and other lazy resources are not all eagerly validated by this transaction.

`PublishRuntimeActor` consumes the prepared actor once, assigns its runtime generation, splices its stable list node into the live scene, and installs its records and indexes. Existing actors are not rebound through a whole-scene `BindActorsForScene` pass. The returned actor can immediately be found and its components inspected; this does not mean its gameplay callbacks have started.

UID, generation and dynamic component-key reservations may have gaps after failure. Callers must treat them as identities rather than contiguous counters.

## Failure contract

| Entry point | Ordinary preparation failure | Successful publication |
| --- | --- | --- |
| Lua `Actor.Instantiate` | Throws a bounded Lua-visible diagnostic, catchable with `pcall`; no partial actor is published. | Returns a live Actor handle with component access and initial prepared animation pose. |
| Host `DuplicateRuntimeActorByUID` | Returns `false`, leaves the output UID untouched, and records `GetLastRuntimeActorError()`. | Returns `true` and writes the new identity. |
| Lua `actor:AddComponent` | The tested unsupported-default failure throws before installing records or lifecycle work; a reserved key may be skipped. | Uses the existing deferred startup contract. |

The guarantee concerns validation and preparation rejection. Once publication has begun, an unexpected host allocation or installation exception is a fatal/reset boundary; there is no claim of arbitrary host-memory rollback. The engine marks reset required and stops normal simulation. Agent use requires reset; an editor host needs a fresh engine/runtime. Candidate lifecycle code that fails later in normal `OnStart` is also outside the preparation transaction: successfully published gameplay is already live.

## Template-cache behavior

`Actor.ReadTemplateForSpawn` reads an uncached candidate without inserting it into the live spawn-template cache. `Instantiate` commits that candidate only after complete actor preparation succeeds. A bad uncached template can therefore be fixed on disk and retried in the same process. Successfully cached templates retain the existing cache behavior; editing one does not promise automatic reload on the next spawn. Scene preparation's local template snapshots are a separate mechanism, described in [scene transitions](scene-transitions.md).

## Lifecycle ordering

An actor instantiated during `OnStart` or `OnUpdate` is immediately visible, but does not recursively run its own `OnStart` inside the creating callback. Its startup and updates occur in the subsequent frame's normal pipeline. An actor spawned from `OnDestroy` likewise follows deferred startup, with activation and cleanup queues processed by their existing frame stages.

The host-idle entry points intentionally retain distinct scheduling:

- `Actor.Instantiate` uses deferred actor activation: the next step starts its components, then frame-end activation makes their updates eligible for the following step.
- Host runtime duplication registers its lifecycle indexes immediately: the next step runs `OnStart` before its first `OnUpdate`.

Destroying a successfully published actor before its first `OnStart` retains the established `OnDestroy` behavior. A fix must distinguish this case from a rejected detached candidate rather than suppressing all destruction callbacks for unstarted actors.

## Regression coverage

`tests/runtime_actor_spawn_test.cpp` constructs a temporary project and checks actual actor/component indexes and pending queues. Its rejection matrix includes late unsupported Lua defaults, unknown types, invalid Rigidbody/ParticleSystem configuration, missing map/animation resources, missing renderer targets and competing animators. It compares the original Lua instance, native userdata and real Box2D body across rejection and the next tick. The host duplicate test requires a recoverable error and no discoverable partial copy, then exercises a valid duplicate. Additional assertions cover template repair/retry, an immediately initialized animation, real deferred body creation, and the published-before-start destruction distinction.

`tests/agent_actor_spawn_test.py` uses the real supervisor/worker protocol and generated temporary project files. Real Lua input callbacks attempt invalid spawns, inspect immediate results and continue stepping. The replay checks original health and actor identities, absence of rejected candidates in subsequent observations, creation from `OnStart`/`OnUpdate`/`OnDestroy`, delayed startup, valid AddComponent after failure, template repair and fresh reset. The atlas used for peer validation is a self-authored one-pixel BMP; no Unity art is imported.

Build and run these through the repository's CTest registration. Their presence and assertions document the intended checks; final Debug, Release and sanitizer results are reported by the coordinated build run, not inferred from syntax checks or this document.
