#include "RegistrationDetail.h"
#include "physics/Collision.h"
#include "physics/PhysicsQueries.h"
#include "physics/RayCast.h"
#include "physics/Rigidbody.h"
#include "scripting/ComponentManager.h"
#include "scene/Actor.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include "LuaBridge/detail/ClassInfo.h"
#include "scripting/LuaBoundary.h"
#include "RawMetamethod.h"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace {

using APIRegistrationDetail::LuaState;

float CppVector2Distance(const Vector2 &a, const Vector2 &b) {
    return Distance(a, b);
}

template <typename T>
void InstallClassMetamethod(const char *name, lua_CFunction function) {
    lua_rawgetp(LuaState(), LUA_REGISTRYINDEX,
                luabridge::detail::getClassRegistryKey<T>());
    if (lua_istable(LuaState(), -1)) {
        lua_pushstring(LuaState(), name);
        lua_pushcfunction(LuaState(), function);
        lua_rawset(LuaState(), -3);
    }
    lua_pop(LuaState(), 1);
}

Vector2 ReadVector(lua_State *state, int index) {
    const auto result = luabridge::Stack<const Vector2 &>::get(state, index);
    LuaBoundary::Check(result, "Vector2 operand");
    return result.value().get();
}
float ReadScalar(lua_State *state, int index) {
    const auto result = luabridge::Stack<float>::get(state, index);
    LuaBoundary::Check(result, "Vector2 scalar operand");
    return result.value();
}
void PushVector(lua_State *state, const Vector2 &value) {
    LuaBoundary::Check(luabridge::Stack<Vector2>::push(state, value), "Vector2 result");
}

int LuaVector2Add(lua_State *L) {
    const Vector2 a = ReadVector(L, 1);
    const Vector2 b = ReadVector(L, 2);
    PushVector(L, a + b);
    return 1;
}

int LuaVector2Sub(lua_State *L) {
    const Vector2 a = ReadVector(L, 1);
    const Vector2 b = ReadVector(L, 2);
    PushVector(L, a - b);
    return 1;
}

int LuaVector2Mul(lua_State *L) {
    const bool lhs_is_vector = luabridge::Stack<Vector2>::isInstance(L, 1);
    const bool rhs_is_vector = luabridge::Stack<Vector2>::isInstance(L, 2);
    const bool lhs_is_number = lua_isnumber(L, 1) != 0;
    const bool rhs_is_number = lua_isnumber(L, 2) != 0;

    if (lhs_is_vector && rhs_is_number) {
        const Vector2 vec = ReadVector(L, 1);
        const float scalar = ReadScalar(L, 2);
        PushVector(L, scalar * vec);
        return 1;
    }

    if (lhs_is_number && rhs_is_vector) {
        const float scalar = ReadScalar(L, 1);
        const Vector2 vec = ReadVector(L, 2);
        PushVector(L, scalar * vec);
        return 1;
    }

    throw LuaBoundary::Error("Vector2 multiplication expects (Vector2, number) or (number, Vector2)");
}

void InjectVector2API() {
    luabridge::getGlobalNamespace(LuaState())
        .beginClass<Vector2>("Vector2")
        .addConstructor<void (*)(float, float)>()
        .addPropertyReadWrite("x", &Vector2::x)
        .addPropertyReadWrite("y", &Vector2::y)
        .addFunction("Normalize", &Vector2::Normalize)
        .addFunction("Length", &Vector2::Length)
        .addStaticFunction("Distance",
                           static_cast<float (*)(const Vector2 &, const Vector2 &)>( &CppVector2Distance))
        .addStaticFunction("Dot",
                           static_cast<float (*)(const Vector2 &, const Vector2 &)>( &Dot))
        .endClass();

    InstallClassMetamethod<Vector2>("__add", &APIRegistrationDetail::CheckedRawMetamethod<LuaVector2Add>);
    InstallClassMetamethod<Vector2>("__sub", &APIRegistrationDetail::CheckedRawMetamethod<LuaVector2Sub>);
    InstallClassMetamethod<Vector2>("__mul", &APIRegistrationDetail::CheckedRawMetamethod<LuaVector2Mul>);
}

void InjectCollisionAPI() {
    luabridge::getGlobalNamespace(LuaState())
        .beginClass<Collision>("Collision")
        .addConstructor<void (*)()>()
        .addPropertyReadWrite("other", &Collision::other_handle)
        .addPropertyReadWrite("point", &Collision::point)
        .addPropertyReadWrite("relative_velocity", &Collision::relative_velocity)
        .addPropertyReadWrite("normal", &Collision::normal)
        .endClass();
}

luabridge::LuaRef CppPhysicsRaycast(const Vector2 &position,
                                    const Vector2 &direction, float distance) {
                                        std::optional<RayCast::HitResult> hit = RayCast::Cast(position, direction, distance);
    if (!hit.has_value()) return luabridge::LuaRef(LuaState());
    return luabridge::LuaRef(LuaState(), *hit);
}

