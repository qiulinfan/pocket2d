#include "scripting/LuaBoundary.h"
#include "Internal.h"
#include "core/Engine.h"
#include "rendering/SpriteRenderer.h"
#include "scripting/ComponentManager.h"
#include "scripting/EventBus.h"
#include "scripting/SceneComponentPlan.h"
#include "scripting/RuntimeActorPlan.h"
#include "scripting/ScriptReloadCandidate.h"
#include "particles/ParticleSystem.h"
#include "physics/Rigidbody.h"
#include "scene/Transform.h"
#include "scene/Tilemap.h"
#include "rendering/SpriteAnimation.h"
#include "scene/Scene.h"
#include "scene/ActorHandle.h"
#include "../luaapi/RegistrationDetail.h"
#include "shared/scene_format/SceneFormat.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

using namespace ManagerDetail;

namespace {

template <typename ValueType>
luabridge::LuaRef BuildLuaArrayTable(const std::vector<ValueType> &values) {
    luabridge::LuaRef table = MakeEmptyArrayTable();
    int lua_index = 1;
    for (const ValueType &element : values) {
        table[lua_index++] = element;
    }
    return table;
}

luabridge::LuaRef BuildLuaBoolArrayTable(const Actor::BoolArray &values) {
    luabridge::LuaRef table = MakeEmptyArrayTable();
    int lua_index = 1;
    for (bool element : values) {
        table[lua_index++] = element;
    }
    return table;
}

// Validate the graph that default cloning will own before allocating any clone
// with user-supplied metatables. Releasing a rejected candidate must not schedule
// copied __gc callbacks against the still-running world.
void ValidateDefaultMetatables(const luabridge::LuaRef &prototype) {
    lua_State *state=prototype.state();
    struct StackScope{lua_State *state;int top;~StackScope(){lua_settop(state,top);}} stack{state,lua_gettop(state)};
    std::vector<luabridge::LuaRef> roots;
    std::unordered_set<const void *> root_ids;
    auto current=prototype;
    while(current.isTable()) {
        current.push(state);
        if(!root_ids.insert(lua_topointer(state,-1)).second){lua_pop(state,1);break;}
        roots.push_back(current);
        luabridge::LuaRef base(state);
        if(lua_getmetatable(state,-1)) {
            lua_pushliteral(state,"__index");lua_rawget(state,-2);
            if(lua_istable(state,-1))base=luabridge::LuaRef::fromStack(state,-1);
            lua_pop(state,2);
        }
        lua_pop(state,1);current=std::move(base);
    }
    std::vector<luabridge::LuaRef> pending;
    const auto seen_fields=luabridge::newTable(state);
    for(const auto &root:roots) {
        root.push(state);const int source=lua_gettop(state);
        seen_fields.push(state);const int seen=lua_gettop(state);
        lua_pushnil(state);
        while(lua_next(state,source)) {
            lua_pushvalue(state,-2);lua_rawget(state,seen);const bool defined=lua_toboolean(state,-1);lua_pop(state,1);
            if(!defined) {
                lua_pushvalue(state,-2);lua_pushboolean(state,true);lua_rawset(state,seen);
                if(!(lua_type(state,-2)==LUA_TSTRING && lua_isfunction(state,-1))) {
                    pending.push_back(luabridge::LuaRef::fromStack(state,-2));
                    pending.push_back(luabridge::LuaRef::fromStack(state,-1));
                }
            }
            lua_pop(state,1);
        }
        lua_pop(state,2);
    }
    std::unordered_set<const void *> visited;
    while(!pending.empty()) {
        auto value=std::move(pending.back());pending.pop_back();
        if(!value.isTable())continue; // Functions/upvalues and native value metadata are not cloned tables.
        value.push(state);const int table=lua_gettop(state);const void *identity=lua_topointer(state,table);
        if(root_ids.contains(identity)||!visited.insert(identity).second){lua_pop(state,1);continue;}
        if(lua_getmetatable(state,table)) {
            for(const char *field:{"__gc","__close","__mode"}) {
                lua_pushstring(state,field);lua_rawget(state,-2);const bool forbidden=!lua_isnil(state,-1);lua_pop(state,1);
                if(forbidden)throw std::runtime_error(std::string("unsupported default metatable ")+field+"; construct owned/finalizable state in OnStart");
            }
            pending.push_back(luabridge::LuaRef::fromStack(state,-1));lua_pop(state,1);
        }
        lua_pushnil(state);
        while(lua_next(state,table)) {
            pending.push_back(luabridge::LuaRef::fromStack(state,-2));
            pending.push_back(luabridge::LuaRef::fromStack(state,-1));lua_pop(state,1);
        }
        lua_pop(state,1);
    }
}

// Mutable defaults belong to an instance. Traverse a finite object graph with
// an explicit worklist, preserving cycles/aliases without recursive C++ calls.
// Methods keep using the prototype; closures intentionally keep their upvalues.
void CopyComponentDefaults(const luabridge::LuaRef &instance,
                           const luabridge::LuaRef &prototype,
                           const std::string &type_name,
                           const luabridge::LuaRef *retained_instance = nullptr) {
    lua_State *state = prototype.state();
    struct StackScope {
        lua_State *state;
        int top;
        ~StackScope() { lua_settop(state, top); }
    } stack_scope{state, lua_gettop(state)};
    struct PendingTable {
        luabridge::LuaRef source;
        luabridge::LuaRef destination;
        bool prototype_root;
    };
    std::unordered_map<const void *, luabridge::LuaRef> copies;
    // Reload prepares missing defaults without mutating the retained instance.
    // Seed matching table identities so new aliases and prototype self-references
    // point into preserved state rather than an abandoned temporary clone.
    if (retained_instance) {
        struct Pair { luabridge::LuaRef source, target; int depth; };
        std::vector<Pair> aligned;
        auto enqueue = [&](const luabridge::LuaRef &source,
                           const luabridge::LuaRef &target, int depth) {
            source.push(state); const void *identity = lua_topointer(state,-1); lua_pop(state,1);
            const auto found = copies.find(identity);
            if (found != copies.end()) {
                if (!ManagerDetail::AreSameLuaRef(found->second,target))
                    throw std::runtime_error("new default aliases conflict with preserved instance state");
                return;
            }
            if (aligned.size() >= 100000 || depth > 64)
                throw std::runtime_error("retained default graph exceeds reload limit");
            // Record now, not when visiting children. Thus every root field's
            // table identity is known before a sibling map uses it as a key.
            copies.emplace(identity,target); aligned.push_back({source,target,depth});
        };
        enqueue(prototype,*retained_instance,0);
        for (std::size_t position=0; position<aligned.size(); ++position) {
            const auto pair=aligned[position];
            pair.source.push(state); const int source_index=lua_gettop(state);
            pair.target.push(state); const int target_index=lua_gettop(state);
            lua_pushnil(state);
            while (lua_next(state,source_index)) {
                if (lua_istable(state,-1)) {
                    if (lua_istable(state,-2)) {
                        const auto mapped=copies.find(lua_topointer(state,-2));
                        if (mapped==copies.end())
                            throw std::runtime_error("table-key default identity has no retained-state correspondence");
                        mapped->second.push(state);
                    } else lua_pushvalue(state,-2);
                    lua_rawget(state,target_index);
                    if (lua_istable(state,-1))
                        enqueue(luabridge::LuaRef::fromStack(state,-2),
                                luabridge::LuaRef::fromStack(state,-1),pair.depth+1);
                    lua_pop(state,1);
                }
                lua_pop(state,1);
            }
            lua_pop(state,2);
        }
    }
    std::vector<PendingTable> pending;
    std::vector<luabridge::LuaRef> prototypes;
    std::unordered_set<const void *> visited_prototypes;
    luabridge::LuaRef current_prototype = prototype;
    while (current_prototype.isTable()) {
        current_prototype.push(state);
        const int table_index = lua_gettop(state);
        const void *identity = lua_topointer(state, table_index);
        if (!visited_prototypes.insert(identity).second) {
            lua_pop(state, 1);
            break;
        }
        copies.emplace(identity, instance);
        prototypes.push_back(current_prototype);
        luabridge::LuaRef base(state);
        if (lua_getmetatable(state, table_index)) {
            lua_pushliteral(state, "__index");
            lua_rawget(state, -2);
            if (lua_istable(state, -1)) base = luabridge::LuaRef::fromStack(state, -1);
            // Function-valued __index is executable code, not static defaults.
            lua_pop(state, 2);
        }
        lua_pop(state, 1);
        current_prototype = base;
    }
    for (auto it = prototypes.rbegin(); it != prototypes.rend(); ++it)
        pending.push_back({*it, instance, true});
    const auto seen_root_fields = luabridge::newTable(state);

    auto push_copy = [&](int index) {
        index = lua_absindex(state, index);
        const int value_type = lua_type(state, index);
        if (value_type == LUA_TTABLE || value_type == LUA_TUSERDATA) {
            const void *identity = lua_topointer(state, index);
            const auto found = copies.find(identity);
            if (found != copies.end()) {
                found->second.push(state);
                return;
            }
            if (value_type == LUA_TTABLE) {
                auto source = luabridge::LuaRef::fromStack(state, index);
                auto destination = luabridge::newTable(state);
                copies.emplace(identity, destination);
                pending.push_back({source, destination, false});
                destination.push(state);
                return;
            }
            // These native userdatas are explicit values, not owned resources.
            if (luabridge::Stack<b2Vec2>::isInstance(state, index)) {
                auto value=luabridge::Stack<b2Vec2>::get(state,index);
                LuaBoundary::Check(value,"cloned default userdata");
                LuaBoundary::Check(luabridge::Stack<b2Vec2>::push(state,*value),"cloned default userdata push");
            } else if (luabridge::Stack<glm::vec2>::isInstance(state, index)) {
                auto value=luabridge::Stack<glm::vec2>::get(state,index);
                LuaBoundary::Check(value,"cloned default userdata");
                LuaBoundary::Check(luabridge::Stack<glm::vec2>::push(state,*value),"cloned default userdata push");
            } else if (luabridge::Stack<ActorHandle>::isInstance(state, index)) {
                auto value=luabridge::Stack<ActorHandle>::get(state,index);
                LuaBoundary::Check(value,"cloned default userdata");
                LuaBoundary::Check(luabridge::Stack<ActorHandle>::push(state,*value),"cloned default userdata push");
            } else {
                throw std::runtime_error("unsupported userdata default in component '" +
                    type_name + "'; construct owned objects in OnStart");
            }
            copies.emplace(identity, luabridge::LuaRef::fromStack(state, -1));
            return;
        }
        if (value_type == LUA_TTHREAD || value_type == LUA_TLIGHTUSERDATA) {
            throw std::runtime_error("unsupported " + std::string(lua_typename(state, value_type)) +
                " default in component '" + type_name + "'; construct it in OnStart");
        }
        lua_pushvalue(state, index);
    };

    while (!pending.empty()) {
        PendingTable current = std::move(pending.back());
        pending.pop_back();
        current.source.push(state);
        const int source_index = lua_gettop(state);
        current.destination.push(state);
        const int destination_index = lua_gettop(state);
        seen_root_fields.push(state);
        const int seen_index = lua_gettop(state);
        lua_pushnil(state);
        while (lua_next(state, source_index) != 0) {
            bool already_defined = false;
            if (current.prototype_root) {
                lua_pushvalue(state, -2);
                lua_rawget(state, seen_index);
                already_defined = lua_toboolean(state, -1);
                lua_pop(state, 1);
                if (!already_defined) {
                    lua_pushvalue(state, -2);
                    lua_pushboolean(state, true);
                    lua_rawset(state, seen_index);
                }
            }
            // Record methods as overrides too: a base table must never shadow
            // a derived method merely because methods stay on the prototype.
            const bool inherited_method = current.prototype_root &&
                lua_type(state, -2) == LUA_TSTRING && lua_isfunction(state, -1);
            if (!already_defined && !inherited_method) {
                push_copy(-2);
                push_copy(-2);
                lua_rawset(state, destination_index);
            }
            lua_pop(state, 1);
        }
        if (!current.prototype_root && lua_getmetatable(state, source_index)) {
            push_copy(-1);
            lua_setmetatable(state, destination_index);
            lua_pop(state, 1);
        }
        lua_pop(state, 3);
    }
}

} // namespace

