#include <stdexcept>
#include "scene/Actor.h"
#include "scene/ActorHandle.h"
#include "scene/Scene.h"
#include "shared/scene_format/SceneFormat.h"
#include "scripting/ComponentManager.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <cstdlib>
#include <iostream>
#include <unordered_map>

namespace {

std::unordered_map<std::string, Actor> g_template_cache;
std::uint64_t g_next_actor_generation = 1;

} // namespace

void Actor::AssignRuntimeGeneration() {
    runtime_generation = g_next_actor_generation++;
}

ActorHandle::ActorHandle(const Actor *actor) {
    if (actor != nullptr) {
        uid = actor->uid;
        generation = actor->runtime_generation;
    }
}

Actor *ActorHandle::Resolve() const {
    return ComponentManager::ResolveActorHandle(uid, generation);
}

bool ActorHandle::IsValid() const { return Resolve() != nullptr; }
Actor::UID ActorHandle::GetUID() const { return uid; }
std::string ActorHandle::GetName() const {
    const Actor *actor = Resolve();
    return actor == nullptr ? "" : actor->actor_name;
}
bool ActorHandle::SetParent(const ActorHandle *parent) const {
    if (!IsValid() || (parent != nullptr && !parent->IsValid())) return false;
    return ComponentManager::SetActorParent(uid, parent == nullptr ? nullptr : parent->Resolve());
}
luabridge::LuaRef ActorHandle::AddComponent(const std::string &type_name) const {
    return ComponentManager::AddComponent(IsValid() ? uid : Actor::kInvalidUID, type_name);
}
void ActorHandle::RemoveComponent(luabridge::LuaRef component) const {
    if (IsValid()) ComponentManager::RemoveComponent(uid, component);
}
luabridge::LuaRef ActorHandle::GetComponentByKey(const std::string &key) const {
    return ComponentManager::GetComponentByKey(IsValid() ? uid : Actor::kInvalidUID, key);
}
luabridge::LuaRef ActorHandle::GetComponent(const std::string &type_name) const {
    return ComponentManager::GetComponentByType(IsValid() ? uid : Actor::kInvalidUID, type_name);
}
luabridge::LuaRef ActorHandle::GetComponents(const std::string &type_name) const {
    return ComponentManager::GetComponentsByType(IsValid() ? uid : Actor::kInvalidUID, type_name);
}
int ActorHandle::GetChildCount() const {
    return IsValid() ? ComponentManager::GetChildCount(uid) : 0;
}
luabridge::LuaRef ActorHandle::GetChildren() const {
    return ComponentManager::GetChildren(IsValid() ? uid : Actor::kInvalidUID);
}
luabridge::LuaRef ActorHandle::GetComponentInChildren(const std::string &type_name) const {
    return ComponentManager::GetComponentInChildrenByType(IsValid() ? uid : Actor::kInvalidUID, type_name);
}
luabridge::LuaRef ActorHandle::GetComponentsInChildren(const std::string &type_name) const {
    return ComponentManager::GetComponentsInChildrenByType(IsValid() ? uid : Actor::kInvalidUID, type_name);
}

void Actor::ClearTemplateCache() {
    g_template_cache.clear();
}

std::string Actor::GetName() const {
    return actor_name;
}

Actor::UID Actor::GetUID() const {
    return uid;
}

bool Actor::IsSceneBacked() const {
    return scene_backed;
}

bool Actor::IsRuntimeSpawned() const {
    return !scene_backed;
}

bool Actor::SetParent(Actor *parent) const {
    return ComponentManager::SetActorParent(uid, parent);
}

luabridge::LuaRef Actor::AddComponent(const std::string &type_name) const {
    return ComponentManager::AddComponent(uid, type_name);
}

void Actor::RemoveComponent(luabridge::LuaRef component_ref) const {
    ComponentManager::RemoveComponent(uid, component_ref);
}

// get component by key / type. returns nil if not found.
// if multiple components of the same type exist, returns the first one.
luabridge::LuaRef Actor::GetComponentByKey(const std::string &key) const {
    return ComponentManager::GetComponentByKey(uid, key);
}

// get component by key / type. returns nil if not found.
// if multiple components of the same type exist, returns the first one.
luabridge::LuaRef Actor::GetComponent(const std::string &type_name) const {
    return ComponentManager::GetComponentByType(uid, type_name);
}

// returns all component instances of the given type as a Lua array.
luabridge::LuaRef Actor::GetComponents(const std::string &type_name) const {
    return ComponentManager::GetComponentsByType(uid, type_name);
}

int Actor::GetChildCount() const {
    return ComponentManager::GetChildCount(uid);
}

luabridge::LuaRef Actor::GetChildren() const {
    return ComponentManager::GetChildren(uid);
}

luabridge::LuaRef Actor::GetComponentInChildren(
    const std::string &type_name) const {
    return ComponentManager::GetComponentInChildrenByType(uid, type_name);
}

luabridge::LuaRef Actor::GetComponentsInChildren(
    const std::string &type_name) const {
    return ComponentManager::GetComponentsInChildrenByType(uid, type_name);
}

// loads an actor data copy from an actor template file.
ActorTemplateCandidate Actor::ReadTemplateForSpawn(const std::string &template_name) {
    const std::string path = SceneFormat::ResolveTemplatePath(template_name, Scene::GetActiveSceneSubdirectory());
    if (path.empty()) throw std::runtime_error("template " + template_name + " is missing");
    const auto cached = g_template_cache.find(path);
    if (cached != g_template_cache.end()) return {cached->second, {}};
    Actor actor = SceneFormat::LoadActorTemplateAsset(template_name, Scene::GetActiveSceneSubdirectory()).actor;
    SceneFormat::EnsureBuiltinTransformComponent(actor.component_specs);
    return {std::move(actor), path};
}

void Actor::CommitSpawnTemplate(const ActorTemplateCandidate &candidate) {
    if (!candidate.uncached_path.empty()) g_template_cache.emplace(candidate.uncached_path, candidate.actor);
}

Actor Actor::LoadTemplate(const std::string &template_name) {
    auto candidate = ReadTemplateForSpawn(template_name);
    CommitSpawnTemplate(candidate);
    return std::move(candidate.actor);
}

// return 第一个名称匹配的 actor.
luabridge::LuaRef Actor::Find(const std::string &name) {
    return ComponentManager::FindActorByName(name);
}

// return所有名称匹配的 actor, 结果是 Lua 数组.
luabridge::LuaRef Actor::FindAll(const std::string &name) {
    return ComponentManager::FindAllActorsByName(name);
}

// 当前场景中实例化一个 actor, 返回其 Lua 引用.
luabridge::LuaRef Actor::Instantiate(const std::string &template_name) {
    return ComponentManager::InstantiateActor(template_name);
}

// 请求在本帧末销毁指定 actor.
void Actor::Destroy(Actor *actor) {
    ComponentManager::DestroyActor(actor);
}
