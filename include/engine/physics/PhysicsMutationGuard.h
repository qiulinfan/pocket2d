#ifndef PHYSICS_MUTATION_GUARD_H
#define PHYSICS_MUTATION_GUARD_H

#include "core/World.h"

// Owner-thread-only scope for native operations that synchronously destroy
// Box2D contacts while the world is unlocked. Public destruction entry points
// reject reentry; internal cleanup may nest scopes. This is not a thread lock.
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