namespace ManagerDetail {

luabridge::LuaRef CloneComponentDefaults(const luabridge::LuaRef &prototype,
                                        const std::string &type_name,
                                        const luabridge::LuaRef *retained_instance) {
    ValidateDefaultMetatables(prototype);
    auto result = luabridge::newTable(Runtime().lua_state);
    CopyComponentDefaults(result, prototype, type_name, retained_instance);
    return result;
}

bool AssignPropertyValueToLuaField(luabridge::LuaRef &instance_table,
                                   const std::string &property_name,
                                   const Actor::ComponentPropertyValue &value) {
    std::visit(
        [&](const auto &typed_value) {
            using ValueType = std::decay_t<decltype(typed_value)>;
            if constexpr (std::is_same_v<ValueType, Actor::BoolArray>) {
                ProtectedSetField(instance_table, property_name.c_str(), BuildLuaBoolArrayTable(typed_value));
            } else if constexpr (std::is_same_v<ValueType, Actor::IntArray> ||
                                 std::is_same_v<ValueType, Actor::DoubleArray> ||
                                 std::is_same_v<ValueType, Actor::StringArray>) {
                ProtectedSetField(instance_table, property_name.c_str(), BuildLuaArrayTable(typed_value));
            } else {
                ProtectedSetField(instance_table, property_name.c_str(), luabridge::LuaRef(Runtime().lua_state, typed_value));
            }
        },
        value);
    return true;
}

} // namespace ManagerDetail

