# ADR 005: prepare complete runtime actors before publication

## Status and context

Accepted under the owner's autonomous Pocket2D development direction, 2026-10-15.
Phase 9's real-runtime audit found two observable failures: a rejected template
could later invoke a never-started component's OnDestroy and damage another actor;
a failed host duplication could leave a live partial actor that starts next tick.
Per-component construction validation cannot make a multi-component actor atomic.

## Decision

Use one move-only PreparedRuntimeActor for both Lua Actor.Instantiate and native
runtime duplication. It owns a preallocated actor node, every detached component
record and a nested main-VM GC pause. Preparation validates keys/types/defaults,
root pose and relevant peer/resource constraints. Tilemap and SpriteAnimation
initialize their data inside detached native objects, without publishing identity,
creating physical bodies or replacing the scene's prepared-resource caches.

Only a successful plan receives a runtime generation, joins the actor list and
indexes, binds component identities and queues lifecycle work. Animation poses
apply after all peers are registered, using the prepared data with no new file IO.
Rejected unpublished records are released as data; gameplay OnDestroy never runs
for them. Legitimately published actors destroyed before OnStart retain their
existing OnDestroy behavior.

Preserve distinct activation behavior: Instantiate is immediately findable but
uses deferred activation; native host duplication immediately registers traversal
and lifecycle entries. Monotonic actor-order reservations and conditional ordered
insertion keep mixed modes consistent across update, late-update and animation
traversal. Publication uses hash-based UID checks and does not rebind the whole
world. Host duplicate-name selection retains its existing name scan. UID/key reservation
gaps are allowed; identities remain opaque and are not rolled back for cosmetics.

A template cache miss is published only after full actor preparation succeeds.
Successfully cached templates retain their prior lifetime policy. Fixing a newly
rejected template can therefore succeed on retry without invalidating unrelated
cached templates or requiring a VM reset.

## Failure and alternatives

Normal preparation rejection preserves the old world, native/Lua identities,
queues and callback counts. Duplication returns false with a bounded diagnostic;
Lua Instantiate raises a normal catchable error. The editor retains its selection
and can display the duplicate diagnostic.

Do not implement failure cleanup by destroying a partially registered actor:
that intentionally runs gameplay teardown and is the reproduced defect. Do not
suppress all OnDestroy calls before OnStart: published actors may already own
resources or state initialized by their caller. A full-world snapshot/rollback
would be broader than necessary and would not undo arbitrary external IO.

CommitStarted explicitly separates publication from preparation. Unexpected
allocation/install failures after that point require a fresh runtime; they do not
claim rollback. Engine stops subsequent frame phases, and the agent reports
actor_publish_failed with requires_reset. Main-VM allocator exhaustion retains its
existing fatal boundary. Tokens must be destroyed before their VM shuts down.

## Verification and remaining scope

Use runtime_actor_preparation, runtime_actor_assets, runtime_actor_spawn and the
real agent_actor_spawn regression, plus existing UID/lifetime/hierarchy/reload and
gameplay tests. Cover late component failure, old-world health/callback preservation,
source repair/retry, cached native identities, valid delayed starts, mixed activation
ordering, resource deletion after preparation and outer-cache isolation. Final
results belong in ../checkpoints/phase10-checkpoint.md and status.md.

Editor scene-edit mutation batches and all lazy sprite/audio/font resources are
separate boundaries. This decision does not certify those paths, all native memory
failures or complete gameplay migration. No asset/save format changes or dependency
updates are required. Reverting only one call site would reintroduce the known
partial-publication bug; keep both call sites and their regression contract together.
