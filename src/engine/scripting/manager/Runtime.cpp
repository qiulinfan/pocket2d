#include "scripting/LuaBoundary.h"
#include "core/Log.h"
#include <stdexcept>
#include "Internal.h"
#include "shared/resources/ResourcePath.h"
#include "scripting/ComponentManager.h"
#include "scripting/ScriptBudget.h"
#include "particles/ParticleManager.h"
#include "physics/Rigidbody.h"
#include "scene/Scene.h"
#include "scripting/APIRegistration.h"
#include "scripting/EventBus.h"
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <unordered_set>

using namespace ManagerDetail;

namespace {

struct ScriptSettings {
    std::uint32_t random_seed = 0;
    bool has_random_seed = false;
    bool script_print_to_stderr = false;
    std::unordered_set<std::string> component_type_warning_keys;
    int component_type_warning_count = 0;
    std::string component_type_last_warning;
};
ScriptSettings &Settings() { return World::Current().State<ScriptSettings>(); }

void ApplyRandomSeed() {
    if (!Settings().has_random_seed || Runtime().lua_state == nullptr) return;
    lua_State *state = Runtime().lua_state;
    lua_getglobal(state, "math");
    lua_getfield(state, -1, "randomseed");
    lua_pushinteger(state, Settings().random_seed);
    lua_call(state, 1, 0);
    lua_pop(state, 1);
}

int PrintToStderr(lua_State *state) {
    const int count = lua_gettop(state);
    for (int i = 1; i <= count; ++i) {
        size_t size = 0;
        const char *text = luaL_tolstring(state, i, &size);
        if (i > 1) std::fputc('\t', stderr);
        std::fwrite(text, 1, size, stderr);
        lua_pop(state, 1);
    }
    std::fputc('\n', stderr);
    return 0;
}

void RouteScriptDiagnostics() {
    lua_State *state = Runtime().lua_state;
    lua_pushcfunction(state, PrintToStderr);
    lua_setglobal(state, "print");
    lua_getglobal(state, "io");
    lua_getfield(state, -1, "stderr");
    lua_setfield(state, -2, "stdout");
    lua_getfield(state, -1, "output");
    lua_getfield(state, -2, "stderr");
    lua_call(state, 1, 0);
    lua_pop(state, 1);
}



void ClearBoundActorState() {
    Runtime().pending_on_start.clear();
    struct TeardownTarget { Actor::UID uid; luabridge::LuaRef reference; };
    while (true) {
        std::vector<TeardownTarget> targets;
        for (const auto &[uid, components] : Runtime().actor_components)
            for (const auto &component : components)
                if (!component->on_destroy_called) targets.push_back({uid, component->instance_table});
        if (targets.empty()) break;
        for (const auto &target : targets) {
            auto found = Runtime().actor_components.find(target.uid);
            if (found == Runtime().actor_components.end()) continue;
            ComponentRecord *component = nullptr;
            for (const auto &candidate : found->second)
                if (AreSameLuaRef(candidate->instance_table, target.reference)) { component = candidate.get(); break; }
            if (component) RunComponentOnDestroyIfNeeded(*component, target.uid);
        }
    }
    // OnDestroy may enqueue additions; this scene is being discarded, so those
    // callbacks must not leak into the next scene's OnStart queue.
    Runtime().pending_on_start.clear();
    Runtime().actor_components.clear();
    ParticleManager::Clear();
    Rigidbody::DestroyPhysicsWorld();
    Runtime().component_index_by_key.clear();
    Runtime().component_first_key_by_type.clear();
    Runtime().component_keys_by_type.clear();
    Runtime().on_update_components.clear();
    Runtime().on_late_update_components.clear();
    Runtime().actor_by_uid.clear();
    Runtime().actors_by_name.clear();
    Runtime().actor_order_by_uid.clear();
    Runtime().next_actor_order = 0;
    Runtime().actor_uids_sorted.clear();
    Runtime().pending_actor_uids_to_activate.clear();
    Runtime().pending_destroy_actor_uids.clear();
    Runtime().dirty_component_actor_uids.clear();
    Runtime().scene_actors = nullptr;
}

} // namespace

namespace ManagerDetail {

void EstablishInheritance(luabridge::LuaRef &instance_table,
                          luabridge::LuaRef &parent_table) {
    // annoymous 中介, between instance and component type,
    // to avoid polluting component type table with instance-level state (e.g. self.key).
    // 这样 parent_table 只需要包含纯粹的方法和属性, 不需要感知自己是否被当作元表使用.
    luabridge::LuaRef new_metatable = luabridge::newTable(Runtime().lua_state);
    new_metatable["__index"] = parent_table;
    instance_table.push(Runtime().lua_state);
    new_metatable.push(Runtime().lua_state);
    lua_setmetatable(Runtime().lua_state, -2);
    lua_pop(Runtime().lua_state, 1);
}

void LogComponentTypeWarningOnce(const std::string &key,
                                 const std::string &message) {
    if (Settings().component_type_warning_keys.find(key) !=
        Settings().component_type_warning_keys.end()) {
        return;
    }
    Settings().component_type_warning_keys.insert(key);
    Settings().component_type_last_warning = message;
    ++Settings().component_type_warning_count;
    Log::Warning("component", Settings().component_type_last_warning);
}


void ApplyPropertyOverrides(
    luabridge::LuaRef &instance_table,
    const std::vector<Actor::ComponentProperty> &property_overrides) {
    // 把 JSON override 覆盖写入实例 table
    for (const Actor::ComponentProperty &property : property_overrides) {
        // Reuse the same property writer used by runtime inspector edits so
        // arrays and scalar overrides produce identical Lua-side values.
        AssignPropertyValueToLuaField(instance_table, property.name,
                                      property.value);
    }
}


} // namespace ManagerDetail