namespace {

void RotateClockwise(float x, float y, float rotation_degrees, float &out_x,
                     float &out_y) {
    const float radians = rotation_degrees * (3.14159265358979323846f / 180.0f);
    const float cos_theta = std::cos(radians);
    const float sin_theta = std::sin(radians);
    out_x = cos_theta * x + sin_theta * y;
    out_y = -sin_theta * x + cos_theta * y;
}

void ResolveRuntimeLocalTransformFromWorld(
    Actor::UID actor_uid, float world_x, float world_y, float world_rotation,
    float &out_local_x, float &out_local_y, float &out_local_rotation) {
    out_local_x = world_x;
    out_local_y = world_y;
    out_local_rotation = world_rotation;

    auto actor_it = Runtime().actor_by_uid.find(actor_uid);
    if (actor_it == Runtime().actor_by_uid.end() || actor_it->second == nullptr) {
        return;
    }

    Actor *actor = actor_it->second;
    if (actor->parent_uid == Actor::kInvalidUID || actor->parent_uid == actor_uid) {
        return;
    }

    /*
    Runtime transform editing speaks in world space for some call sites. If the
    actor has a parent, convert the requested world pose back into local space
    before writing into the built-in Transform component.
    */
    float parent_world_x = 0.0f;
    float parent_world_y = 0.0f;
    float parent_world_rotation = 0.0f;
    if (!ComponentManager::TryGetRuntimeTransformWorld(
            actor->parent_uid, parent_world_x, parent_world_y,
            parent_world_rotation, nullptr)) {
        return;
    }

    const float relative_world_x = world_x - parent_world_x;
    const float relative_world_y = world_y - parent_world_y;
    RotateClockwise(relative_world_x, relative_world_y, -parent_world_rotation,
                    out_local_x, out_local_y);
    out_local_rotation = world_rotation - parent_world_rotation;
}

void SyncTransformAndRigidbodyAfterPropertyEdit(
    Actor::UID actor_uid, ComponentRecord &edited_component,
    const std::string &property_name,
    const Actor::ComponentPropertyValue &) {
    if (property_name != "x" && property_name != "y" &&
        property_name != "rotation") {
        return;
    }

    if (edited_component.type == "Transform") {
        /*
        Transform edits must immediately push a matching pose into Rigidbody so
        rendering and physics do not disagree for one frame in editor/runtime
        inspection paths.
        */
        ComponentRecord *rigidbody_component =
            FindPrimaryComponentByType(actor_uid, "Rigidbody");
        if (rigidbody_component == nullptr) return;

        Transform *transform = nullptr;
        Rigidbody *rigidbody = nullptr;
        try {
            transform = LuaBoundary::Cast<Transform *>(edited_component.instance_table);
            rigidbody = LuaBoundary::Cast<Rigidbody *>(rigidbody_component->instance_table);
        } catch (const LuaBoundary::Error &) {
            return;
        }
        if (transform == nullptr || rigidbody == nullptr) return;

        float world_x = transform->x;
        float world_y = transform->y;
        float world_rotation = transform->rotation;
        if (ComponentManager::TryGetRuntimeTransformWorld(
                actor_uid, world_x, world_y, world_rotation, nullptr)) {
            rigidbody->SetPosition(b2Vec2(world_x, world_y));
            rigidbody->SetRotation(world_rotation);
            return;
        }

        rigidbody->SetPosition(b2Vec2(transform->x, transform->y));
        rigidbody->SetRotation(transform->rotation);
        return;
    }

    if (edited_component.type != "Rigidbody") return;

    /*
    The reverse direction matters too: when Rigidbody properties are edited
    directly, keep the built-in Transform aligned so hierarchy/world resolve
    continues to see one coherent pose.
    */
    ComponentRecord *transform_component =
        FindPrimaryComponentByType(actor_uid, "Transform");
    if (transform_component == nullptr) return;

    Transform *transform = nullptr;
    Rigidbody *rigidbody = nullptr;
    try {
        transform = LuaBoundary::Cast<Transform *>(transform_component->instance_table);
        rigidbody = LuaBoundary::Cast<Rigidbody *>(edited_component.instance_table);
    } catch (const LuaBoundary::Error &) {
        return;
    }
    if (transform == nullptr || rigidbody == nullptr) return;

    float local_x = rigidbody->x;
    float local_y = rigidbody->y;
    float local_rotation = rigidbody->rotation;
    ResolveRuntimeLocalTransformFromWorld(actor_uid, rigidbody->x,
                                          rigidbody->y,
                                          rigidbody->rotation, local_x, local_y,
                                          local_rotation);
    transform->x = local_x;
    transform->y = local_y;
    transform->rotation = local_rotation;
}

} // namespace

