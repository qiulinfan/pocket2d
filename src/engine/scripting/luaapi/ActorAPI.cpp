#include "RegistrationDetail.h"
#include "scene/ActorHandle.h"
#include "scripting/ComponentManager.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include "LuaBridge/detail/ClassInfo.h"
#include "scripting/LuaBoundary.h"
#include "RawMetamethod.h"

namespace {

using APIRegistrationDetail::LuaState;

void DestroyActorHandle(const ActorHandle *actor) {
    if (actor != nullptr) ComponentManager::DestroyActor(actor->Resolve());
}

ActorHandle ReadHandle(lua_State *state, int index) {
    const auto result = luabridge::Stack<const ActorHandle &>::get(state, index);
    LuaBoundary::Check(result, "Actor equality operand");
    return result.value().get();
}

int EqualActorHandles(lua_State *state) {
    if (!luabridge::Stack<ActorHandle>::isInstance(state, 1) ||
        !luabridge::Stack<ActorHandle>::isInstance(state, 2)) {
        lua_pushboolean(state, false);
        return 1;
    }
    const ActorHandle left = ReadHandle(state, 1);
    const ActorHandle right = ReadHandle(state, 2);
    lua_pushboolean(state, left.uid == right.uid && left.generation == right.generation);
    return 1;
}

void InjectActorAPI() {
    luabridge::getGlobalNamespace(LuaState())
        .beginClass<ActorHandle>("Actor")
        .addFunction("IsValid", &ActorHandle::IsValid)
        .addProperty("is_valid", &ActorHandle::IsValid)
        .addFunction("GetName", &ActorHandle::GetName)
        .addFunction("GetUID", &ActorHandle::GetUID)
        .addFunction("SetParent", &ActorHandle::SetParent)
        .addFunction("AddComponent", &ActorHandle::AddComponent)
        .addFunction("RemoveComponent", &ActorHandle::RemoveComponent)
        .addFunction("GetComponentByKey", &ActorHandle::GetComponentByKey)
        .addFunction("GetComponent", &ActorHandle::GetComponent)
        .addFunction("GetComponents", &ActorHandle::GetComponents)
        .addFunction("GetChildCount", &ActorHandle::GetChildCount)
        .addFunction("GetChildren", &ActorHandle::GetChildren)
        .addFunction("GetComponentInChildren", &ActorHandle::GetComponentInChildren)
        .addFunction("GetComponentsInChildren",
                     &ActorHandle::GetComponentsInChildren)
        .endClass();

    lua_rawgetp(LuaState(), LUA_REGISTRYINDEX,
                luabridge::detail::getClassRegistryKey<ActorHandle>());
    lua_pushstring(LuaState(), "__eq");
    lua_pushcfunction(LuaState(), APIRegistrationDetail::CheckedRawMetamethod<EqualActorHandles>);
    lua_rawset(LuaState(), -3);
    lua_pop(LuaState(), 1);

    luabridge::getGlobalNamespace(LuaState())
        .beginNamespace("Actor")
        .addFunction("Instantiate", &Actor::Instantiate)
        .addFunction("Destroy", &DestroyActorHandle)
        .addFunction("Find", &Actor::Find)
        .addFunction("FindAll", &Actor::FindAll)
        .endNamespace();
}

} // namespace

namespace APIRegistrationDetail {

void RegisterActorAPI() {
    InjectActorAPI();
}

} // namespace APIRegistrationDetail
