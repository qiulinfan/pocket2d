# Rigidbody activation

The two-world fixture exposed two independent lifecycle bugs. A Rigidbody
authored with `enabled=false` could be enabled by another component's `OnStart`
yet remain stationary. The physics hierarchy had already cached the disabled
state, and the Lua setter did not invalidate that cache. Physics consequently
treated the body as Transform-owned and repeatedly restored its old position.
Separately, `ProcessPendingOnStart` marked disabled components as started before
the callback dispatcher skipped them. Later activation could never create their
native body or run their Lua `OnStart`.

## Runtime contract

- Disabled components remain pending until an existing frame-start lifecycle
  boundary observes them enabled. Their `OnStart` runs once. Enabling a component
  after that boundary starts it on a subsequent tick; activation does not insert
  callbacks into an already-running update traversal.
- A component that has already started is not started again when reenabled.
  Its existing Rigidbody userdata and Box2D body are retained.
- Lua writes that change `Rigidbody.enabled` or `body_type` invalidate the engine's
  physics hierarchy cache. Invalid values still fail before assignment; assigning
  the same value does not dirty the cache.
- The lifecycle bridge applies effective body type and Box2D enabled state at
  frame start and again before physics. Changes made during `OnStart`, `OnUpdate`
  or `OnLateUpdate` therefore reach the next physics step. Disabling an existing
  body removes its contacts and simulation participation; reenabling restores
  participation without reconstructing its fixtures.
- Dynamic bodies write their simulated world pose back to Transform. The
  existing non-dynamic hierarchy behavior remains Transform-owned. Authoring
  flags are visible immediately, but Box2D mutation waits for these safe boundaries.

`SetVelocity` before the first native `OnStart` remains a no-op. It is not a queued
velocity request. Gameplay should issue movement after the body's startup
boundary, rather than manually invoking lifecycle methods to force initialization.

## Contact callbacks and ownership

All Lua and Box2D work remains on the engine owner thread. The bridge snapshots
strong component references, then checks liveness after native calls that can
dispatch callbacks; it does not retain mutable component-container iterators
across those callbacks.

Box2D `SetEnabled(false)`, `SetType` and `DestroyBody` can synchronously destroy
contacts while the world is unlocked. A contact-exit callback that directly
destroys either body can invalidate Box2D's contact traversal. The owner-thread
`PhysicsMutationGuard` scope therefore rejects direct `Rigidbody.OnDestroy` and
`Tilemap.OnDestroy` during that traversal, including destruction of a different
body-owning component. Both components check the same guard before entering
public destruction, then hold it around their actual native body destruction.
Rigidbody activation/type changes and Tilemap chunk activation, chunk cleanup
and prepared-state adoption also hold the scope around their native calls.
Tilemap checks the guard before changing any map data or destroying collision
bodies. Its placement-only `OnStart` and queued `SetCell` remain available. The
error is catchable with Lua `pcall`; this is not a rollback of earlier callback
actions.

`Actor.Destroy` and `actor:RemoveComponent` remain the supported deferred removal
paths. The pre-physics bridge flushes their destruction requests after state
synchronization and before stepping the world. State changes requested by a
contact callback can be applied at the next safe synchronization boundary; this
does not introduce a recursive physics-state flush or a worker-thread API.

## Evidence and remaining validation

`tests/engine/rigidbody_activation_test.cpp` uses the real Engine, Lua setters and Box2D.
Before the fix it failed with an enabled body's position not advancing because
the hierarchy still reported it disabled. The regression covers early and late
activation, one-time Lua/native startup, real contact suspension and recovery,
body identity across repeated toggles, and runtime body-type changes. Contact
callback regressions also exercise direct-destruction rejection and deferred
removal.

A separate real-agent reproducer confirmed that disabling a circle touching a
solid map cell, then calling `map:OnDestroy()` from its contact-exit callback,
crashed the worker with SIGSEGV even inside `pcall`. The reverse operation also
reproduced SIGSEGV: disabling the map and directly destroying the touching
Rigidbody from its exit callback. The shared mutation scope covers both owners;
`tests/engine/tilemap_activation_test.cpp` establishes real circle/map contact in each
of six cases: rejecting direct map destruction when disabling the Rigidbody;
rejecting direct Rigidbody destruction when disabling the map; safely deferring
map actor destruction; safely deferring Rigidbody component removal; allowing
the map's placement-only `OnStart`; and retaining a queued `SetCell` until the
next normal commit. The rejection cases check unchanged userdata identity,
body count and map data, with a raycast also verifying surviving map geometry.
Every case advances another frame to check continued runtime usability.