// rebind actor pointers after a scene container changes
Actor *ComponentManager::ResolveActorHandle(Actor::UID uid, std::uint64_t generation) {
    ManagerDetail::RuntimeState &runtime = ManagerDetail::Runtime();
    if (uid == Actor::kInvalidUID || generation == 0) return nullptr;
    if (runtime.pending_destroy_actor_uids.count(uid) != 0) return nullptr;
    const auto found = runtime.actor_by_uid.find(uid);
    if (found == runtime.actor_by_uid.end() || found->second == nullptr) return nullptr;
    Actor *actor = found->second;
    return !actor->runtime_destroyed && actor->runtime_generation == generation ? actor : nullptr;
}

void ComponentManager::BindActorsForScene(std::list<Actor> &actors) {
    // Bind after scene vector is finalized, so pointers injected into Lua
    // refer to stable actor objects for this scene lifetime.
    Runtime().scene_actors = &actors;
    Runtime().actor_by_uid.clear();
    Runtime().actors_by_name.clear();
    Runtime().actor_order_by_uid.clear();
    Runtime().actor_uids_sorted.clear();
    Runtime().component_index_by_key.clear();
    Runtime().component_first_key_by_type.clear();
    Runtime().component_keys_by_type.clear();
    Runtime().on_update_components.clear();
    Runtime().on_late_update_components.clear();
    Runtime().dirty_component_actor_uids.clear();
    Runtime().actor_by_uid.reserve(actors.size());
    Runtime().actor_order_by_uid.reserve(actors.size());
    Runtime().actor_uids_sorted.reserve(actors.size());

    for (Actor &actor : actors) {
        if (actor.runtime_destroyed) continue;
        if (actor.runtime_generation == 0) actor.AssignRuntimeGeneration();
        const size_t actor_order = Runtime().actor_uids_sorted.size();
        Runtime().actor_by_uid[actor.uid] = &actor;
        Runtime().actors_by_name[actor.actor_name].push_back(&actor);
        Runtime().actor_order_by_uid[actor.uid] = actor_order;
        Runtime().actor_uids_sorted.push_back(actor.uid);
        SortComponentsForActor(actor.uid);
        auto component_it = Runtime().actor_components.find(actor.uid);
        if (component_it == Runtime().actor_components.end()) continue;
        for (std::unique_ptr<ComponentRecord> &component_ptr :
             component_it->second) {
            ComponentRecord &component = *component_ptr;
            // Flexible reference: each component can reach its owner actor.
            SetSystemField(component.instance_table, "actor", luabridge::LuaRef(Runtime().lua_state, ActorHandle(&actor)));
            if (component.removed) continue;
            if (component.has_on_update) {
                Runtime().on_update_components.emplace_back(actor.uid,
                                                           &component);
            }
            if (component.has_on_late_update) {
                Runtime().on_late_update_components.emplace_back(actor.uid,
                                                                 &component);
            }
        }
    }
    Runtime().next_actor_order = Runtime().actor_uids_sorted.size();
    std::sort(Runtime().on_update_components.begin(),
              Runtime().on_update_components.end(),
              CompareLifecycleComponentRef);
    std::sort(Runtime().on_late_update_components.begin(),
              Runtime().on_late_update_components.end(),
              CompareLifecycleComponentRef);
}

