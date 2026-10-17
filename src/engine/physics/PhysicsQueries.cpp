#include "physics/PhysicsQueries.h"
#include "physics/Rigidbody.h"
#include "scene/Actor.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <queue>
#include <set>
#include <unordered_map>
#include <stdexcept>

namespace {

constexpr float kDegreesToRadians = B2_PI / 180.0f;

void Require(bool valid, const char *message) {
    if (!valid) throw std::invalid_argument(std::string("Physics: ") + message);
}
void Finite(const Vector2 &value, const char *name) {
    Require(std::isfinite(value.x) && std::isfinite(value.y) && std::abs(value.x) <= 1e8f &&
                std::abs(value.y) <= 1e8f,
            name);
}
void Positive(float value, const char *name) {
    Require(std::isfinite(value) && value > 0.0f && value <= 1e6f, name);
}
b2WorldId WorldOrThrow() {
    Require(RayCast::HasPhysicsWorld(), "no physics world exists yet");
    return RayCast::GetPhysicsWorld();
}

struct OverlapContext {
    bool include_triggers = false;
    std::set<std::pair<Actor::UID, std::uint64_t>> actors;
};
bool CollectOverlap(b2ShapeId shape, void *context) {
    auto &overlap = *static_cast<OverlapContext *>(context);
    const auto *metadata = RayCast::FindShapeMetadata(shape);
    if (metadata == nullptr || metadata->actor == nullptr) return true;
    if (metadata->kind == RayCast::FixtureKind::Phantom) return true;
    if (metadata->kind == RayCast::FixtureKind::Trigger && !overlap.include_triggers) return true;
    const ActorHandle handle(metadata->actor);
    overlap.actors.insert({handle.uid, handle.generation});
    return true;
}
std::vector<ActorHandle> Handles(const OverlapContext &overlap) {
    std::vector<ActorHandle> result;
    for (const auto &[uid, generation] : overlap.actors) {
        ActorHandle handle;
        handle.uid = uid;
        handle.generation = generation;
        result.push_back(handle);
    }
    return result;
}
std::vector<ActorHandle> Overlap(const b2ShapeProxy &proxy, bool include_triggers) {
    OverlapContext context{include_triggers, {}};
    b2World_OverlapShape(WorldOrThrow(), &proxy, b2DefaultQueryFilter(), CollectOverlap, &context);
    return Handles(context);
}

struct CastContext {
    std::optional<RayCast::HitResult> nearest;
    float fraction = 2.0f;
    const Rigidbody *ignore = nullptr;
    // The mover starts touching what it slides along; like Box2D's mover
    // cast, it skips shapes it already overlaps.
    bool skip_overlaps = false;
};
float CollectCast(b2ShapeId shape, b2Vec2 point, b2Vec2 normal, float fraction, void *context) {
    auto &cast = *static_cast<CastContext *>(context);
    const auto *metadata = RayCast::FindShapeMetadata(shape);
    if (metadata == nullptr || metadata->actor == nullptr) return -1.0f;
    if (metadata->kind != RayCast::FixtureKind::Collider) return -1.0f;
    if (cast.ignore != nullptr && b2Shape_GetBody(shape) == cast.ignore->body) return -1.0f;
    if (cast.skip_overlaps && fraction == 0.0f) return -1.0f;
    if (fraction < cast.fraction) {
        cast.fraction = fraction;
        RayCast::HitResult hit;
        hit.actor = metadata->actor;
        hit.actor_handle = ActorHandle(metadata->actor);
        hit.point = Vector2(point.x, point.y);
        hit.normal = Vector2(normal.x, normal.y);
        cast.nearest = hit;
    }
    return fraction;
}

// The mover shape: the body's collider as a capsule in world space.
// Twice Box2D's linear slop (0.005 m).
float MoverSkin() { return 0.01f * b2GetLengthUnitsPerMeter(); }

b2Capsule MoverCapsule(const Rigidbody &body, const b2Vec2 &position) {
    float radius = body.radius;
    float half = 0.0f;
    if (body.collider_type == "capsule") {
        half = std::max(body.height * 0.5f - body.radius, 0.0f);
    } else if (body.collider_type != "circle") {
        radius = std::min(body.width, body.height) * 0.5f;
        half = std::max(body.height * 0.5f - radius, 0.0f);
    }
    return {{position.x, position.y - half}, {position.x, position.y + half}, radius};
}

// Box mover sweep: ignores the body's own shapes and non-colliders; a shape
// already touching blocks only when it faces against the motion. Tilemap
// outlines are one-sided chains, as for Box2D's contacts and ray casts: only
// motion into their outer side is stopped, so a box touching a tile face
// cannot slip in at a corner, and a box inside a solid tile can walk out.
struct BoxCastContext {
    const Rigidbody *self = nullptr;
    b2Vec2 direction{0, 0};
    float fraction = 1.0f;
    bool hit = false;
};
float CollectBoxCast(b2ShapeId shape, b2Vec2, b2Vec2 normal, float fraction, void *context) {
    auto &cast = *static_cast<BoxCastContext *>(context);
    if (b2Shape_GetBody(shape) == cast.self->body) return -1.0f;
    const auto *metadata = RayCast::FindShapeMetadata(shape);
    if (metadata == nullptr || metadata->kind != RayCast::FixtureKind::Collider) return -1.0f;
    if (b2Shape_GetType(shape) == b2_chainSegmentShape) {
        const b2Segment segment = b2Shape_GetChainSegment(shape).segment;
        const b2Vec2 edge = b2RotateVector(b2Body_GetRotation(b2Shape_GetBody(shape)),
                                           b2Sub(segment.point2, segment.point1));
        // The outer side is right of the segment direction.
        if (edge.y * cast.direction.x - edge.x * cast.direction.y >= 0.0f) return -1.0f;
    } else if (fraction == 0.0f && normal.x * cast.direction.x + normal.y * cast.direction.y > -0.5f) {
        return -1.0f;
    }
    if (fraction < cast.fraction) { cast.fraction = fraction; cast.hit = true; }
    return fraction;
}
Vector2 MoveBox(Rigidbody &body, const Vector2 &delta) {
    const b2WorldId world = RayCast::GetPhysicsWorld();
    const b2Vec2 start = b2Body_GetPosition(body.body);
    b2Vec2 position = start;
    const b2Filter filter = body.GetCollisionFilter();
    const b2QueryFilter query{filter.categoryBits, filter.maskBits};
    const float hx = body.width * 0.5f, hy = body.height * 0.5f;
    const float skin = 0.005f * b2GetLengthUnitsPerMeter();
    for (int axis = 0; axis < 2; ++axis) {
        const float amount = axis == 0 ? delta.x : delta.y;
        if (amount == 0.0f) continue;
        const b2Vec2 corners[4] = {{position.x - hx, position.y - hy}, {position.x + hx, position.y - hy},
                                   {position.x + hx, position.y + hy}, {position.x - hx, position.y + hy}};
        const b2ShapeProxy proxy = b2MakeProxy(corners, 4, 0.0f);
        const b2Vec2 translation = axis == 0 ? b2Vec2{amount, 0} : b2Vec2{0, amount};
        BoxCastContext cast{&body, {axis == 0 ? (amount > 0 ? 1.0f : -1.0f) : 0.0f,
                                    axis == 1 ? (amount > 0 ? 1.0f : -1.0f) : 0.0f}};
        b2World_CastShape(world, &proxy, translation, query, CollectBoxCast, &cast);
        float moved = std::abs(amount);
        if (cast.hit) moved = std::max(0.0f, cast.fraction * std::abs(amount) - skin);
        const float signed_move = amount > 0 ? moved : -moved;
        if (axis == 0) position.x += signed_move; else position.y += signed_move;
    }
    body.SetPosition({position.x, position.y});
    return {position.x - start.x, position.y - start.y};
}

struct MoverContext {
    const Rigidbody *self = nullptr;
    b2Filter filter;
    std::vector<b2CollisionPlane> planes;
};
bool CollectPlane(b2ShapeId shape, const b2PlaneResult *plane, void *context) {
    auto &mover = *static_cast<MoverContext *>(context);
    if (b2Shape_GetBody(shape) == mover.self->body) return true;
    const auto *metadata = RayCast::FindShapeMetadata(shape);
    if (metadata == nullptr || metadata->kind != RayCast::FixtureKind::Collider) return true;
    // Both sides must accept each other, as for contacts.
    const b2Filter other = b2Shape_GetFilter(shape);
    if ((other.categoryBits & mover.filter.maskBits) == 0 || (other.maskBits & mover.filter.categoryBits) == 0) return true;
    if (plane->hit && mover.planes.size() < 16) mover.planes.push_back({plane->plane, FLT_MAX, 0.0f, true});
    return true;
}

} // namespace

