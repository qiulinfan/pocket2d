#ifndef RAYCAST_H
#define RAYCAST_H

#include "core/Vector2.h"
#include "physics/PhysicsIds.h"
#include "scene/ActorHandle.h"
#include <cstdint>
#include <optional>
#include <vector>

struct Actor;

namespace RayCast {

struct HitResult {
    Actor *actor = nullptr;
    ActorHandle actor_handle;
    Vector2 point = Vector2(0.0f, 0.0f);
    Vector2 normal = Vector2(0.0f, 0.0f);
    bool is_trigger = false;
};

enum class FixtureKind {
    // normal collider shape
    // 普通碰撞体
    Collider,

    // sensor-only trigger shape
    // 仅触发事件的 trigger
    Trigger,

    // placeholder shape, ignored by raycast hit results
    // 占位 shape, 不会作为 raycast 命中结果返回
    Phantom,
};

struct FixtureMetadata {
    // owning actor and logical shape type. The handle stays meaningful after
    // the actor is destroyed, when Box2D may still report end events.
    // 所属 actor 和逻辑 shape 类型
    Actor *actor = nullptr;
    ActorHandle actor_handle;
    FixtureKind kind = FixtureKind::Collider;
};

// The current World's Box2D world, or b2_nullWorldId when it has none.
// 当前 World 的物理世界
b2WorldId GetPhysicsWorld();
bool HasPhysicsWorld();
void SetPhysicsWorld(b2WorldId world);
// Forget the world and all shape records without destroying the world.
void ClearPhysicsState();
// Number of bodies in the current physics world (0 without a world).
int GetBodyCount();

// register and query shape metadata
// 注册和查询 shape 元数据
void RegisterShape(b2ShapeId shape, Actor *actor, FixtureKind kind);
// Call before destroying a shape. Box2D reports the end of its contacts after
// the next step, so the record stays resolvable until that step's events have
// been dispatched; see ReleaseRetiredShapes.
void UnregisterShape(b2ShapeId shape);
const FixtureMetadata *FindShapeMetadata(b2ShapeId shape);
// Called once per step: records retired before `step_index` are dropped.
void ReleaseRetiredShapes(std::uint64_t step_index);
std::uint64_t GetStepIndex();
void AdvanceStepIndex();

// nearest hit only
// 只返回最近命中
std::optional<HitResult> Cast(const Vector2 &position, const Vector2 &direction,
                              float distance);

// return all hits, sorted by distance
// 返回所有命中结果, 并按距离排序
std::vector<HitResult> CastAll(const Vector2 &position, const Vector2 &direction,
                               float distance);

} // namespace RayCast

#endif