// -----------------------------------------------------------------------------
// ComponentManager public API: component / actor creation
// -----------------------------------------------------------------------------

// instantiate one component from parsed scene / template spec
void ComponentManager::InstantiateComponentForActor(
    Actor::UID actor_uid, const Actor::ComponentSpec &component_spec) {
    // Construction is callback-free even if a Lua allocation would otherwise
    // schedule a user finalizer. Nested scene plans already keep GC paused.
    struct ConstructionGC {
        lua_State *state; bool running;
        explicit ConstructionGC(lua_State *value):state(value),running(lua_gc(value,LUA_GCISRUNNING)!=0){lua_gc(state,LUA_GCSTOP);}
        ~ConstructionGC(){if(running)lua_gc(state,LUA_GCRESTART);}
    } gc(Runtime().lua_state);
    if (component_spec.type == "Tilemap" || component_spec.type == "Rigidbody") {
        const bool has_map = FindPrimaryComponentByType(actor_uid, "Tilemap") != nullptr;
        const bool has_body = FindPrimaryComponentByType(actor_uid, "Rigidbody") != nullptr;
        if ((component_spec.type == "Rigidbody" && has_map) ||
            (component_spec.type == "Tilemap" && (has_map || has_body)))
            throw std::runtime_error("Tilemap actor cannot contain another Tilemap or Rigidbody");
    }
    luabridge::LuaRef prototype(Runtime().lua_state);
    if (!IsBuiltinComponentType(component_spec.type)) {
        const auto found = Runtime().component_type_tables.find(component_spec.type);
        if (found == Runtime().component_type_tables.end()) {
            std::cout << "warning: failed to locate component " << component_spec.type << "; skipping component" << std::endl;
            return;
        }
        prototype = found->second;
    }
    auto prepared = PrepareComponentRecord(component_spec, prototype);
    if (Runtime().pending_destroy_actor_uids.count(actor_uid)) return;
    InstallPreparedComponent(actor_uid, std::move(prepared));
    if (component_spec.type == "Tilemap") {
        try { InitializeTilemap(actor_uid, component_spec.key); }
        catch (...) {
            // Failed asset setup must not leave a half-registered native object.
            auto *failed = FindComponentRecord(actor_uid, component_spec.key);
            if (failed != nullptr) failed->removed = true;
            throw;
        }
    }
}