luabridge::LuaRef CppPhysicsRaycastAll(const Vector2 &position,
                                       const Vector2 &direction,
                                       float distance) {
    luabridge::LuaRef results = luabridge::newTable(LuaState());
    const std::vector<RayCast::HitResult> hits = RayCast::CastAll(position, direction, distance);
    for (size_t i = 0; i < hits.size(); i++) {
        results[static_cast<int>(i + 1)] = hits[i];
    }
    return results;
}

luabridge::LuaRef ActorArray(const std::vector<ActorHandle> &actors) {
    luabridge::LuaRef results = luabridge::newTable(LuaState());
    for (std::size_t i = 0; i < actors.size(); ++i) results[static_cast<int>(i + 1)] = actors[i];
    return results;
}
luabridge::LuaRef CppOverlapCircle(const Vector2 &center, float radius, bool include_triggers) {
    return ActorArray(PhysicsQueries::OverlapCircle(center, radius, include_triggers));
}
luabridge::LuaRef CppOverlapBox(const Vector2 &center, float width, float height, bool include_triggers) {
    return ActorArray(PhysicsQueries::OverlapBox(center, width, height, include_triggers));
}
luabridge::LuaRef CppCircleCast(const Vector2 &origin, float radius, const Vector2 &direction, float distance) {
    const auto hit = PhysicsQueries::CircleCast(origin, radius, direction, distance);
    if (!hit) return luabridge::LuaRef(LuaState());
    return luabridge::LuaRef(LuaState(), *hit);
}
Rigidbody &Body(Rigidbody *body) {
    if (body == nullptr) throw std::invalid_argument("Physics: a Rigidbody is required");
    return *body;
}
PhysicsJoint ConnectAt(const char *kind, Rigidbody *a, Rigidbody *b, const Vector2 &anchor) {
    return PhysicsJoint::Connect(kind, Body(a), Body(b), anchor, anchor);
}
PhysicsJoint ConnectDistance(Rigidbody *a, Rigidbody *b, const Vector2 &anchor_a, const Vector2 &anchor_b) {
    return PhysicsJoint::Connect("distance", Body(a), Body(b), anchor_a, anchor_b);
}
PhysicsJoint ConnectRevolute(Rigidbody *a, Rigidbody *b, const Vector2 &anchor) { return ConnectAt("revolute", a, b, anchor); }
PhysicsJoint ConnectWeld(Rigidbody *a, Rigidbody *b, const Vector2 &anchor) { return ConnectAt("weld", a, b, anchor); }
PhysicsJoint ConnectPrismatic(Rigidbody *a, Rigidbody *b, const Vector2 &anchor, const Vector2 &axis) {
    return PhysicsJoint::Connect("prismatic", Body(a), Body(b), anchor, axis);
}
PhysicsJoint ConnectWheel(Rigidbody *a, Rigidbody *b, const Vector2 &anchor, const Vector2 &axis) {
    return PhysicsJoint::Connect("wheel", Body(a), Body(b), anchor, axis);
}
PhysicsJoint ConnectMotor(Rigidbody *a, Rigidbody *b) {
    return PhysicsJoint::Connect("motor", Body(a), Body(b), Vector2(), Vector2());
}
// The mover is kinematic, and kinematic bodies follow their Transform each
// physics step, so the moved position is written back to the Transform too.
Vector2 MoveCharacter(Rigidbody *body, const Vector2 &delta) {
    Rigidbody &mover = Body(body);
    const Vector2 moved = PhysicsQueries::MoveCharacter(mover, delta);
    if (mover.actor_handle.IsValid()) {
        const Vector2 position = mover.GetPosition();
        ComponentManager::SetRuntimeTransformWorldPosition(mover.actor_handle.uid, position.x, position.y);
    }
    return moved;
}

// Static colliders and solid tilemap cells block; movers and triggers do not.
// `radius` is the walker's clearance: a cell centre within it of a static
// collider is a clearance cell, one inside the collider is solid.
std::function<PhysicsQueries::PathCell(float, float)> PathOccupancy(const char *api, float radius) {
    if (!std::isfinite(radius) || radius < 0.0f || radius > 1e6f)
        throw std::invalid_argument(std::string(api) + " radius must be finite and nonnegative");
    const bool has_world = RayCast::HasPhysicsWorld();
    const auto touches_static = [](float x, float y, float r) {
        struct Hit { bool any = false; } hit;
        const b2Vec2 point{x, y};
        const b2ShapeProxy proxy = b2MakeProxy(&point, 1, r);
        b2World_OverlapShape(RayCast::GetPhysicsWorld(), &proxy, b2DefaultQueryFilter(),
            [](b2ShapeId shape, void *context) {
                const auto *metadata = RayCast::FindShapeMetadata(shape);
                if (metadata == nullptr || metadata->kind != RayCast::FixtureKind::Collider) return true;
                if (b2Body_GetType(b2Shape_GetBody(shape)) != b2_staticBody) return true;
                static_cast<Hit *>(context)->any = true;
                return false;
            }, &hit);
        return hit.any;
    };
    return [has_world, touches_static, radius](float x, float y) {
        using PhysicsQueries::PathCell;
        if (ComponentManager::IsSolidTileAt(x, y)) return PathCell::Solid;
        if (!has_world || !touches_static(x, y, std::max(radius, 0.001f))) return PathCell::Free;
        return radius > 0.001f && !touches_static(x, y, 0.001f) ? PathCell::Clearance : PathCell::Solid;
    };
}

