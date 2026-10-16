#include "physics/RayCast.h"
#include "core/World.h"
#include <algorithm>
#include <unordered_map>

namespace {

struct RaycastHitCandidate {
    RayCast::HitResult hit;
    float fraction = 1.0f;
};

struct RetiredShape {
    RayCast::FixtureMetadata metadata;
    std::uint64_t retired_at_step = 0;
};

// The physics world and its shape metadata belong to the current World.
struct PhysicsState {
    b2WorldId physics_world = b2_nullWorldId;
    std::unordered_map<std::uint64_t, RayCast::FixtureMetadata> shapes;
    std::unordered_map<std::uint64_t, RetiredShape> retired;
    std::uint64_t step_index = 0;
};
PhysicsState &Physics() { return World::Current().State<PhysicsState>(); }

float CollectHit(b2ShapeId shape, b2Vec2 point, b2Vec2 normal, float fraction,
                 void *context) {
    const RayCast::FixtureMetadata *metadata = RayCast::FindShapeMetadata(shape);
    if (metadata == nullptr || metadata->actor == nullptr) return -1.0f;
    if (metadata->kind == RayCast::FixtureKind::Phantom) return -1.0f;

    RaycastHitCandidate candidate;
    candidate.hit.actor = metadata->actor;
    candidate.hit.actor_handle = metadata->actor_handle;
    candidate.hit.point = Vector2(point.x, point.y);
    candidate.hit.normal = Vector2(normal.x, normal.y);
    candidate.hit.is_trigger = metadata->kind == RayCast::FixtureKind::Trigger;
    candidate.fraction = fraction;
    static_cast<std::vector<RaycastHitCandidate> *>(context)->push_back(candidate);
    return 1.0f;
}

std::vector<RaycastHitCandidate> CollectHits(const Vector2 &position,
                                             const Vector2 &direction,
                                             float distance) {
    std::vector<RaycastHitCandidate> hits;
    if (!RayCast::HasPhysicsWorld() || distance <= 0.0f) return hits;

    Vector2 normalized_direction = direction;
    if (normalized_direction.Normalize() == 0.0f) return hits;

    const Vector2 translation = distance * normalized_direction;
    b2World_CastRay(Physics().physics_world, {position.x, position.y},
                    {translation.x, translation.y}, b2DefaultQueryFilter(),
                    CollectHit, &hits);

    std::stable_sort(hits.begin(), hits.end(),
                     [](const RaycastHitCandidate &a,
                        const RaycastHitCandidate &b) {
                         return a.fraction < b.fraction;
                     });
    return hits;
}

} // namespace

namespace RayCast {

b2WorldId GetPhysicsWorld() {
    return Physics().physics_world;
}

bool HasPhysicsWorld() {
    return B2_IS_NON_NULL(Physics().physics_world);
}

void SetPhysicsWorld(b2WorldId world) {
    Physics().physics_world = world;
}

void ClearPhysicsState() {
    PhysicsState &physics = Physics();
    physics.shapes.clear();
    physics.retired.clear();
    physics.physics_world = b2_nullWorldId;
}

int GetBodyCount() {
    if (!HasPhysicsWorld()) return 0;
    return b2World_GetCounters(Physics().physics_world).bodyCount;
}

void RegisterShape(b2ShapeId shape, Actor *actor, FixtureKind kind) {
    if (B2_IS_NULL(shape)) return;
    const std::uint64_t key = b2StoreShapeId(shape);
    Physics().retired.erase(key);
    Physics().shapes[key] = {actor, ActorHandle(actor), kind};
}

void UnregisterShape(b2ShapeId shape) {
    if (B2_IS_NULL(shape)) return;
    PhysicsState &physics = Physics();
    const std::uint64_t key = b2StoreShapeId(shape);
    const auto found = physics.shapes.find(key);
    if (found == physics.shapes.end()) return;
    // The actor may be gone by the time end events arrive; keep only the handle.
    RetiredShape retired{found->second, physics.step_index};
    retired.metadata.actor = nullptr;
    physics.retired[key] = retired;
    physics.shapes.erase(found);
}

const FixtureMetadata *FindShapeMetadata(b2ShapeId shape) {
    PhysicsState &physics = Physics();
    const std::uint64_t key = b2StoreShapeId(shape);
    const auto live = physics.shapes.find(key);
    if (live != physics.shapes.end()) return &live->second;
    const auto retired = physics.retired.find(key);
    if (retired != physics.retired.end()) return &retired->second.metadata;
    return nullptr;
}

void ReleaseRetiredShapes(std::uint64_t step_index) {
    std::erase_if(Physics().retired, [step_index](const auto &entry) {
        return entry.second.retired_at_step < step_index;
    });
}

std::uint64_t GetStepIndex() {
    return Physics().step_index;
}

void AdvanceStepIndex() {
    ++Physics().step_index;
}

// nearest hit only
std::optional<HitResult> Cast(const Vector2 &position, const Vector2 &direction,
                              float distance) {
    const std::vector<RaycastHitCandidate> hits = CollectHits(position, direction, distance);
    if (hits.empty()) return std::nullopt;
    return hits.front().hit;
}

// return all hits, sorted by distance
std::vector<HitResult> CastAll(const Vector2 &position, const Vector2 &direction,
                               float distance) {
    const std::vector<RaycastHitCandidate> hit_candidates = CollectHits(position, direction, distance);

    std::vector<HitResult> hits;
    hits.reserve(hit_candidates.size());
    for (const RaycastHitCandidate &candidate : hit_candidates) {
        hits.push_back(candidate.hit);
    }
    return hits;
}

} // namespace RayCast