// runtime component add / remove
luabridge::LuaRef ComponentManager::AddComponent(Actor::UID actor_uid,
                                                 const std::string &type_name) {
    // AddComponent 的语义: 立即创建并返回 ref, 但生命周期从下一帧开始.
    if (Runtime().lua_state == nullptr) return MakeNilRef();
    auto actor_ptr_it = Runtime().actor_by_uid.find(actor_uid);
    if (actor_ptr_it == Runtime().actor_by_uid.end() ||
        actor_ptr_it->second == nullptr) {
        return MakeNilRef();
    }
    if (Runtime().pending_destroy_actor_uids.find(actor_uid) !=
        Runtime().pending_destroy_actor_uids.end()) {
        return MakeNilRef();
    }

    Actor::ComponentSpec component_spec;
    component_spec.type = type_name;
    // 运行时组件 key: r<n>
    component_spec.key = "r" + std::to_string(Runtime().runtime_add_component_counter++);
    InstantiateComponentForActor(actor_uid, component_spec);
    SortComponentsForActor(actor_uid);

    ComponentRecord *created = FindComponentRecord(actor_uid,
                                                   component_spec.key);
    if (created == nullptr) return MakeNilRef();
    return created->instance_table;
}

// runtime component add / remove
void ComponentManager::RemoveComponent(Actor::UID actor_uid,
                                       luabridge::LuaRef component_ref) {
    // 立即逻辑移除: 标记 removed + enabled=false
    // 物理清理由 FinalizeFrameMutations 在帧末统一做
    auto actor_it = Runtime().actor_components.find(actor_uid);
    if (actor_it == Runtime().actor_components.end()) return;

    ComponentRecord *target = nullptr;
    // Identity comes from the registered object, never a script-controlled key
    // getter (which could reenter or point at a different component).
    for (const auto &component : actor_it->second) {
        if (!component->removed && AreSameLuaRef(component->instance_table, component_ref)) {
            target = component.get();
            break;
        }
    }

    if (target == nullptr) return;
    target->removed = true;
    SetSystemField(target->instance_table, "enabled", luabridge::LuaRef(Runtime().lua_state, false));
    auto index_it = Runtime().component_index_by_key.find(actor_uid);
    if (index_it != Runtime().component_index_by_key.end()) {
        index_it->second.erase(target->key);
    }
    RebuildTypeIndexForActor(actor_uid);
    Runtime().dirty_component_actor_uids.insert(actor_uid);
}

