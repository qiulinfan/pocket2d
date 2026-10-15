#include "scripting/LuaBoundary.h"
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

std::uint32_t g_random_seed = 0;
bool g_has_random_seed = false;

void ApplyRandomSeed() {
    if (!g_has_random_seed || g_runtime.lua_state == nullptr) return;
    lua_State *state = g_runtime.lua_state;
    lua_getglobal(state, "math");
    lua_getfield(state, -1, "randomseed");
    lua_pushinteger(state, g_random_seed);
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

bool g_script_print_to_stderr = false;

void RouteScriptDiagnostics() {
    lua_State *state = g_runtime.lua_state;
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

std::unordered_set<std::string> g_component_type_warning_keys;
int g_component_type_warning_count = 0;
std::string g_component_type_last_warning;


void ClearBoundActorState() {
    g_runtime.pending_on_start.clear();
    struct TeardownTarget { Actor::UID uid; luabridge::LuaRef reference; };
    while (true) {
        std::vector<TeardownTarget> targets;
        for (const auto &[uid, components] : g_runtime.actor_components)
            for (const auto &component : components)
                if (!component->on_destroy_called) targets.push_back({uid, component->instance_table});
        if (targets.empty()) break;
        for (const auto &target : targets) {
            auto found = g_runtime.actor_components.find(target.uid);
            if (found == g_runtime.actor_components.end()) continue;
            ComponentRecord *component = nullptr;
            for (const auto &candidate : found->second)
                if (AreSameLuaRef(candidate->instance_table, target.reference)) { component = candidate.get(); break; }
            if (component) RunComponentOnDestroyIfNeeded(*component, target.uid);
        }
    }
    // OnDestroy may enqueue additions; this scene is being discarded, so those
    // callbacks must not leak into the next scene's OnStart queue.
    g_runtime.pending_on_start.clear();
    g_runtime.actor_components.clear();
    ParticleManager::Clear();
    Rigidbody::DestroyPhysicsWorld();
    g_runtime.component_index_by_key.clear();
    g_runtime.component_first_key_by_type.clear();
    g_runtime.component_keys_by_type.clear();
    g_runtime.on_update_components.clear();
    g_runtime.on_late_update_components.clear();
    g_runtime.actor_by_uid.clear();
    g_runtime.actors_by_name.clear();
    g_runtime.actor_order_by_uid.clear();
    g_runtime.next_actor_order = 0;
    g_runtime.actor_uids_sorted.clear();
    g_runtime.pending_actor_uids_to_activate.clear();
    g_runtime.pending_destroy_actor_uids.clear();
    g_runtime.dirty_component_actor_uids.clear();
    g_runtime.scene_actors = nullptr;
}

} // namespace

namespace ManagerDetail {

void EstablishInheritance(luabridge::LuaRef &instance_table,
                          luabridge::LuaRef &parent_table) {
    // annoymous 中介, between instance and component type,
    // to avoid polluting component type table with instance-level state (e.g. self.key).
    // 这样 parent_table 只需要包含纯粹的方法和属性, 不需要感知自己是否被当作元表使用.
    luabridge::LuaRef new_metatable = luabridge::newTable(g_runtime.lua_state);
    new_metatable["__index"] = parent_table;
    instance_table.push(g_runtime.lua_state);
    new_metatable.push(g_runtime.lua_state);
    lua_setmetatable(g_runtime.lua_state, -2);
    lua_pop(g_runtime.lua_state, 1);
}

void LogComponentTypeWarningOnce(const std::string &key,
                                 const std::string &message) {
    if (g_component_type_warning_keys.find(key) !=
        g_component_type_warning_keys.end()) {
        return;
    }
    g_component_type_warning_keys.insert(key);
    g_component_type_last_warning = message;
    ++g_component_type_warning_count;
    std::cout << "warning: " << g_component_type_last_warning << std::endl;
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
    if (g_runtime.lua_state != nullptr) return;

    g_runtime.lua_state = ScriptBudget::NewState();
    if (g_runtime.lua_state == nullptr) throw std::runtime_error("unable to create Lua state");

    luaL_openlibs(g_runtime.lua_state);
    // Keep LuaBridge argument failures as Lua errors catchable by script pcall.
    // LuaBoundary owns checked host calls; do not opt into registry-level throws.
    ClearRuntimeErrors();
    ApplyRandomSeed();
    if (g_script_print_to_stderr) {
        RouteScriptDiagnostics();
    }
    EventBus::Hooks event_bus_hooks;
    event_bus_hooks.lua_state = g_runtime.lua_state;
    event_bus_hooks.are_same_ref = &AreSameLuaRef;
    event_bus_hooks.try_extract_component_identity = &TryExtractComponentIdentity;
    event_bus_hooks.is_component_ref_alive = &IsComponentRefAlive;
    event_bus_hooks.report_error = &ReportEventBusError;
    EventBus::Initialize(event_bus_hooks);

    APIRegistration::Initialize(g_runtime.lua_state);
    // Discover the actual host surface once; project files must not shadow a
    // native namespace merely by sharing its filename. Trusted code remains
    // free to explicitly mutate globals after this boundary.
    g_runtime.reserved_component_type_names.clear();
    lua_pushglobaltable(g_runtime.lua_state);
    lua_pushnil(g_runtime.lua_state);
    while (lua_next(g_runtime.lua_state, -2)) {
        if (lua_type(g_runtime.lua_state, -2) == LUA_TSTRING) {
            std::size_t length = 0;
            const char *name = lua_tolstring(g_runtime.lua_state, -2, &length);
            g_runtime.reserved_component_type_names.emplace(name, length);
        }
        lua_pop(g_runtime.lua_state, 1);
    }
    lua_pop(g_runtime.lua_state, 1);
}

void ComponentManager::SetRandomSeed(std::uint32_t seed) {
    g_random_seed = seed;
    g_has_random_seed = true;
    ApplyRandomSeed();
}

void ComponentManager::RouteScriptPrintToStderr() {
    g_script_print_to_stderr = true;
    if (g_runtime.lua_state != nullptr) {
        RouteScriptDiagnostics();
    }
}


int ComponentManager::GetComponentTypeWarningCount() {
    return g_component_type_warning_count;
}

const std::string &ComponentManager::GetLastComponentTypeWarning() {
    return g_component_type_last_warning;
}

// Tear down the shared Lua runtime and every cached component-type table.
void ComponentManager::Shutdown() {
    Actor::ClearTemplateCache();
    // 先清空所有持有 LuaRef 的容器, 再关闭 lua_state
    if (g_runtime.lua_state == nullptr) return;

    EventBus::Shutdown();
    ClearBoundActorState();
    APIRegistration::BindEngine(nullptr);
    g_runtime.runtime_add_component_counter = 0;
    g_runtime.component_type_tables.clear();
    g_runtime.component_type_sources.clear();
    g_runtime.component_source_cache.clear();
    g_runtime.component_source_owners.clear();
    g_runtime.reserved_component_type_names.clear();
    g_runtime.component_types_bootstrapped = false;
    g_runtime.scene_script_gc_guards = 0;
    g_runtime.scene_script_gc_was_running = false;
    g_component_type_warning_keys.clear();
    g_component_type_warning_count = 0;
    g_component_type_last_warning.clear();
    ScriptBudget::CloseState(g_runtime.lua_state);
    g_runtime.lua_state = nullptr;
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
