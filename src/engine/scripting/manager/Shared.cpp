#include "scripting/LuaBoundary.h"
#include "Internal.h"
#include "rendering/SpriteRenderer.h"
#include "particles/ParticleSystem.h"
#include "scripting/ComponentManager.h"
#include "scripting/EventBus.h"
#include "scene/Transform.h"
#include "scene/ActorHandle.h"
#include <algorithm>
#include <cmath>
#include <iostream>

namespace {
int runtime_error_count = 0;
std::string last_runtime_error;
} // namespace

namespace ManagerDetail {

RuntimeState g_runtime;

bool IsBuiltinComponentType(const std::string &type_name) {
    return type_name == "Rigidbody" || type_name == "ParticleSystem" ||
           type_name == "Transform" || type_name == "SpriteRenderer" || type_name == "Tilemap" || type_name == "SpriteAnimation";
}

ComponentRecord *FindComponentRecord(Actor::UID actor_uid,
                                     const std::string &component_key) {
    auto actor_it = g_runtime.actor_components.find(actor_uid);
    if (actor_it == g_runtime.actor_components.end()) return nullptr;

    auto index_it = g_runtime.component_index_by_key.find(actor_uid);
    if (index_it != g_runtime.component_index_by_key.end()) {
        auto key_it = index_it->second.find(component_key);
        if (key_it != index_it->second.end()) {
            const size_t index = key_it->second;
            if (index < actor_it->second.size()) {
                ComponentRecord &component = *actor_it->second[index];
                if (!component.removed && component.key == component_key) {
                    return &component;
                }
            }
        }
    }

    // Fallback:: 索引缺失或过期时线性扫描并自愈索引
    std::vector<std::unique_ptr<ComponentRecord>> &components = actor_it->second;
    for (size_t i = 0; i < components.size(); i++) {
        ComponentRecord &component = *components[i];
        if (component.removed) continue;
        if (component.key != component_key) continue;
        g_runtime.component_index_by_key[actor_uid][component_key] = i;
        return &component;
    }
    return nullptr;
}

void RebuildComponentIndexForActor(Actor::UID actor_uid) {
    auto actor_it = g_runtime.actor_components.find(actor_uid);
    if (actor_it == g_runtime.actor_components.end()) {
        g_runtime.component_index_by_key.erase(actor_uid);
        return;
    }

    std::unordered_map<std::string, size_t> &key_index =
        g_runtime.component_index_by_key[actor_uid];
    key_index.clear();
    key_index.reserve(actor_it->second.size());
    for (size_t i = 0; i < actor_it->second.size(); i++) {
        const ComponentRecord &component = *actor_it->second[i];
        if (component.removed) continue;
        key_index[component.key] = i;
    }
}

void RebuildTypeIndexForActor(Actor::UID actor_uid) {
    auto actor_it = g_runtime.actor_components.find(actor_uid);
    if (actor_it == g_runtime.actor_components.end()) {
        g_runtime.component_first_key_by_type.erase(actor_uid);
        g_runtime.component_keys_by_type.erase(actor_uid);
        return;
    }

    std::unordered_map<std::string, std::string> &first_key_map =
        g_runtime.component_first_key_by_type[actor_uid];
    std::unordered_map<std::string, std::vector<std::string>> &keys_by_type =
        g_runtime.component_keys_by_type[actor_uid];
    first_key_map.clear();
    keys_by_type.clear();

    // 组件容器按 key 有序, 因此这里构建出来的 type 列表也天然按 key 有序
    const std::vector<std::unique_ptr<ComponentRecord>> &components = actor_it->second;
    for (const std::unique_ptr<ComponentRecord> &component_ptr : components) {
        const ComponentRecord &component = *component_ptr;
        if (component.removed) continue;
        std::vector<std::string> &keys = keys_by_type[component.type];
        if (keys.empty()) {
            first_key_map[component.type] = component.key;
        }
        keys.push_back(component.key);
    }
}

bool CompareLifecycleComponentRef(const LifecycleComponentRef &a,
                                  const LifecycleComponentRef &b) {
    const auto a_order_it = g_runtime.actor_order_by_uid.find(a.actor_uid);
    const auto b_order_it = g_runtime.actor_order_by_uid.find(b.actor_uid);
    const size_t a_order =
        (a_order_it == g_runtime.actor_order_by_uid.end())
            ? static_cast<size_t>(a.actor_uid)
            : a_order_it->second;
    const size_t b_order =
        (b_order_it == g_runtime.actor_order_by_uid.end())
            ? static_cast<size_t>(b.actor_uid)
            : b_order_it->second;
    if (a_order != b_order) return a_order < b_order;
    if (a.actor_uid != b.actor_uid) return a.actor_uid < b.actor_uid;
    const std::string a_key = (a.component == nullptr) ? std::string() : a.component->key;
    const std::string b_key = (b.component == nullptr) ? std::string() : b.component->key;
    return a_key < b_key;
}

// Lifecycle lists are cached globally so frame updates do not need to scan all
// components every tick.
void RebuildLifecycleListsForDirtyActors(
    const std::unordered_set<Actor::UID> &dirty_actor_uids) {
    if (dirty_actor_uids.empty()) return;

    auto is_dirty_actor = [&](const LifecycleComponentRef &entry) {
        return dirty_actor_uids.find(entry.actor_uid) != dirty_actor_uids.end();
    };

    // 先把 dirty actors 的旧 entry 从生命周期列表剔除
    g_runtime.on_update_components.erase(
        std::remove_if(g_runtime.on_update_components.begin(),
                       g_runtime.on_update_components.end(), is_dirty_actor),
        g_runtime.on_update_components.end());
    g_runtime.on_late_update_components.erase(
        std::remove_if(g_runtime.on_late_update_components.begin(),
                       g_runtime.on_late_update_components.end(), is_dirty_actor),
        g_runtime.on_late_update_components.end());

    // 再按当前组件状态重建 dirty actors 的 entry
    for (Actor::UID actor_uid : dirty_actor_uids) {
        if (g_runtime.pending_destroy_actor_uids.find(actor_uid) !=
            g_runtime.pending_destroy_actor_uids.end()) {
            continue;
        }

        auto actor_it = g_runtime.actor_components.find(actor_uid);
        if (actor_it == g_runtime.actor_components.end()) continue;

        for (std::unique_ptr<ComponentRecord> &component_ptr :
             actor_it->second) {
            ComponentRecord &component = *component_ptr;
            if (component.removed) continue;
            if (component.has_on_update) {
                g_runtime.on_update_components.emplace_back(actor_uid,
                                                           &component);
            }
            if (component.has_on_late_update) {
                g_runtime.on_late_update_components.emplace_back(actor_uid,
                                                                 &component);
            }
        }
    }

    // 保证执行顺序: actor_uid -> component_key
    std::sort(g_runtime.on_update_components.begin(),
              g_runtime.on_update_components.end(),
              CompareLifecycleComponentRef);
    std::sort(g_runtime.on_late_update_components.begin(),
              g_runtime.on_late_update_components.end(),
              CompareLifecycleComponentRef);
}

namespace {
struct LuaStackScope {
    lua_State *state;
    int top;
    ~LuaStackScope() { lua_settop(state, top); }
};
int ReadField(lua_State *state) { lua_settop(state, 2); lua_gettable(state, 1); return 1; }
int WriteField(lua_State *state) { lua_settop(state, 3); lua_settable(state, 1); return 0; }
}
luabridge::LuaRef ProtectedField(const luabridge::LuaRef &object, const char *name) {
    auto *state = g_runtime.lua_state;
    const LuaStackScope stack{state, lua_gettop(state)};
    // Ordinary prototype inheritance is a chain of raw table reads, not user
    // code. Resolve that common case without a protected Lua call. Keep one
    // table on the stack and bound traversal; functions, userdata, unusual
    // metamethod values and cycles still use Lua's own protected semantics.
    if (object.isTable()) {
        object.push(state);
        constexpr int max_raw_depth = 64;
        for (int depth = 0; depth < max_raw_depth; ++depth) {
            lua_pushstring(state, name);
            lua_rawget(state, -2);
            if (!lua_isnil(state, -1)) return luabridge::LuaRef::fromStack(state);
            lua_pop(state, 1);
            if (!lua_getmetatable(state, -1)) return luabridge::LuaRef(state);
            lua_pushliteral(state, "__index");
            lua_rawget(state, -2);
            lua_remove(state, -2); // metatable; leave current table + __index
            if (lua_isnil(state, -1)) return luabridge::LuaRef(state);
            if (!lua_istable(state, -1)) break;
            lua_replace(state, stack.top + 1); // next table replaces the current one
        }
        lua_settop(state, stack.top);
    }
    lua_pushcfunction(state, ReadField); object.push(state); lua_pushstring(state, name);
    LuaBoundary::ProtectedCall(state, 2, 1);
    return luabridge::LuaRef::fromStack(state);
}
void ProtectedSetField(const luabridge::LuaRef &object, const char *name, const luabridge::LuaRef &value) {
    auto *state = g_runtime.lua_state;
    const LuaStackScope stack{state, lua_gettop(state)};
    lua_pushcfunction(state, WriteField); object.push(state); lua_pushstring(state, name); value.push(state);
    LuaBoundary::ProtectedCall(state, 3, 0);
}
void SetSystemField(const luabridge::LuaRef &object, const char *name, const luabridge::LuaRef &value) {
    if (!object.isTable()) { ProtectedSetField(object, name, value); return; }
    auto *state = g_runtime.lua_state;
    const LuaStackScope stack{state, lua_gettop(state)};
    // Engine identity/disable writes must not call script __newindex during
    // container maintenance. User-facing property edits retain setter behavior.
    object.push(state); lua_pushstring(state, name); value.push(state); lua_rawset(state, -3);
}
bool IsComponentEnabled(const ComponentRecord &component) {
    if (component.removed) return false;
    const auto reference = component.instance_table;
    const auto enabled = ProtectedField(reference, "enabled");
    return LuaBoundary::IsEnabled(enabled);
}

void SyncBuiltinParticleSystemState(ComponentRecord &component) {
    if (component.type != "ParticleSystem") return;
    ParticleSystem *system = LuaBoundary::Cast<ParticleSystem *>(component.instance_table);
    if (system == nullptr) return;
    system->SyncRuntimeState();
}

ComponentRecord *FindPrimaryComponentByType(Actor::UID actor_uid,
                                            const std::string &type_name) {
    auto type_it = g_runtime.component_first_key_by_type.find(actor_uid);
    if (type_it == g_runtime.component_first_key_by_type.end()) return nullptr;
    auto key_it = type_it->second.find(type_name);
    if (key_it == type_it->second.end()) return nullptr;
    return FindComponentRecord(actor_uid, key_it->second);
}

void RotateClockwise(float x, float y, float rotation_degrees, float &out_x,
                     float &out_y) {
                         const float radians = rotation_degrees * (3.14159265358979323846f / 180.0f);
    const float cos_theta = std::cos(radians);
    const float sin_theta = std::sin(radians);
    out_x = cos_theta * x + sin_theta * y;
    out_y = -sin_theta * x + cos_theta * y;
}

void QueueBuiltinSpriteRendererDraws(bool scene_backed_only) {
    // Built-in render components draw during the render phase itself so they
    // still appear in editor frozen frames where gameplay updates are paused.
    // Sorting-order ties must preserve authored scene order, not hash-table
    // iteration order. This also makes overlapping fractional pixel edges
    // deterministic across the SpriteRenderer and Tilemap submission paths.
    for (Actor::UID uid : g_runtime.actor_uids_sorted) {
        auto components_it = g_runtime.actor_components.find(uid);
        if (components_it == g_runtime.actor_components.end()) continue;
        auto actor_it = g_runtime.actor_by_uid.find(uid);
        Actor *actor = (actor_it != g_runtime.actor_by_uid.end())
                           ? actor_it->second
                           : nullptr;
        if (scene_backed_only &&
            (actor == nullptr || !actor->IsSceneBacked())) {
            continue;
        }

        for (std::unique_ptr<ComponentRecord> &component_ptr :
             components_it->second) {
            ComponentRecord &component = *component_ptr;
            if (component.removed) continue;
            if (component.type != "SpriteRenderer") continue;

            SpriteRenderer *sprite_renderer = LuaBoundary::Cast<SpriteRenderer *>(component.instance_table);
            if (sprite_renderer == nullptr) continue;
            sprite_renderer->QueueDraw();
        }
    }
}

// Keep component order deterministic for all key-based lifecycle rules.
void SortComponentsForActor(Actor::UID actor_uid) {
    // 生命周期顺序要求按 key 字典序
    auto actor_it = g_runtime.actor_components.find(actor_uid);
    if (actor_it == g_runtime.actor_components.end()) return;
    std::sort(actor_it->second.begin(), actor_it->second.end(),
              [](const std::unique_ptr<ComponentRecord> &a,
                 const std::unique_ptr<ComponentRecord> &b) {
                  return a->key < b->key;
              });
    RebuildComponentIndexForActor(actor_uid);
    RebuildTypeIndexForActor(actor_uid);
}

void RemoveActorFromNameIndex(Actor *actor_ptr) {
    if (actor_ptr == nullptr) return;
    auto name_it = g_runtime.actors_by_name.find(actor_ptr->actor_name);
    if (name_it == g_runtime.actors_by_name.end()) return;
    std::vector<Actor *> &bucket = name_it->second;
    bucket.erase(std::remove(bucket.begin(), bucket.end(), actor_ptr),
                 bucket.end());
    if (bucket.empty()) {
        g_runtime.actors_by_name.erase(name_it);
    }
}

bool AreSameLuaRef(const luabridge::LuaRef &a, const luabridge::LuaRef &b) {
    // 用 raw lua_rawequal 比较两个 Lua table 是否同一对象
    if (g_runtime.lua_state == nullptr) return false;
    if (a.isNil() || b.isNil()) return false;
    a.push(g_runtime.lua_state);
    b.push(g_runtime.lua_state);
    const bool equal = lua_rawequal(g_runtime.lua_state, -1, -2) != 0;
    lua_pop(g_runtime.lua_state, 2);
    return equal;
}

luabridge::LuaRef MakeNilRef() {
    return luabridge::LuaRef(g_runtime.lua_state);
}

luabridge::LuaRef MakeEmptyArrayTable() {
    return luabridge::newTable(g_runtime.lua_state);
}

bool TryExtractComponentIdentity(const luabridge::LuaRef &component_ref,
                                 Actor::UID &actor_uid,
                                 std::string &component_key) {
    if (component_ref.isNil()) return false;
    const auto key_ref = ProtectedField(component_ref, "key");
    const auto actor_ref = ProtectedField(component_ref, "actor");
    if (!key_ref.isString() || !actor_ref.isInstance<ActorHandle>()) return false;
    auto *actor = LuaBoundary::Cast<ActorHandle>(actor_ref).Resolve();
    if (actor == nullptr) return false;
    actor_uid = actor->uid;
    component_key = LuaBoundary::Cast<std::string>(key_ref);
    return IsComponentRefAlive(component_ref, actor_uid, component_key);
}

bool IsComponentRefAlive(const luabridge::LuaRef &component_ref,
                         Actor::UID actor_uid,
                         const std::string &component_key) {
    ComponentRecord *component = FindComponentRecord(actor_uid, component_key);
    if (component == nullptr) return false;
    return AreSameLuaRef(component->instance_table, component_ref);
}

std::string GetActorNameByUID(Actor::UID actor_uid) {
    auto actor_it = g_runtime.actor_by_uid.find(actor_uid);
    if (actor_it == g_runtime.actor_by_uid.end() ||
        actor_it->second == nullptr) {
        return "";
    }
    return actor_it->second->GetName();
}

void ReportError(const std::string &actor_name,
                 const LuaBoundary::Error &e) {
    std::string error_message = e.what();
    std::replace(error_message.begin(), error_message.end(), '\\', '/');
    const LuaBoundary::Error bounded(actor_name + " : " + error_message);
    ++runtime_error_count;
    last_runtime_error = bounded.what();
    std::cout << "\033[31m" << last_runtime_error << "\033[0m" << std::endl;
}

void ReportEventBusError(Actor::UID actor_uid,
                         const LuaBoundary::Error &e) {
    ReportError(GetActorNameByUID(actor_uid), e);
}

void RunComponentOnDestroyIfNeeded(ComponentRecord &component, Actor::UID actor_uid) {
    if (component.on_destroy_called) return;
    // Mark before either getter or callback can reenter teardown. Copy everything
    // needed afterwards; callbacks can grow/sort the component container.
    component.on_destroy_called = true;
    const bool has_callback = component.has_on_destroy;
    const auto reference = component.instance_table;
    const auto key = component.key;
    const int top = lua_gettop(g_runtime.lua_state);
    if (has_callback) {
        try {
            const auto callback = ProtectedField(reference, "OnDestroy");
            if (callback.isFunction()) LuaBoundary::CallDiscard(callback, reference);
        } catch (const LuaBoundary::Error &error) {
            ReportError(GetActorNameByUID(actor_uid), error);
        }
    }
    lua_settop(g_runtime.lua_state, top);
    EventBus::RemoveSubscriptionsForComponent(reference, actor_uid, key);
}

} // namespace ManagerDetail

