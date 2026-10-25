#ifndef PHYSICS_MUTATION_GUARD_H
#define PHYSICS_MUTATION_GUARD_H

#include "core/World.h"

// Scope in which physics contact and trigger callbacks are being delivered.
// Public destruction entry points (Rigidbody/Tilemap OnDestroy, scene edits)
// reject direct native destruction inside it; deferred removal through
// RemoveComponent or Actor.Destroy stays available. This is not a thread lock.
// The depth is per World.
class PhysicsMutationGuard {
public:
    PhysicsMutationGuard() noexcept { ++Depth().value; }
    ~PhysicsMutationGuard() { --Depth().value; }
    PhysicsMutationGuard(const PhysicsMutationGuard &) = delete;
    PhysicsMutationGuard &operator=(const PhysicsMutationGuard &) = delete;
    static bool IsActive() noexcept { return Depth().value != 0; }

private:
    struct DepthState { unsigned value = 0; };
    static DepthState &Depth() noexcept { return World::Current().State<DepthState>(); }
};

#endif
