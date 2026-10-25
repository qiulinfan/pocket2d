# ADR 002: Stable actor lifetimes and bounded CPU parallelism

Date: 2026-10-15. Status: implemented; regression and supported sanitizer checks pass.

## Actor identity

The defense regression demonstrated that compacting a deque after one actor's
removal moved surviving actors, invalidating Box2D fixture metadata and cached
Lua actor pointers. Rebinding only component fields would leave other references
unsafe. Keeping every destroyed actor forever would hide the problem with
unbounded memory growth.

Store live native actors in a list so removing one actor preserves survivor
addresses. Lua receives `ActorHandle {uid, generation}` values resolved through
the current runtime registry. Generations are process-monotonic and are not
serialized scene IDs. Destroyed/pending-destroy actors and a later actor reusing
an old UID cannot be mistaken for one another. Lookup, hierarchy, collision,
raycast and builtin component owner fields share this identity contract.

A list trades allocation/locality cost for stable survivor addresses. A measured
actor-iteration bottleneck may justify a stable pool later; this change does not
claim that a linked list is the highest-throughput actor representation.

Native physics stays on the owning thread and can use stable pointers during a
live actor's lifetime. This decision does not establish arbitrary component or
Lua-table save compatibility. Regressions cover churn, surviving cached refs,
destroyed handles, scene UID reuse, collisions and runtime reset.

## CPU tasks

Use a fixed, bounded pool for synchronous disjoint-range work, with the caller
participating and a join barrier before state is observed. Recursive submission
on the executing worker runs inline; errors propagate after all current workers
have finished. The first consumer is particle SoA arithmetic. Emission, array
resizing, destruction/free-list updates, Lua, SDL and physics stay on the owner
thread. Order-sensitive merging is serial.

Serial and parallel paths use equal chunk tiling. Work below the measured
threshold avoids thread dispatch. This is deliberately smaller than a global
entity scheduler; other systems become parallel only with explicit ownership
and measured benefit. Tests check coverage, nested calls, concurrent submitters,
error recovery, shutdown and exact serial/parallel particle equivalence.