namespace PhysicsQueries {

std::vector<ActorHandle> OverlapCircle(const Vector2 &center, float radius, bool include_triggers) {
    Finite(center, "overlap center must be finite");
    Positive(radius, "overlap radius must be positive and finite");
    const b2Vec2 point{center.x, center.y};
    return Overlap(b2MakeProxy(&point, 1, radius), include_triggers);
}

std::vector<ActorHandle> OverlapBox(const Vector2 &center, float width, float height, bool include_triggers) {
    Finite(center, "overlap center must be finite");
    Positive(width, "overlap width must be positive and finite");
    Positive(height, "overlap height must be positive and finite");
    const float hx = width * 0.5f, hy = height * 0.5f;
    const b2Vec2 corners[4] = {{center.x - hx, center.y - hy}, {center.x + hx, center.y - hy},
                               {center.x + hx, center.y + hy}, {center.x - hx, center.y + hy}};
    return Overlap(b2MakeProxy(corners, 4, 0.0f), include_triggers);
}

std::optional<RayCast::HitResult> CircleCast(const Vector2 &origin, float radius,
                                             const Vector2 &direction, float distance) {
    Finite(origin, "cast origin must be finite");
    Positive(radius, "cast radius must be positive and finite");
    Require(std::isfinite(distance) && distance >= 0.0f && distance <= 1e8f, "cast distance must be finite and nonnegative");
    Vector2 unit = direction;
    if (unit.Normalize() == 0.0f || distance == 0.0f || !RayCast::HasPhysicsWorld()) return std::nullopt;
    const b2Vec2 point{origin.x, origin.y};
    const b2ShapeProxy proxy = b2MakeProxy(&point, 1, radius);
    CastContext context;
    b2World_CastShape(RayCast::GetPhysicsWorld(), &proxy, {unit.x * distance, unit.y * distance},
                      b2DefaultQueryFilter(), CollectCast, &context);
    return context.nearest;
}

Vector2 GetGravity() {
    if (!RayCast::HasPhysicsWorld()) return {0.0f, 9.8f};
    const b2Vec2 gravity = b2World_GetGravity(RayCast::GetPhysicsWorld());
    return {gravity.x, gravity.y};
}

void SetGravity(const Vector2 &gravity) {
    Finite(gravity, "gravity must be finite within +/-1e8");
    Rigidbody::EnsurePhysicsWorld();
    b2World_SetGravity(RayCast::GetPhysicsWorld(), {gravity.x, gravity.y});
}

// Follows Box2D 3.1's mover: collide to gather planes, solve, then sweep the
// solved translation, a few times per call.
Vector2 MoveCharacter(Rigidbody &body, const Vector2 &delta) {
    Finite(delta, "character movement must be finite");
    Require(body.HasBody(), "MoveCharacter requires a started Rigidbody");
    Require(b2Body_GetType(body.body) == b2_kinematicBody, "MoveCharacter requires a kinematic Rigidbody");
    if (body.collider_type == "box") return MoveBox(body, delta);
    const b2WorldId world = RayCast::GetPhysicsWorld();
    const b2Vec2 start = b2Body_GetPosition(body.body);
    b2Vec2 position = start;
    const b2Vec2 target{start.x + delta.x, start.y + delta.y};
    MoverContext context;
    context.self = &body;
    context.filter = body.GetCollisionFilter();
    b2QueryFilter query{context.filter.categoryBits, context.filter.maskBits};
    for (int iteration = 0; iteration < 5; ++iteration) {
        context.planes.clear();
        b2Capsule mover = MoverCapsule(body, position);
        // Shape casts stop a linear slop short of contact, where CollideMover
        // reports nothing; a skin keeps the touched surfaces as planes.
        b2Capsule skin = mover;
        skin.radius += MoverSkin();
        b2World_CollideMover(world, &skin, query, CollectPlane, &context);
        const b2PlaneSolverResult solved = b2SolvePlanes({target.x - position.x, target.y - position.y},
                                                         context.planes.data(), static_cast<int>(context.planes.size()));
        // Sweep with a capsule proxy, ignoring the body's own shapes.
        const b2Vec2 points[2] = {mover.center1, mover.center2};
        const b2ShapeProxy proxy = b2MakeProxy(points, 2, mover.radius);
        CastContext cast;
        cast.ignore = &body;
        cast.skip_overlaps = true;
        b2World_CastShape(world, &proxy, solved.translation, query, CollectCast, &cast);
        const float fraction = cast.fraction <= 1.0f ? cast.fraction : 1.0f;
        const b2Vec2 step{fraction * solved.translation.x, fraction * solved.translation.y};
        position.x += step.x;
        position.y += step.y;
        if (step.x * step.x + step.y * step.y < 1e-6f) break;
    }
    body.SetPosition({position.x, position.y});
    return {position.x - start.x, position.y - start.y};
}

// A step into a clearance cell costs this many times a free step, so a path
// from or to the margin crosses it by the shortest way instead of running along
// the obstacle, where a walker wider than a point snags.
constexpr float kClearanceStepCost = 4.0f;

std::vector<Vector2> FindPath(const Vector2 &from, const Vector2 &to, float cell, int max_nodes, float clearance,
                              const std::function<PathCell(float, float)> &occupancy) {
    Finite(from, "path start must be finite");
    Finite(to, "path goal must be finite");
    Positive(cell, "path cell size must be positive and finite");
    Require(max_nodes >= 1 && max_nodes <= 1000000, "path max_nodes must be in [1, 1000000]");
    using Cell = std::pair<int, int>;
    const auto cell_of = [&](const Vector2 &p) {
        return Cell{static_cast<int>(std::floor(p.x / cell)), static_cast<int>(std::floor(p.y / cell))};
    };
    const Cell start = cell_of(from), goal = cell_of(to);
    const auto center = [&](const Cell &c) { return Vector2((c.first + 0.5f) * cell, (c.second + 0.5f) * cell); };
    const auto heuristic = [&](const Cell &c) {
        const float dx = std::abs(static_cast<float>(c.first - goal.first));
        const float dy = std::abs(static_cast<float>(c.second - goal.second));
        return std::max(dx, dy) + 0.41421356f * std::min(dx, dy);
    };
    std::map<Cell, PathCell> occupancy_cache;
    const auto occupied = [&](const Cell &c) {
        if (c == start) return PathCell::Free;
        const auto found = occupancy_cache.find(c);
        if (found != occupancy_cache.end()) return found->second;
        const Vector2 p = center(c);
        return occupancy_cache[c] = occupancy(p.x, p.y);
    };
    // Clearance cells pass only this many cells (Chebyshev) from the start or goal.
    const int margin = static_cast<int>(std::ceil(std::max(clearance, 0.0f) / cell)) + 1;
    const auto near = [&](const Cell &a, const Cell &b) {
        return std::max(std::abs(a.first - b.first), std::abs(a.second - b.second)) <= margin;
    };
    const auto passable = [&](const Cell &c) {
        const PathCell kind = occupied(c);
        return kind == PathCell::Free || (kind == PathCell::Clearance && (near(c, start) || near(c, goal)));
    };
    struct Open { float f; float g; Cell cell; bool operator>(const Open &o) const { return f > o.f; } };
    std::priority_queue<Open, std::vector<Open>, std::greater<Open>> open;
    std::map<Cell, float> cost{{start, 0.0f}};
    std::map<Cell, Cell> parent;
    open.push({heuristic(start), 0.0f, start});
    Cell best = start;
    float best_h = heuristic(start);
    int expanded = 0;
    while (!open.empty() && expanded < max_nodes) {
        const Open current = open.top();
        open.pop();
        if (current.g > cost[current.cell]) continue;
        ++expanded;
        const float h = heuristic(current.cell);
        if (h < best_h) { best_h = h; best = current.cell; }
        if (current.cell == goal) { best = goal; break; }
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                const Cell next{current.cell.first + dx, current.cell.second + dy};
                if (!passable(next)) continue;
                if (dx != 0 && dy != 0 && (!passable({current.cell.first + dx, current.cell.second}) ||
                                           !passable({current.cell.first, current.cell.second + dy}))) continue;
                const float step = dx != 0 && dy != 0 ? 1.41421356f : 1.0f;
                const float g = current.g + (occupied(next) == PathCell::Clearance ? step * kClearanceStepCost : step);
                const auto known = cost.find(next);
                if (known != cost.end() && known->second <= g) continue;
                cost[next] = g;
                parent[next] = current.cell;
                open.push({g + heuristic(next), g, next});
            }
    }
    std::vector<Vector2> path;
    for (Cell c = best; c != start; c = parent.at(c)) path.push_back(center(c));
    std::reverse(path.begin(), path.end());
    if (best == goal && !path.empty()) path.back() = to;
    return path;
}