// -----------------------------------------------------------------------------
// ComponentManager public API: initialization / teardown
// -----------------------------------------------------------------------------

// update the current engine pointer used by Lua APIs
void ComponentManager::BindEngine(Engine *engine) {
    APIRegistration::BindEngine(engine);
}

// Create the shared Lua VM and host APIs; project bootstrap is explicit.
void ComponentManager::Initialize() {
    // 只初始化一次, 全程复用同一个 Lua VM
    if (Runtime().lua_state != nullptr) return;

    Runtime().lua_state = ScriptBudget::NewState();
    if (Runtime().lua_state == nullptr) throw std::runtime_error("unable to create Lua state");

    luaL_openlibs(Runtime().lua_state);
    // Keep LuaBridge argument failures as Lua errors catchable by script pcall.
    // LuaBoundary owns checked host calls; do not opt into registry-level throws.
    ClearRuntimeErrors();
    ApplyRandomSeed();
    if (Settings().script_print_to_stderr) {
        RouteScriptDiagnostics();
    }
    EventBus::Hooks event_bus_hooks;
    event_bus_hooks.lua_state = Runtime().lua_state;
    event_bus_hooks.are_same_ref = &AreSameLuaRef;
    event_bus_hooks.try_extract_component_identity = &TryExtractComponentIdentity;
    event_bus_hooks.is_component_ref_alive = &IsComponentRefAlive;
    event_bus_hooks.report_error = &ReportEventBusError;
    EventBus::Initialize(event_bus_hooks);

    APIRegistration::Initialize(Runtime().lua_state);
    // Discover the actual host surface once; project files must not shadow a
    // native namespace merely by sharing its filename. Trusted code remains
    // free to explicitly mutate globals after this boundary.
    Runtime().reserved_component_type_names.clear();
    lua_pushglobaltable(Runtime().lua_state);
    lua_pushnil(Runtime().lua_state);
    while (lua_next(Runtime().lua_state, -2)) {
        if (lua_type(Runtime().lua_state, -2) == LUA_TSTRING) {
            std::size_t length = 0;
            const char *name = lua_tolstring(Runtime().lua_state, -2, &length);
            Runtime().reserved_component_type_names.emplace(name, length);
        }
        lua_pop(Runtime().lua_state, 1);
    }
    lua_pop(Runtime().lua_state, 1);
}

void ComponentManager::SetRandomSeed(std::uint32_t seed) {
    Settings().random_seed = seed;
    Settings().has_random_seed = true;
    ApplyRandomSeed();
}

void ComponentManager::RouteScriptPrintToStderr() {
    Settings().script_print_to_stderr = true;
    if (Runtime().lua_state != nullptr) {
        RouteScriptDiagnostics();
    }
}


int ComponentManager::GetComponentTypeWarningCount() {
    return Settings().component_type_warning_count;
}

const std::string &ComponentManager::GetLastComponentTypeWarning() {
    return Settings().component_type_last_warning;
}

// Tear down the shared Lua runtime and every cached component-type table.
void ComponentManager::Shutdown() {
    Actor::ClearTemplateCache();
    // 先清空所有持有 LuaRef 的容器, 再关闭 lua_state
    if (Runtime().lua_state == nullptr) return;

    EventBus::Shutdown();
    ClearBoundActorState();
    APIRegistration::BindEngine(nullptr);
    Runtime().runtime_add_component_counter = 0;
    Runtime().component_type_tables.clear();
    Runtime().component_type_sources.clear();
    Runtime().component_source_cache.clear();
    Runtime().component_source_owners.clear();
    Runtime().reserved_component_type_names.clear();
    Runtime().component_types_bootstrapped = false;
    Runtime().scene_script_gc_guards = 0;
    Runtime().scene_script_gc_was_running = false;
    Settings().component_type_warning_keys.clear();
    Settings().component_type_warning_count = 0;
    Settings().component_type_last_warning.clear();
    ScriptBudget::CloseState(Runtime().lua_state);
    Runtime().lua_state = nullptr;
}

// -----------------------------------------------------------------------------
// ComponentManager public API: scene binding
// -----------------------------------------------------------------------------

// clear all runtime component state tied to scene actors
void ComponentManager::ClearActorComponents() {
    // 场景切换时清理与 actor 相关的状态
    EventBus::Clear();
    ClearBoundActorState();
}