// runtime component add / remove
bool ComponentManager::RenameComponentKey(Actor::UID actor_uid,
                                          const std::string &old_key,
                                          const std::string &new_key) {
    if (!Runtime().lua_state || old_key.empty() || new_key.empty() || old_key == new_key)
        return false;
    if (APIRegistrationDetail::BoundEngine() && APIRegistrationDetail::BoundEngine()->RequiresSceneReset())
        return false;
    if (FindComponentRecord(actor_uid, new_key) != nullptr) return false;
    ComponentRecord *component = FindComponentRecord(actor_uid, old_key);
    if (component == nullptr) return false;

    // Keep finalizers outside the identity publication boundary, including when
    // this operation nests inside another prepared owner-thread command.
    struct GCScope {
        lua_State *state;
        explicit GCScope(lua_State *value) : state(value) { PauseScenePreparationGC(state); }
        ~GCScope() { ResumeScenePreparationGC(state); }
    } gc(Runtime().lua_state);
    const int stack_top = lua_gettop(Runtime().lua_state);
    bool publishing = false;
    try {
        const std::string previous_key = old_key;
        const std::string replacement_key = new_key;
        EventBus::KeyRemapPlan events;
        std::string error;
        if (!EventBus::PrepareKeyRemap(
                {{component->instance_table, actor_uid, previous_key, replacement_key}}, events, error))
            return false;

        publishing = true;
        SetSystemField(component->instance_table, "key", luabridge::LuaRef(Runtime().lua_state, replacement_key));
        component->key = replacement_key;
        for (auto &pending : Runtime().pending_on_start) {
            if (pending.actor_uid == actor_uid && pending.component_key == previous_key)
                pending.component_key = replacement_key;
        }
        SortComponentsForActor(actor_uid);
        EventBus::CommitKeyRemap(std::move(events));
        // Deferred actors have not joined these lists yet. Activation appends
        // them once; rebuilding here would cause duplicate Update/Late calls.
        if (std::find(Runtime().pending_actor_uids_to_activate.begin(),
                      Runtime().pending_actor_uids_to_activate.end(), actor_uid) ==
            Runtime().pending_actor_uids_to_activate.end())
            RebuildLifecycleListsForDirtyActors(std::unordered_set<Actor::UID>{actor_uid});
        return true;
    } catch (const std::exception &error) {
        lua_settop(Runtime().lua_state, stack_top);
        if (publishing && APIRegistrationDetail::BoundEngine())
            APIRegistrationDetail::BoundEngine()->ReportRuntimeActorPublicationFailure(error.what());
        return false;
    }
}

// runtime component add / remove
bool ComponentManager::SetComponentPropertyValue(
    Actor::UID actor_uid, const std::string &component_key,
    const std::string &property_name,
    const Actor::ComponentPropertyValue &value) {
    if (property_name.empty() || property_name == "actor" || property_name == "key") return false;
    const int stack_top = lua_gettop(Runtime().lua_state);

    ComponentRecord *component = FindComponentRecord(actor_uid, component_key);
    if (component == nullptr) return false;

    try {
        if (component->type == "SpriteAnimation")
            return SetSpriteAnimationProperty(actor_uid, component_key, property_name, value);
        if (component->type == "Tilemap") {
            if (property_name == "map_resource") {
                const auto *resource = std::get_if<std::string>(&value);
                return resource != nullptr && SetTilemapResource(actor_uid, component_key, *resource);
            }
            if (property_name != "enabled" || !std::holds_alternative<bool>(value)) return false;
        }
        auto reference = component->instance_table;
        AssignPropertyValueToLuaField(reference, property_name, value);
        if (!IsComponentRefAlive(reference, actor_uid, component_key)) return false;
        component = FindComponentRecord(actor_uid, component_key);

        if (component->type == "ParticleSystem" && property_name == "enabled") {
            ParticleSystem *system = LuaBoundary::Cast<ParticleSystem *>(component->instance_table);
            if (system != nullptr) {
                system->SyncRuntimeState();
            }
        }

        // Built-in Transform and Rigidbody now coexist in authoring/runtime
        // scenes. Keep their pose values aligned immediately so editor edits do
        // not leave rendering and physics looking out of sync for a frame.
        SyncTransformAndRigidbodyAfterPropertyEdit(actor_uid, *component,
                                                   property_name, value);
    } catch (const LuaBoundary::Error &) {
        lua_settop(Runtime().lua_state, stack_top);
        return false;
    } catch (const std::exception &error) {
        std::cerr << "component property rejected: " << error.what() << '\n';
        return false;
    }

    return true;
}

bool ComponentManager::SetRuntimeComponentPropertyValue(
    Actor::UID actor_uid, const std::string &component_key,
    const std::string &property_name,
    const Actor::ComponentPropertyValue &value) {
    if (property_name.empty() || property_name == "actor" || property_name == "key") return false;

    auto actor_it = Runtime().actor_by_uid.find(actor_uid);
    if (actor_it == Runtime().actor_by_uid.end() || actor_it->second == nullptr) {
        return false;
    }

    Actor *actor = actor_it->second;
    if (actor->runtime_destroyed) return false;

    Actor::ComponentSpec *component_spec =
        SceneFormat::FindComponentSpec(actor->component_specs, component_key);
    if (component_spec == nullptr) return false;

    Actor::ComponentSpec candidate = *component_spec;
    SceneFormat::UpsertComponentProperty(candidate, property_name, value);
    std::unique_ptr<PreparedRuntimeComponent> prepared;
    if (IsBuiltinComponentType(candidate.type)) {
        std::string preparation_error;
        prepared = PrepareRuntimeComponent(candidate, preparation_error);
        if (!prepared) {
            std::cerr << "component property rejected: " << preparation_error << '\n';
            return false;
        }
    }
    const bool can_patch_in_place =
        !IsBuiltinComponentType(component_spec->type) ||
        component_spec->type == "Transform" || component_spec->type == "SpriteRenderer" ||
        component_spec->type == "Tilemap" || component_spec->type == "SpriteAnimation";
    if (can_patch_in_place) {
        if (!SetComponentPropertyValue(actor_uid, component_key, property_name, value)) return false;
        SceneFormat::UpsertComponentProperty(*component_spec, property_name, value);
        return true;
    }
    const auto component_ref = ComponentManager::GetComponentByKey(actor_uid, component_key);
    if (component_ref.isNil()) return false;
    ComponentManager::RemoveComponent(actor_uid, component_ref);
    ComponentManager::FinalizeFrameMutations();
    component_spec = SceneFormat::FindComponentSpec(actor->component_specs, component_key);
    if (component_spec == nullptr) throw std::runtime_error("component authoring spec disappeared during replacement");
    *component_spec = std::move(candidate);
    InstallRuntimeComponent(*prepared, actor_uid);
    if (Runtime().scene_actors != nullptr) {
        ComponentManager::BindActorsForScene(*Runtime().scene_actors);
    }
    return true;
}