int ComponentManager::GetRuntimeErrorCount() { return runtime_error_count; }

const std::string &ComponentManager::GetLastRuntimeError() {
    return last_runtime_error;
}

void ComponentManager::ClearRuntimeErrors() {
    runtime_error_count = 0;
    last_runtime_error.clear();
}

void ComponentManager::QueueBuiltinRenderers(bool scene_backed_only) {
    ApplySpriteAnimationPoses();
    ManagerDetail::QueueBuiltinSpriteRendererDraws(scene_backed_only);
}

namespace {

// A resolver lives for one query or one full-scene pass. Local fields are
// writable from Lua, so retaining results across queries would make them stale.
class WorldTransformResolver {
public:
    void Resolve(Actor::UID actor_uid) {
        while (resolved_.find(actor_uid) == resolved_.end() &&
               resolving_.insert(actor_uid).second) {
            auto actor_it = ManagerDetail::g_runtime.actor_by_uid.find(actor_uid);
            Actor *actor = actor_it == ManagerDetail::g_runtime.actor_by_uid.end()
                               ? nullptr : actor_it->second;
            auto *component = ManagerDetail::FindPrimaryComponentByType(actor_uid, "Transform");
            Transform *transform = component == nullptr
                                       ? nullptr : LuaBoundary::Cast<Transform *>(component->instance_table);
            const Actor::UID parent_uid = actor == nullptr
                                             ? Actor::kInvalidUID : actor->parent_uid;
            path_.push_back({actor_uid, parent_uid, transform});
            if (transform == nullptr || parent_uid == Actor::kInvalidUID ||
                parent_uid == actor_uid) {
                break;
            }
            actor_uid = parent_uid;
        }

        // Unwind explicitly rather than recursively; imported/deep hierarchies
        // must not exhaust the native stack. A cycle uses the same already
        // stored parent world pose as the former recursive guard.
        while (!path_.empty()) {
            const Entry entry = path_.back();
            path_.pop_back();
            if (entry.transform != nullptr) {
                Transform *parent = nullptr;
                if (entry.parent_uid != Actor::kInvalidUID &&
                    entry.parent_uid != entry.uid) {
                    auto *component = ManagerDetail::FindPrimaryComponentByType(
                        entry.parent_uid, "Transform");
                    if (component != nullptr) {
                        parent = LuaBoundary::Cast<Transform *>(component->instance_table);
                    }
                }
                Transform &transform = *entry.transform;
                if (parent == nullptr) {
                    transform.world_x = transform.x;
                    transform.world_y = transform.y;
                    transform.world_rotation = transform.rotation;
                } else {
                    float rotated_x = 0.0f, rotated_y = 0.0f;
                    ManagerDetail::RotateClockwise(
                        transform.x, transform.y, parent->world_rotation,
                        rotated_x, rotated_y);
                    transform.world_x = parent->world_x + rotated_x;
                    transform.world_y = parent->world_y + rotated_y;
                    transform.world_rotation = parent->world_rotation + transform.rotation;
                }
            }
            resolving_.erase(entry.uid);
            resolved_.insert(entry.uid);
        }
    }

private:
    struct Entry {
        Actor::UID uid;
        Actor::UID parent_uid;
        Transform *transform;
    };
    std::unordered_set<Actor::UID> resolved_;
    std::unordered_set<Actor::UID> resolving_;
    std::vector<Entry> path_;
};

} // namespace