std::vector<int> PathRegions(const std::vector<Vector2> &points, float cell, int max_nodes, float clearance,
                             const std::function<PathCell(float, float)> &occupancy) {
    Positive(cell, "path cell size must be positive and finite");
    Require(max_nodes >= 1 && max_nodes <= 1000000, "path max_nodes must be in [1, 1000000]");
    Require(points.size() <= 1024, "path regions take at most 1024 points");
    using Cell = std::pair<int, int>;
    std::vector<Cell> seeds;
    for (const Vector2 &p : points) {
        Finite(p, "path region point must be finite");
        seeds.push_back(Cell{static_cast<int>(std::floor(p.x / cell)), static_cast<int>(std::floor(p.y / cell))});
    }
    // Cells within the clearance margin of a point, where clearance cells pass.
    const int margin = static_cast<int>(std::ceil(std::max(clearance, 0.0f) / cell)) + 1;
    std::set<Cell> near;
    for (const Cell &seed : seeds)
        for (int dy = -margin; dy <= margin; ++dy)
            for (int dx = -margin; dx <= margin; ++dx) near.insert({seed.first + dx, seed.second + dy});
    const std::set<Cell> seed_cells(seeds.begin(), seeds.end());
    std::map<Cell, PathCell> occupancy_cache;
    const auto passable = [&](const Cell &c) {
        if (seed_cells.count(c)) return true;
        auto found = occupancy_cache.find(c);
        if (found == occupancy_cache.end())
            found = occupancy_cache.emplace(c, occupancy((c.first + 0.5f) * cell, (c.second + 0.5f) * cell)).first;
        return found->second == PathCell::Free || (found->second == PathCell::Clearance && near.count(c) > 0);
    };
    std::map<Cell, int> label_of;
    std::vector<int> labels(points.size(), 0);
    int next_label = 0;
    for (std::size_t i = 0; i < seeds.size(); ++i) {
        const auto known = label_of.find(seeds[i]);
        if (known != label_of.end()) { labels[i] = known->second; continue; }
        const int label = ++next_label;
        labels[i] = label;
        std::deque<Cell> queue{seeds[i]};
        label_of[seeds[i]] = label;
        for (int expanded = 0; !queue.empty() && expanded < max_nodes; ++expanded) {
            const Cell current = queue.front();
            queue.pop_front();
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    const Cell next{current.first + dx, current.second + dy};
                    if (label_of.count(next) || !passable(next)) continue;
                    if (dx != 0 && dy != 0 && (!passable({current.first + dx, current.second}) ||
                                               !passable({current.first, current.second + dy}))) continue;
                    label_of[next] = label;
                    queue.push_back(next);
                }
        }
    }
    return labels;
}

} // namespace PhysicsQueries

