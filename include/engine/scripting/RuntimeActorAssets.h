#ifndef POCKET2D_RUNTIME_ACTOR_ASSETS_H
#define POCKET2D_RUNTIME_ACTOR_ASSETS_H

#include "scene/Actor.h"
#include <memory>
#include <vector>

namespace ManagerDetail {
struct ComponentRecord;
// Owner-thread preparation of fresh, unbound native records. No gameplay
// callbacks, actor publication, physics creation or global scene-cache writes.
// Caller owns the records and the enclosing main-VM preparation GC pause.
void PrepareDetachedActorAssets(const Actor &actor,
    const std::vector<std::unique_ptr<ComponentRecord>> &records);
void PrepareDetachedTilemapAssets(const Actor &actor,
    const std::vector<std::unique_ptr<ComponentRecord>> &records);
void PrepareDetachedAnimationAssets(const Actor &actor,
    const std::vector<std::unique_ptr<ComponentRecord>> &records);
// Called after all actor records are published and bound. No file I/O.
void ApplyDetachedActorPoses(Actor::UID uid);
}

#endif