void ComponentManager::ResolveTransformHierarchy() {
    WorldTransformResolver resolver;
    for (const auto &entry : ManagerDetail::g_runtime.actor_by_uid) {
        if (entry.second == nullptr || entry.second->runtime_destroyed) continue;
        resolver.Resolve(entry.first);
    }
}

bool ComponentManager::TryGetRuntimeTransformWorld(
    Actor::UID actor_uid, float &x, float &y, float &rotation,
    std::string *out_component_key) {
    WorldTransformResolver resolver;
    resolver.Resolve(actor_uid);

    ManagerDetail::ComponentRecord *transform_component =
        ManagerDetail::FindPrimaryComponentByType(actor_uid, "Transform");
    if (transform_component == nullptr) return false;

    Transform *transform = LuaBoundary::Cast<Transform *>(transform_component->instance_table);
    if (transform == nullptr) return false;

    x = transform->world_x;
    y = transform->world_y;
    rotation = transform->world_rotation;
    if (out_component_key != nullptr) {
        *out_component_key = transform_component->key;
    }
    return true;
}

bool ComponentManager::SetRuntimeTransformWorldPosition(Actor::UID actor_uid,
                                                        float world_x,
                                                        float world_y) {
    auto actor_it = ManagerDetail::g_runtime.actor_by_uid.find(actor_uid);
    if (actor_it == ManagerDetail::g_runtime.actor_by_uid.end() ||
        actor_it->second == nullptr) {
        return false;
    }

    Actor *actor = actor_it->second;
    ManagerDetail::ComponentRecord *transform_component =
        ManagerDetail::FindPrimaryComponentByType(actor_uid, "Transform");
    if (transform_component == nullptr) return false;

    Transform *transform = LuaBoundary::Cast<Transform *>(transform_component->instance_table);
    if (transform == nullptr) return false;

    if (actor->parent_uid == Actor::kInvalidUID ||
        actor->parent_uid == actor_uid) {
        transform->x = world_x;
        transform->y = world_y;
        return true;
    }

    float parent_world_x = 0.0f;
    float parent_world_y = 0.0f;
    float parent_world_rotation = 0.0f;
    if (!TryGetRuntimeTransformWorld(actor->parent_uid, parent_world_x,
                                     parent_world_y, parent_world_rotation,
                                     nullptr)) {
        transform->x = world_x;
        transform->y = world_y;
        return true;
    }

    const float relative_world_x = world_x - parent_world_x;
    const float relative_world_y = world_y - parent_world_y;
    float local_x = 0.0f;
    float local_y = 0.0f;
    ManagerDetail::RotateClockwise(relative_world_x, relative_world_y,
                                   -parent_world_rotation, local_x, local_y);
    transform->x = local_x;
    transform->y = local_y;
    return true;
}