The changed Lifecycle, Rigidbody API and Rigidbody implementation passed private
syntax checks. After the no-world fast path, the coordinator's final 72-test
matrices passed in Debug (77.83 s), Release (51.35 s) and ASan (162.19 s); see
[status.md](status.md) for the integrated
checkpoint. This change does not certify arbitrary native mutation from contact
callbacks or provide transactional tick rollback.

## Bounded frame-cost check

After builds, tests and renderer checks were idle, three interleaved Release
before/after pairs compared the retained Phase 14 executable with the
activation/shared-guard build before the no-world fast path. This keeps LuaBridge3 fixed; it is not the earlier
LuaBridge 2.7 migration comparison. The unmodified animation benchmark runs full
headless `Engine::StepSimulation`, with eight warmup and 60 measured ticks per
case. All 24 workload rows completed, checked every actor's final timeline and
reported equal native/Lua final states.

Median of the three runs' step p50 values:

| Actors | Workload | Before (ms) | After (ms) | Change |
| ---: | --- | ---: | ---: | ---: |
| 1,000 | Native animation | 0.417792 | 0.417042 | -0.18% |
| 1,000 | Lua loop | 0.618375 | 0.630750 | +2.00% |
| 10,000 | Native animation | 4.262500 | 4.476375 | +5.02% |
| 10,000 | Lua loop | 6.449583 | 6.332417 | -1.82% |

The 10,000-actor native p50 ranges did not overlap: 4.208875–4.337375 ms before,
4.406417–4.541667 ms after. This is a measured regression in this workload; the
other three groups' ranges overlapped. Tail samples included outliers on both
sides, so this short run does not establish reliable tail-latency improvements.
These scenes contain no Rigidbody: the comparison checks unrelated frame cost,
not activation throughput, contact-heavy physics or graphical frame rate. It does
not attribute the timing difference to a particular function. Raw measurements,
binary/source provenance and the reusable driver remain in ignored
`build/phase15-activation-performance/`.

That first comparison led to a narrow no-world fast path: after applying pending
EventBus operations, `StepPhysics` returns before the additional activation scan
if no physics world exists. Rigidbody `OnStart` creates its world at the earlier
startup boundary; Tilemap creates collision bodies at the earlier commit
boundary. The bridge itself does not create bodies. The existing post-callback
world check remains in place. Thus animation-only scenes avoid scanning actors
for absent physics, while first activation still follows the normal startup
order. The intermediate executable and original measurements are retained.

After the final three test matrices and hardware renderer checks completed, a
fresh six-process comparison used the same Phase 14 baseline and identical
workload settings. All 24 rows again completed with every actor verified and
paired final states equal. The final measurement, without discarding runs or
rerunning outliers, was:

| Actors | Workload | Before p50 median (ms) | Final p50 median (ms) | Change |
| ---: | --- | ---: | ---: | ---: |
| 1,000 | Native animation | 0.423375 | 0.453917 | +7.21% |
| 1,000 | Lua loop | 0.626042 | 0.660583 | +5.52% |
| 10,000 | Native animation | 4.943500 | 5.481667 | +10.89% |
| 10,000 | Lua loop | 6.550208 | 6.794292 | +3.73% |

All four groups' three-run p50 ranges overlapped in this final comparison. For
10,000 native actors, baseline ranged from 4.246125 to 5.279667 ms and final from
4.218208 to 11.235083 ms. The final executable's first process took 3.51 s,
compared with 1.59 and 1.39 s for its next two processes. Both baseline and final
had variable tail timings. These are higher observed medians, not evidence that
the fast path eliminated the initial regression; the spread also prevents a
confident stable-cost estimate or causal attribution. No additional optimization
or measurement was undertaken after this final pass. Its separate raw results
and provenance remain in `build/phase15-activation-performance-final/`; the
earlier +5.02% result has not been overwritten.