// actor-level helpers exposed through Actor static APIs
luabridge::LuaRef ComponentManager::InstantiateActor(const std::string &template_name) {
    using APIRegistrationDetail::BoundEngine;
    if (!Runtime().lua_state || !Runtime().scene_actors || !BoundEngine()) return MakeNilRef();
    if (BoundEngine()->RequiresSceneReset()) throw std::runtime_error("runtime requires reset");
    std::unique_ptr<PreparedRuntimeActor> prepared;
    try {
        const auto candidate = Actor::ReadTemplateForSpawn(template_name);
        Actor actor = candidate.actor;
        actor.uid = BoundEngine()->AllocateRuntimeGeneratedActorUID();
        actor.runtime_generation = 0;
        actor.scene_backed = false;
        actor.parent_uid = Actor::kInvalidUID;
        actor.runtime_destroyed = false;
        std::string error;
        prepared = PrepareRuntimeActor(std::move(actor), error);
        if (!prepared) throw std::runtime_error(error);
        Actor::CommitSpawnTemplate(candidate);
        Actor *published = PublishRuntimeActor(*prepared, *Runtime().scene_actors, true);
        BoundEngine()->NotifyRuntimeActorPublished(*published);
        return luabridge::LuaRef(Runtime().lua_state, ActorHandle(published));
    } catch (const std::exception &failure) {
        if (prepared && prepared->CommitStarted()) BoundEngine()->ReportRuntimeActorPublicationFailure(failure.what());
        throw std::runtime_error(ScriptReloadCandidate::SanitizeDiagnosticText(failure.what()));
    }
}

// actor-level helpers exposed through Actor static APIs
void ComponentManager::DestroyActor(Actor *actor) {
    // Actor.Destroy 语义：
    // 立即禁用其所有组件 (本帧后续生命周期不再执行)
    // 真正从索引移除放到帧末处理
    if (actor == nullptr) return;
    if (actor->runtime_destroyed) return;

    const Actor::UID actor_uid = actor->uid;
    if (Runtime().actor_by_uid.find(actor_uid) == Runtime().actor_by_uid.end()) {
        return;
    }
    if (!Runtime().pending_destroy_actor_uids.insert(actor_uid).second) return;

    // Destroy 之后应当立即无法通过 Actor.Find / FindAll 找到该 actor
    // 真实物理清理仍在帧末统一完成
    RemoveActorFromNameIndex(actor);

    auto components_it = Runtime().actor_components.find(actor_uid);
    if (components_it != Runtime().actor_components.end()) {
        for (std::unique_ptr<ComponentRecord> &component_ptr :
             components_it->second) {
            ComponentRecord &component = *component_ptr;
            component.removed = true;
            SetSystemField(component.instance_table, "enabled", luabridge::LuaRef(Runtime().lua_state, false));
        }
        Runtime().component_index_by_key.erase(actor_uid);
        Runtime().component_first_key_by_type.erase(actor_uid);
        Runtime().component_keys_by_type.erase(actor_uid);
        Runtime().dirty_component_actor_uids.insert(actor_uid);
    }
}

// actor-level helpers exposed through Actor instance APIs
bool ComponentManager::SetActorParent(Actor::UID actor_uid, Actor *parent_actor) {
    using APIRegistrationDetail::BoundEngine;

    if (BoundEngine() == nullptr) return false;
    const Actor::UID parent_uid =
        (parent_actor == nullptr) ? Actor::kInvalidUID : parent_actor->uid;
    return BoundEngine()->SetRuntimeActorParentByUID(actor_uid, parent_uid);
}