PhysicsJoint PhysicsJoint::Connect(const std::string &kind, Rigidbody &a, Rigidbody &b,
                                   const Vector2 &anchor, const Vector2 &anchor_or_axis) {
    Require(a.HasBody() && b.HasBody(), "joints require two started Rigidbodies");
    Require(!(a.body == b.body), "a joint needs two different bodies");
    Finite(anchor, "joint anchor must be finite");
    Finite(anchor_or_axis, "joint anchor or axis must be finite");
    const b2WorldId world = RayCast::GetPhysicsWorld();
    const b2Vec2 world_anchor{anchor.x, anchor.y};
    const float reference = b2Rot_GetAngle(b2InvMulRot(b2Body_GetRotation(a.body), b2Body_GetRotation(b.body)));
    PhysicsJoint joint;
    joint.world_ = world;
    joint.kind_ = kind;
    if (kind == "distance") {
        b2DistanceJointDef def = b2DefaultDistanceJointDef();
        def.bodyIdA = a.body; def.bodyIdB = b.body;
        const b2Vec2 other{anchor_or_axis.x, anchor_or_axis.y};
        def.localAnchorA = b2Body_GetLocalPoint(a.body, world_anchor);
        def.localAnchorB = b2Body_GetLocalPoint(b.body, other);
        def.length = std::max(b2Distance(world_anchor, other), 0.01f);
        joint.joint_ = b2CreateDistanceJoint(world, &def);
    } else if (kind == "revolute") {
        b2RevoluteJointDef def = b2DefaultRevoluteJointDef();
        def.bodyIdA = a.body; def.bodyIdB = b.body;
        def.localAnchorA = b2Body_GetLocalPoint(a.body, world_anchor);
        def.localAnchorB = b2Body_GetLocalPoint(b.body, world_anchor);
        def.referenceAngle = reference;
        joint.joint_ = b2CreateRevoluteJoint(world, &def);
    } else if (kind == "weld") {
        b2WeldJointDef def = b2DefaultWeldJointDef();
        def.bodyIdA = a.body; def.bodyIdB = b.body;
        def.localAnchorA = b2Body_GetLocalPoint(a.body, world_anchor);
        def.localAnchorB = b2Body_GetLocalPoint(b.body, world_anchor);
        def.referenceAngle = reference;
        joint.joint_ = b2CreateWeldJoint(world, &def);
    } else if (kind == "prismatic" || kind == "wheel") {
        Vector2 axis = anchor_or_axis;
        Require(axis.Normalize() > 0.0f, "prismatic and wheel joints need a nonzero axis");
        const b2Vec2 local_axis = b2Body_GetLocalVector(a.body, {axis.x, axis.y});
        if (kind == "prismatic") {
            b2PrismaticJointDef def = b2DefaultPrismaticJointDef();
            def.bodyIdA = a.body; def.bodyIdB = b.body;
            def.localAnchorA = b2Body_GetLocalPoint(a.body, world_anchor);
            def.localAnchorB = b2Body_GetLocalPoint(b.body, world_anchor);
            def.localAxisA = local_axis;
            def.referenceAngle = reference;
            joint.joint_ = b2CreatePrismaticJoint(world, &def);
        } else {
            b2WheelJointDef def = b2DefaultWheelJointDef();
            def.bodyIdA = a.body; def.bodyIdB = b.body;
            def.localAnchorA = b2Body_GetLocalPoint(a.body, world_anchor);
            def.localAnchorB = b2Body_GetLocalPoint(b.body, world_anchor);
            def.localAxisA = local_axis;
            joint.joint_ = b2CreateWheelJoint(world, &def);
        }
    } else if (kind == "motor") {
        b2MotorJointDef def = b2DefaultMotorJointDef();
        def.bodyIdA = a.body; def.bodyIdB = b.body;
        const b2Vec2 offset = b2Body_GetLocalPoint(a.body, b2Body_GetPosition(b.body));
        def.linearOffset = offset;
        def.angularOffset = reference;
        joint.joint_ = b2CreateMotorJoint(world, &def);
    } else {
        throw std::invalid_argument("Physics: joint kind must be distance/revolute/weld/prismatic/wheel/motor");
    }
    return joint;
}

