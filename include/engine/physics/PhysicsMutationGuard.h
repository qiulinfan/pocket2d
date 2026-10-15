#ifndef PHYSICS_MUTATION_GUARD_H
#define PHYSICS_MUTATION_GUARD_H

// Owner-thread-only scope for native operations that synchronously destroy
// Box2D contacts while the world is unlocked. Public destruction entry points
// reject reentry; internal cleanup may nest scopes. This is not a thread lock.
class PhysicsMutationGuard {
public:
    PhysicsMutationGuard() noexcept { ++depth_; }
    ~PhysicsMutationGuard() { --depth_; }
    PhysicsMutationGuard(const PhysicsMutationGuard &) = delete;
    PhysicsMutationGuard &operator=(const PhysicsMutationGuard &) = delete;
    static bool IsActive() noexcept { return depth_ != 0; }

private:
    inline static unsigned depth_ = 0;
};

#endif