luabridge::LuaRef FindPath(const Vector2 &from, const Vector2 &to, float cell, float radius, int max_nodes) {
    const auto occupancy = PathOccupancy("Physics.FindPath", radius);
    const auto path = PhysicsQueries::FindPath(from, to, cell, max_nodes, radius, occupancy);
    lua_State *state = LuaState();
    luabridge::LuaRef result = luabridge::newTable(state);
    for (std::size_t i = 0; i < path.size(); ++i) result[static_cast<int>(i + 1)] = path[i];
    return result;
}

luabridge::LuaRef PathRegions(const luabridge::LuaRef &points, float cell, float radius, int max_nodes) {
    if (!points.isTable()) throw std::invalid_argument("Physics.PathRegions points must be an array of Vector2");
    std::vector<Vector2> values;
    for (int i = 1;; ++i) {
        const luabridge::LuaRef point = points[i];
        if (point.isNil()) break;
        if (!point.isInstance<Vector2>()) throw std::invalid_argument("Physics.PathRegions points must be Vector2 values");
        values.push_back(point.cast<Vector2>().value());
    }
    const auto labels = PhysicsQueries::PathRegions(values, cell, max_nodes, radius,
                                                    PathOccupancy("Physics.PathRegions", radius));
    luabridge::LuaRef result = luabridge::newTable(LuaState());
    for (std::size_t i = 0; i < labels.size(); ++i) result[static_cast<int>(i + 1)] = labels[i];
    return result;
}

void InjectPhysicsAPI() {
    luabridge::getGlobalNamespace(LuaState())
        .beginClass<RayCast::HitResult>("HitResult")
        .addConstructor<void (*)()>()
        .addPropertyReadWrite("actor", &RayCast::HitResult::actor_handle)
        .addPropertyReadWrite("point", &RayCast::HitResult::point)
        .addPropertyReadWrite("normal", &RayCast::HitResult::normal)
        .addPropertyReadWrite("is_trigger", &RayCast::HitResult::is_trigger)
        .endClass();

    luabridge::getGlobalNamespace(LuaState())
        .beginClass<PhysicsJoint>("Joint")
        .addFunction("IsValid", &PhysicsJoint::IsValid)
        .addFunction("Destroy", &PhysicsJoint::Destroy)
        .addFunction("GetKind", &PhysicsJoint::GetKind)
        .addFunction("SetMotor", &PhysicsJoint::SetMotor)
        .addFunction("SetLimits", &PhysicsJoint::SetLimits)
        .addFunction("SetSpring", &PhysicsJoint::SetSpring)
        .endClass();

    luabridge::getGlobalNamespace(LuaState())
        .beginNamespace("Physics")
        .addFunction("Raycast", &CppPhysicsRaycast)
        .addFunction("RaycastAll", &CppPhysicsRaycastAll)
        .addFunction("OverlapCircle", &CppOverlapCircle)
        .addFunction("OverlapBox", &CppOverlapBox)
        .addFunction("CircleCast", &CppCircleCast)
        .addFunction("GetGravity", &PhysicsQueries::GetGravity)
        .addFunction("SetGravity", &PhysicsQueries::SetGravity)
        .addFunction("MoveCharacter", &MoveCharacter)
        .addFunction("FindPath", &FindPath)
        .addFunction("PathRegions", &PathRegions)
        .addFunction("ConnectDistance", &ConnectDistance)
        .addFunction("ConnectRevolute", &ConnectRevolute)
        .addFunction("ConnectWeld", &ConnectWeld)
        .addFunction("ConnectPrismatic", &ConnectPrismatic)
        .addFunction("ConnectWheel", &ConnectWheel)
        .addFunction("ConnectMotor", &ConnectMotor)
        .endNamespace();
}

} // namespace

namespace APIRegistrationDetail {

void RegisterPhysicsAPI() {
    InjectVector2API();
    InjectCollisionAPI();
    InjectPhysicsAPI();
}

} // namespace APIRegistrationDetail
