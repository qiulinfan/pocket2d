#include "RegistrationDetail.h"
#include "physics/Collision.h"
#include "physics/RayCast.h"
#include "scene/Actor.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include "LuaBridge/detail/ClassInfo.h"
#include "scripting/LuaBoundary.h"
#include "RawMetamethod.h"
#include <optional>
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
        .beginNamespace("Physics")
        .addFunction("Raycast", &CppPhysicsRaycast)
        .addFunction("RaycastAll", &CppPhysicsRaycastAll)
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