bool PhysicsJoint::IsValid() const {
    return B2_IS_NON_NULL(joint_) && RayCast::HasPhysicsWorld() &&
           world_ == RayCast::GetPhysicsWorld() && b2Joint_IsValid(joint_);
}

void PhysicsJoint::Destroy() {
    if (IsValid()) b2DestroyJoint(joint_);
    joint_ = b2_nullJointId;
}

void PhysicsJoint::SetMotor(bool enabled, float speed, float max_force) {
    Require(std::isfinite(speed) && std::isfinite(max_force) && max_force >= 0.0f, "motor speed and force must be finite");
    Require(IsValid(), "joint is no longer valid");
    if (kind_ == "distance") {
        b2DistanceJoint_EnableMotor(joint_, enabled); b2DistanceJoint_SetMotorSpeed(joint_, speed);
        b2DistanceJoint_SetMaxMotorForce(joint_, max_force);
    } else if (kind_ == "revolute") {
        b2RevoluteJoint_EnableMotor(joint_, enabled); b2RevoluteJoint_SetMotorSpeed(joint_, speed * kDegreesToRadians);
        b2RevoluteJoint_SetMaxMotorTorque(joint_, max_force);
    } else if (kind_ == "prismatic") {
        b2PrismaticJoint_EnableMotor(joint_, enabled); b2PrismaticJoint_SetMotorSpeed(joint_, speed);
        b2PrismaticJoint_SetMaxMotorForce(joint_, max_force);
    } else if (kind_ == "wheel") {
        b2WheelJoint_EnableMotor(joint_, enabled); b2WheelJoint_SetMotorSpeed(joint_, speed * kDegreesToRadians);
        b2WheelJoint_SetMaxMotorTorque(joint_, max_force);
    } else if (kind_ == "motor") {
        b2MotorJoint_SetMaxForce(joint_, enabled ? max_force : 0.0f);
        b2MotorJoint_SetMaxTorque(joint_, enabled ? max_force : 0.0f);
    } else {
        throw std::invalid_argument("Physics: weld joints have no motor");
    }
}

