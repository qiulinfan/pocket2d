#ifndef ENGINE_SCENE_ACTOR_HANDLE_H
#define ENGINE_SCENE_ACTOR_HANDLE_H

#include "scene/Actor.h"

// Lua owns this small value, never a pointer into the runtime actor container.
// A generation distinguishes a new scene actor from an earlier actor with the
// same authored UID. Generations are internal and never serialized.
struct ActorHandle {
    Actor::UID uid = Actor::kInvalidUID;
    std::uint64_t generation = 0;

    ActorHandle() = default;
    explicit ActorHandle(const Actor *actor);
    Actor *Resolve() const;
    bool IsValid() const;
    Actor::UID GetUID() const;
    std::string GetName() const;
    bool SetParent(const ActorHandle *parent) const;
    luabridge::LuaRef AddComponent(const std::string &type_name) const;
    void RemoveComponent(luabridge::LuaRef component) const;
    luabridge::LuaRef GetComponentByKey(const std::string &key) const;
    luabridge::LuaRef GetComponent(const std::string &type_name) const;
    luabridge::LuaRef GetComponents(const std::string &type_name) const;
    int GetChildCount() const;
    luabridge::LuaRef GetChildren() const;
    luabridge::LuaRef GetComponentInChildren(const std::string &type_name) const;
    luabridge::LuaRef GetComponentsInChildren(const std::string &type_name) const;
};

#endif