void PhysicsJoint::SetLimits(bool enabled, float lower, float upper) {
    Require(std::isfinite(lower) && std::isfinite(upper) && lower <= upper, "joint limits must be finite with lower <= upper");
    Require(IsValid(), "joint is no longer valid");
    if (kind_ == "distance") {
        Require(lower >= 0.0f, "distance limits must be nonnegative");
        b2DistanceJoint_EnableLimit(joint_, enabled); b2DistanceJoint_SetLengthRange(joint_, lower, upper);
    } else if (kind_ == "revolute") {
        b2RevoluteJoint_EnableLimit(joint_, enabled);
        b2RevoluteJoint_SetLimits(joint_, lower * kDegreesToRadians, upper * kDegreesToRadians);
    } else if (kind_ == "prismatic") {
        b2PrismaticJoint_EnableLimit(joint_, enabled); b2PrismaticJoint_SetLimits(joint_, lower, upper);
    } else if (kind_ == "wheel") {
        b2WheelJoint_EnableLimit(joint_, enabled); b2WheelJoint_SetLimits(joint_, lower, upper);
    } else {
        throw std::invalid_argument("Physics: weld and motor joints have no limits");
    }
}

void PhysicsJoint::SetSpring(bool enabled, float hertz, float damping_ratio) {
    Require(std::isfinite(hertz) && std::isfinite(damping_ratio) && hertz >= 0.0f && damping_ratio >= 0.0f,
            "spring hertz and damping must be finite and nonnegative");
    Require(IsValid(), "joint is no longer valid");
    if (kind_ == "distance") {
        b2DistanceJoint_EnableSpring(joint_, enabled); b2DistanceJoint_SetSpringHertz(joint_, hertz);
        b2DistanceJoint_SetSpringDampingRatio(joint_, damping_ratio);
    } else if (kind_ == "revolute") {
        b2RevoluteJoint_EnableSpring(joint_, enabled); b2RevoluteJoint_SetSpringHertz(joint_, hertz);
        b2RevoluteJoint_SetSpringDampingRatio(joint_, damping_ratio);
    } else if (kind_ == "prismatic") {
        b2PrismaticJoint_EnableSpring(joint_, enabled); b2PrismaticJoint_SetSpringHertz(joint_, hertz);
        b2PrismaticJoint_SetSpringDampingRatio(joint_, damping_ratio);
    } else if (kind_ == "wheel") {
        b2WheelJoint_EnableSpring(joint_, enabled); b2WheelJoint_SetSpringHertz(joint_, hertz);
        b2WheelJoint_SetSpringDampingRatio(joint_, damping_ratio);
    } else if (kind_ == "weld") {
        const float value = enabled ? hertz : 0.0f;
        b2WeldJoint_SetLinearHertz(joint_, value); b2WeldJoint_SetAngularHertz(joint_, value);
        b2WeldJoint_SetLinearDampingRatio(joint_, damping_ratio); b2WeldJoint_SetAngularDampingRatio(joint_, damping_ratio);
    } else {
        throw std::invalid_argument("Physics: motor joints have no spring");
    }
}
