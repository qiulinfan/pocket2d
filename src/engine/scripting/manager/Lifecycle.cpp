#include "scripting/LuaBoundary.h"
#include "core/Profile.h"
#include "Internal.h"
#include "core/Engine.h"
#include "scripting/ComponentManager.h"
#include "physics/Rigidbody.h"
#include "scripting/EventBus.h"
#include "scene/Transform.h"
#include "../luaapi/RegistrationDetail.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <cmath>

using namespace ManagerDetail;
using APIRegistrationDetail::BoundEngine;

namespace {

// Normal deferred spawns append in order. A host duplicate can be published
// between preparation and deferred activation, so insert only that inverted case.
void AppendOrderedLifecycle(std::vector<LifecycleComponentRef> &entries,
                            Actor::UID uid, ComponentRecord *record) {
    LifecycleComponentRef next(uid, record);
    if (entries.empty() || !CompareLifecycleComponentRef(next, entries.back())) entries.push_back(next);
    else entries.insert(std::lower_bound(entries.begin(), entries.end(), next,
                                        CompareLifecycleComponentRef), next);
}

struct PendingCollisionEvent {
    Actor::UID actor_uid = Actor::kInvalidUID;
    bool is_enter = true;
    Collision collision;

    PendingCollisionEvent(Actor::UID actor_uid_in, bool is_enter_in,
                          const Collision &collision_in)
        : actor_uid(actor_uid_in), is_enter(is_enter_in),
          collision(collision_in) {}
};

struct PendingTriggerEvent {
    Actor::UID actor_uid = Actor::kInvalidUID;
    bool is_enter = true;
    Collision collision;

    PendingTriggerEvent(Actor::UID actor_uid_in, bool is_enter_in,
                        const Collision &collision_in)
        : actor_uid(actor_uid_in), is_enter(is_enter_in),
          collision(collision_in) {}
};

ComponentRecord *FindRuntimeRigidbodyComponent(Actor::UID actor_uid) {
    ManagerDetail::RuntimeState &runtime = ManagerDetail::Runtime();
    ComponentRecord *rigidbody_component =
        FindComponentRecord(runtime, actor_uid, "Rigidbody");
    if (rigidbody_component != nullptr) return rigidbody_component;

    auto type_it = runtime.component_first_key_by_type.find(actor_uid);
    if (type_it == runtime.component_first_key_by_type.end()) return nullptr;
    auto rigidbody_key_it = type_it->second.find("Rigidbody");
    if (rigidbody_key_it == type_it->second.end()) return nullptr;
    return FindComponentRecord(runtime, actor_uid, rigidbody_key_it->second);
}

ComponentRecord *FindRuntimeTransformComponent(Actor::UID actor_uid) {
    auto type_it = Runtime().component_first_key_by_type.find(actor_uid);
    if (type_it == Runtime().component_first_key_by_type.end()) return nullptr;
    auto transform_key_it = type_it->second.find("Transform");
    if (transform_key_it == type_it->second.end()) return nullptr;
    return FindComponentRecord(actor_uid, transform_key_it->second);
}

Actor *FindRuntimeActor(Actor::UID actor_uid) {
    auto actor_it = Runtime().actor_by_uid.find(actor_uid);
    if (actor_it == Runtime().actor_by_uid.end()) return nullptr;
    return actor_it->second;
}

bool TryCastRuntimeRigidbody(ComponentRecord *component, Rigidbody *&out_rigidbody) {
    out_rigidbody = nullptr;
    if (component == nullptr || component->removed) return false;
    try {
        out_rigidbody = component->Native<Rigidbody>();
    } catch (const LuaBoundary::Error &) {
        return false;
    }
    return out_rigidbody != nullptr;
}

bool TryCastRuntimeTransform(ComponentRecord *component, Transform *&out_transform) {
    out_transform = nullptr;
    if (component == nullptr || component->removed) return false;
    try {
        out_transform = component->Native<Transform>();
    } catch (const LuaBoundary::Error &) {
        return false;
    }
    return out_transform != nullptr;
}

void RotateClockwiseLocal(float x, float y, float rotation_degrees,
                          float &out_x, float &out_y) {
    const float radians =
        rotation_degrees * (3.14159265358979323846f / 180.0f);
    const float cos_theta = std::cos(radians);
    const float sin_theta = std::sin(radians);
    out_x = cos_theta * x + sin_theta * y;
    out_y = -sin_theta * x + cos_theta * y;
}

void ResolveLocalTransformFromWorld(Actor *actor, float world_x, float world_y,
                                    float world_rotation, float &out_local_x,
                                    float &out_local_y,
                                    float &out_local_rotation) {
    out_local_x = world_x;
    out_local_y = world_y;
    out_local_rotation = world_rotation;
    if (actor == nullptr || actor->parent_uid == Actor::kInvalidUID ||
        actor->parent_uid == actor->uid) {
        return;
    }

    ComponentRecord *parent_transform_component =
        FindRuntimeTransformComponent(actor->parent_uid);
    Transform *parent_transform = nullptr;
    if (!TryCastRuntimeTransform(parent_transform_component, parent_transform) ||
        parent_transform == nullptr) {
        return;
    }

    const float relative_world_x = world_x - parent_transform->world_x;
    const float relative_world_y = world_y - parent_transform->world_y;
    RotateClockwiseLocal(relative_world_x, relative_world_y,
                         -parent_transform->world_rotation, out_local_x,
                         out_local_y);
    out_local_rotation = world_rotation - parent_transform->world_rotation;
}

using CallbackTarget = RecordSnapshot;
CallbackTarget Snapshot(Actor::UID uid, const ComponentRecord &component) { return SnapshotRecord(uid, component); }
ComponentRecord *Alive(RuntimeState &runtime, CallbackTarget &target) { return ResolveSnapshot(runtime, target); }
// Calls self[method](self[, collision]) unless self.enabled is false. Plain
// tables and their table __index chains are read on the stack; any metamethod
// that may run user code takes the protected path, which revalidates the
// component after each read.
void Invoke(CallbackTarget &target, const char *method, const Collision *collision = nullptr) {
    RuntimeState &runtime = Runtime();
    lua_State *state = runtime.lua_state;
    const int top = lua_gettop(state);
    struct Restore { lua_State *state; int top; ~Restore() { lua_settop(state, top); } } restore{state, top};
    try {
        ComponentRecord *record = Alive(runtime, target);
        if (record == nullptr) return;
        if (!lua_checkstack(state, 8)) throw LuaBoundary::Error("Lua callback could not reserve stack space");
        record->instance_table.push(state);
        if (PushRawField(state, "enabled")) {
            const bool enabled = lua_type(state, -1) != LUA_TBOOLEAN || lua_toboolean(state, -1) != 0;
            lua_pop(state, 1);
            if (!enabled) return;
            if (PushRawField(state, method)) {
                if (!lua_isfunction(state, -1)) return;
                lua_insert(state, -2);
                if (collision != nullptr) {
                    try {
                        LuaBoundary::Check(luabridge::Stack<Collision>::push(state, *collision), "Lua callback argument");
                    } catch (const luabridge::LuaException &failure) {
                        throw LuaBoundary::Error(failure.what());
                    }
                }
                LuaBoundary::ProtectedCall(state, collision != nullptr ? 2 : 1, 0);
                return;
            }
        }
        lua_settop(state, top);
        const luabridge::LuaRef reference = record->instance_table;
        const auto enabled = ProtectedField(reference, "enabled");
        if (!Alive(runtime, target) || !LuaBoundary::IsEnabled(enabled)) return;
        const auto callback = ProtectedField(reference, method);
        if (!Alive(runtime, target) || !callback.isFunction()) return;
        if (collision) LuaBoundary::CallDiscard(callback, reference, *collision);
        else LuaBoundary::CallDiscard(callback, reference);
    } catch (const LuaBoundary::Error &error) {
        ReportError(GetActorNameByUID(target.uid), error);
    }
}
// Calls Type.OnUpdateBatch(instances) once with every live, enabled instance
// of targets[first]'s type from `first` on, in update order. Instances are
// read when the batch starts; a Lua error stops the whole batch.
void InvokeBatch(RuntimeState &runtime, std::vector<CallbackTarget> &targets, std::size_t first) {
    lua_State *state = runtime.lua_state;
    const int top = lua_gettop(state);
    struct Restore { lua_State *state; int top; ~Restore() { lua_settop(state, top); } } restore{state, top};
    const std::string type = Alive(runtime, targets[first])->type;
    try {
        if (!lua_checkstack(state, 8)) throw LuaBoundary::Error("Lua batch could not reserve stack space");
        const auto table = runtime.component_type_tables.find(type);
        if (table == runtime.component_type_tables.end()) return;
        table->second.push(state);
        if (!lua_istable(state, -1)) return;
        lua_pushliteral(state, "OnUpdateBatch");
        lua_rawget(state, -2);
        lua_remove(state, -2);
        if (!lua_isfunction(state, -1)) return;
        // Reuse the type's list table: a fresh table per frame is garbage
        // proportional to the instance count.
        auto list_ref = runtime.batch_update_lists.find(type);
        if (list_ref == runtime.batch_update_lists.end()) {
            lua_createtable(state, 0, 0);
            list_ref = runtime.batch_update_lists.emplace(type, luabridge::LuaRef::fromStack(state)).first;
        }
        list_ref->second.push(state);
        const int list = lua_gettop(state);
        const lua_Integer previous = static_cast<lua_Integer>(lua_rawlen(state, list));
        lua_Integer count = 0;
        for (std::size_t index = first; index < targets.size(); ++index) {
            ComponentRecord *component = Alive(runtime, targets[index]);
            if (component == nullptr || component->type != type) continue;
            component->instance_table.push(state);
            bool enabled = true;
            if (PushRawField(state, "enabled")) {
                enabled = lua_type(state, -1) != LUA_TBOOLEAN || lua_toboolean(state, -1) != 0;
                lua_pop(state, 1);
            } else {
                lua_pop(state, 1);
                enabled = LuaBoundary::IsEnabled(ProtectedField(component->instance_table, "enabled"));
                component->instance_table.push(state);
                if (Alive(runtime, targets[index]) == nullptr) { lua_pop(state, 1); continue; }
            }
            if (enabled) lua_rawseti(state, list, ++count);
            else lua_pop(state, 1);
        }
        for (lua_Integer index = previous; index > count; --index) {
            lua_pushnil(state);
            lua_rawseti(state, list, index);
        }
        LuaBoundary::ProtectedCall(state, 1, 0);
    } catch (const LuaBoundary::Error &error) {
        ReportError(type + ".OnUpdateBatch", error);
    }
}

void DispatchContact(Actor::UID uid, const Collision &collision, const char *method,
                     bool ComponentRecord::*has_method) {
    const auto found = Runtime().actor_components.find(uid);
    if (found == Runtime().actor_components.end()) return;
    std::vector<CallbackTarget> targets;
    for (const auto &component : found->second)
        if (!component->removed && component->on_start_called && component.get()->*has_method)
            targets.push_back(Snapshot(uid, *component));
    for (auto &target : targets) Invoke(target, method, &collision);
}
// Physics events still reach actors whose destruction is pending this frame,
// but never a destroyed actor or a later actor reusing the UID.
Actor *FindEventActor(const ActorHandle &handle) {
    const auto found = Runtime().actor_by_uid.find(handle.uid);
    if (found == Runtime().actor_by_uid.end() || found->second == nullptr) return nullptr;
    Actor *actor = found->second;
    return !actor->runtime_destroyed && actor->runtime_generation == handle.generation ? actor : nullptr;
}
void DispatchCollisionEventToActor(const PendingCollisionEvent &event) {
    DispatchContact(event.actor_uid, event.collision, event.is_enter ? "OnCollisionEnter" : "OnCollisionExit",
                    event.is_enter ? &ComponentRecord::has_on_collision_enter : &ComponentRecord::has_on_collision_exit);
}
void DispatchTriggerEventToActor(const PendingTriggerEvent &event) {
    DispatchContact(event.actor_uid, event.collision, event.is_enter ? "OnTriggerEnter" : "OnTriggerExit",
                    event.is_enter ? &ComponentRecord::has_on_trigger_enter : &ComponentRecord::has_on_trigger_exit);
}
void DestroyRemovedComponents(Actor::UID uid) {
    while (true) {
        auto found = Runtime().actor_components.find(uid);
        if (found == Runtime().actor_components.end()) return;
        std::vector<luabridge::LuaRef> removed;
        for (const auto &component : found->second)
            if (component->removed && !component->on_destroy_called) removed.push_back(component->instance_table);
        if (removed.empty()) break;
        for (const auto &reference : removed) {
            // Reacquire after each callback. Only use a record pointer until the
            // invocation starts; no vector iterator is advanced after user code.
            found = Runtime().actor_components.find(uid);
            if (found == Runtime().actor_components.end()) return;
            ComponentRecord *target = nullptr;
            for (const auto &component : found->second)
                if (AreSameLuaRef(component->instance_table, reference)) { target = component.get(); break; }
            if (target && target->removed) RunComponentOnDestroyIfNeeded(*target, uid);
        }
    }
    auto found = Runtime().actor_components.find(uid);
    if (found == Runtime().actor_components.end()) return;
    auto &components = found->second;
    std::erase_if(components, [](const auto &component) { return component->removed && component->on_destroy_called; });
    RebuildComponentIndexForActor(uid);
    RebuildTypeIndexForActor(uid);
}

} // namespace

// -----------------------------------------------------------------------------
// ComponentManager public API: per-frame lifecycle
// -----------------------------------------------------------------------------

void ComponentManager::ApplyEffectiveRigidbodyBodyTypes() {
    // SetType/SetEnabled may dispatch contact-exit callbacks. Keep userdata
    // alive and never retain a container iterator or record across those calls.
    // Scene actor order, not hash-table order, so every platform applies body
    // type changes in the same sequence.
    std::vector<CallbackTarget> targets;
    for (const Actor::UID uid : ActorsWithNative(Runtime(), NativeKind::Rigidbody)) {
        const auto *component = FindRuntimeRigidbodyComponent(uid);
        if (component && !component->removed) targets.push_back(Snapshot(uid, *component));
    }
    for (auto &target : targets) {
        if (BoundEngine() && BoundEngine()->RequiresSceneReset()) return;
        auto *rigidbody_component = Alive(Runtime(), target);
        Rigidbody *rigidbody = nullptr;
        if (!TryCastRuntimeRigidbody(rigidbody_component, rigidbody) ||
            rigidbody == nullptr) {
            continue;
        }

        const PhysicsHierarchy::State physics_state =
            BoundEngine() ? BoundEngine()->GetRuntimePhysicsHierarchyStateByUID(target.uid)
                     : PhysicsHierarchy::State{};
        const std::string effective_body_type =
            physics_state.has_rigidbody_self
                ? physics_state.effective_body_type
                : rigidbody->body_type;
        /*
        Runtime rigidbodies keep both authored and effective body types. This
        bridge copies the hierarchy-derived decision into the live instance
        before OnStart / physics step consume it.
        */
        rigidbody->SetEffectiveBodyType(effective_body_type);
        if (Alive(Runtime(), target) == nullptr) continue;
        rigidbody->ApplyEnabledState();
    }
}

// per-frame lifecycle entry points
void ComponentManager::ProcessPendingOnStart() {
    POCKET_ZONE("Lua OnStart");
    auto pending = std::move(Runtime().pending_on_start);
    Runtime().pending_on_start.clear();
    std::vector<CallbackTarget> targets;
    for (const auto &entry : pending) {
        const auto *component = FindComponentRecord(entry.actor_uid, entry.component_key);
        if (component && !component->on_start_called) targets.push_back(Snapshot(entry.actor_uid, *component));
    }
    for (auto &target : targets) {
        if (BoundEngine() && BoundEngine()->RequiresSceneReset()) return;
        auto *component = Alive(Runtime(), target);
        if (!component || component->on_start_called) continue;
        // Disabled components have not started. Retain their pending entry so
        // later activation observes the same one-time lifecycle boundary.
        try {
            const luabridge::LuaRef reference = component->instance_table;
            const auto enabled = ProtectedField(reference, "enabled");
            component = Alive(Runtime(), target);
            if (!component || component->on_start_called) continue;
            if (!LuaBoundary::IsEnabled(enabled)) {
                Runtime().pending_on_start.emplace_back(target.uid, component->key);
                continue;
            }
            const bool has_callback = component->has_on_start;
            // Mark before calling user code, but after the enabled getter has
            // succeeded. Do not read enabled twice (it may be a Lua getter).
            component->on_start_called = true;
            if (has_callback) {
                const auto callback = ProtectedField(reference, "OnStart");
                if (Alive(Runtime(), target) && callback.isFunction())
                    LuaBoundary::CallDiscard(callback, reference);
            }
        } catch (const LuaBoundary::Error &error) {
            ReportError(GetActorNameByUID(target.uid), error);
            component = Alive(Runtime(), target);
            if (component && !component->on_start_called)
                Runtime().pending_on_start.emplace_back(target.uid, component->key);
        }
    }
}

void ComponentManager::ProcessOnUpdate() {
    POCKET_ZONE("Lua OnUpdate");
    RuntimeState &runtime = Runtime();
    std::vector<CallbackTarget> targets;
    targets.reserve(runtime.on_update_components.size());
    for (const auto &entry : runtime.on_update_components) {
        auto *component = entry.component;
        if (!component || component->removed) continue;
        if (component->on_start_called) targets.push_back(Snapshot(entry.actor_uid, *component));
    }
    Engine *engine = BoundEngine();
    // Types whose table defines OnUpdateBatch(instances) are updated by one
    // call at their first instance's place in the update order.
    std::unordered_map<std::string, bool> batched;
    std::unordered_set<std::string> batch_done;
    const auto is_batched = [&](const ComponentRecord &component) {
        if (IsBuiltinComponentType(component.type)) return false;
        const auto [found, inserted] = batched.try_emplace(component.type, false);
        if (inserted) {
            const auto type = runtime.component_type_tables.find(component.type);
            if (type != runtime.component_type_tables.end()) {
                lua_State *state = runtime.lua_state;
                const int top = lua_gettop(state);
                type->second.push(state);
                // Raw: no user metamethod runs outside a protected call.
                found->second = lua_istable(state, -1) &&
                                (lua_pushliteral(state, "OnUpdateBatch"), lua_rawget(state, -2)) == LUA_TFUNCTION;
                lua_settop(state, top);
            }
        }
        return found->second;
    };
    for (std::size_t index = 0; index < targets.size(); ++index) {
        auto &target = targets[index];
        if (engine && engine->RequiresSceneReset()) return;
        auto *component = Alive(runtime, target);
        if (!component) continue;
        // Preserve original per-component ordering: earlier Lua updates can
        // change this emitter's enabled state during the same tick.
        SyncBuiltinParticleSystemState(*component);
        if (is_batched(*component)) {
            if (batch_done.insert(component->type).second) InvokeBatch(runtime, targets, index);
            continue;
        }
        Invoke(target, "OnUpdate");
    }
}

void ComponentManager::ProcessOnLateUpdate() {
    POCKET_ZONE("Lua OnLateUpdate");
    std::vector<CallbackTarget> targets;
    targets.reserve(Runtime().on_late_update_components.size());
    for (const auto &entry : Runtime().on_late_update_components)
        if (entry.component && !entry.component->removed && entry.component->on_start_called)
            targets.push_back(Snapshot(entry.actor_uid, *entry.component));
    for (auto &target : targets) Invoke(target, "OnLateUpdate");
}

// per-frame lifecycle entry points
void ComponentManager::StepPhysics() {
    POCKET_ZONE("StepPhysics");
    // Subscribe/Unsubscribe become visible at the very end of the frame,
    // after LateUpdate and before the physics step starts.
    EventBus::ApplyPendingOperations();
    // Startup and Tilemap commit create worlds before this boundary. With no
    // world there is no native state to synchronize or step.
    if (!Rigidbody::HasPhysicsWorld()) return;

    // OnStart/Update/LateUpdate may have changed enabled or body_type after
    // the frame-start bridge. Apply those requests before this actual step.
    ApplyEffectiveRigidbodyBodyTypes();
    FinalizePrePhysicsDestructions();
    if (BoundEngine() && BoundEngine()->RequiresSceneReset()) return;

    if (!Rigidbody::HasPhysicsWorld()) return;

    if (BoundEngine() != nullptr) {
        ComponentManager::ResolveTransformHierarchy();
        for (const auto &actor_components_entry : Runtime().actor_components) {
            const Actor::UID actor_uid = actor_components_entry.first;
            ComponentRecord *rigidbody_component =
                FindRuntimeRigidbodyComponent(actor_uid);
            Rigidbody *rigidbody = nullptr;
            if (!TryCastRuntimeRigidbody(rigidbody_component, rigidbody) ||
                rigidbody == nullptr) {
                continue;
            }

            const PhysicsHierarchy::State physics_state =
                BoundEngine()->GetRuntimePhysicsHierarchyStateByUID(actor_uid);
            if (!physics_state.has_rigidbody_self ||
                physics_state.effective_body_type == "dynamic") {
                continue;
            }

            ComponentRecord *transform_component =
                FindRuntimeTransformComponent(actor_uid);
            Transform *transform = nullptr;
            if (!TryCastRuntimeTransform(transform_component, transform) ||
                transform == nullptr) {
                continue;
            }

            /*
            Non-dynamic rigidbodies follow hierarchy/world-transform state into
            Box2D, rather than competing with physics as a second motion owner.
            */
            rigidbody->SetPosition(Vector2(transform->world_x,
                                          transform->world_y));
            rigidbody->SetRotation(transform->world_rotation);
        }
    }

    Rigidbody::StepPhysicsWorld();

    for (const auto &actor_components_entry : Runtime().actor_components) {
        const Actor::UID actor_uid = actor_components_entry.first;
        ComponentRecord *rigidbody_component =
            FindRuntimeRigidbodyComponent(actor_uid);
        Rigidbody *rigidbody = nullptr;
        if (!TryCastRuntimeRigidbody(rigidbody_component, rigidbody) ||
            rigidbody == nullptr) {
            continue;
        }

        ComponentRecord *transform_component =
            FindRuntimeTransformComponent(actor_uid);
        Transform *transform = nullptr;
        if (!TryCastRuntimeTransform(transform_component, transform) ||
            transform == nullptr) {
            continue;
        }

        const PhysicsHierarchy::State physics_state =
            (BoundEngine() != nullptr)
                ? BoundEngine()->GetRuntimePhysicsHierarchyStateByUID(actor_uid)
                : PhysicsHierarchy::State{};
        if (!physics_state.has_rigidbody_self ||
            physics_state.effective_body_type != "dynamic") {
            continue;
        }

        const Vector2 position = rigidbody->GetPosition();
        const float rotation = rigidbody->GetRotation();
        // Keep the Lua-facing Rigidbody fields in sync with the Box2D body so
        // inspector/runtime reads reflect physics-driven motion immediately.
        rigidbody_component->instance_table["x"] = position.x;
        rigidbody_component->instance_table["y"] = position.y;
        rigidbody_component->instance_table["rotation"] = rotation;
        Actor *actor = FindRuntimeActor(actor_uid);
        float local_x = position.x;
        float local_y = position.y;
        float local_rotation = rotation;
        /*
        Dynamic roots run in the opposite direction: physics owns the world
        pose, so we convert that world pose back into local Transform under the
        current parent chain after the Box2D step.
        */
        ResolveLocalTransformFromWorld(actor, position.x, position.y, rotation,
                                       local_x, local_y, local_rotation);
        transform_component->instance_table["x"] = local_x;
        transform_component->instance_table["y"] = local_y;
        transform_component->instance_table["rotation"] = local_rotation;
    }
}

// per-frame lifecycle entry points
void ComponentManager::FinalizePrePhysicsDestructions() {
    while (true) {
        std::unordered_set<Actor::UID> dirty_actor_uids =
            Runtime().pending_destroy_actor_uids;
        for (Actor::UID actor_uid : Runtime().dirty_component_actor_uids) {
            if (dirty_actor_uids.find(actor_uid) != dirty_actor_uids.end()) continue;

            auto actor_it = Runtime().actor_components.find(actor_uid);
            if (actor_it == Runtime().actor_components.end()) continue;

            bool has_removed_components = false;
            for (const std::unique_ptr<ComponentRecord> &component_ptr :
                 actor_it->second) {
                if (!component_ptr->removed) continue;
                has_removed_components = true;
                break;
            }
            if (has_removed_components) {
                dirty_actor_uids.insert(actor_uid);
            }
        }
        const std::unordered_set<Actor::UID> destroyed_actor_uids =
            Runtime().pending_destroy_actor_uids;

        if (dirty_actor_uids.empty() && destroyed_actor_uids.empty()) {
            return;
        }

        // Consume existing marks before user code; new marks from OnDestroy
        // must survive even when they concern an actor processed earlier.
        for (Actor::UID actor_uid : dirty_actor_uids) Runtime().dirty_component_actor_uids.erase(actor_uid);
        for (Actor::UID actor_uid : dirty_actor_uids) DestroyRemovedComponents(actor_uid);

        RebuildLifecycleListsForDirtyActors(dirty_actor_uids);

        if (!destroyed_actor_uids.empty()) {
            Runtime().pending_on_start.erase(
                std::remove_if(
                    Runtime().pending_on_start.begin(),
                    Runtime().pending_on_start.end(),
                    [&](const PendingOnStartRecord &pending) {
                        return destroyed_actor_uids.find(pending.actor_uid) !=
                               destroyed_actor_uids.end();
                    }),
                Runtime().pending_on_start.end());

            Runtime().pending_actor_uids_to_activate.erase(
                std::remove_if(
                    Runtime().pending_actor_uids_to_activate.begin(),
                    Runtime().pending_actor_uids_to_activate.end(),
                    [&](Actor::UID actor_uid) {
                        return destroyed_actor_uids.find(actor_uid) !=
                               destroyed_actor_uids.end();
                    }),
                Runtime().pending_actor_uids_to_activate.end());
        }

        for (Actor::UID actor_uid : destroyed_actor_uids) {
            auto actor_ptr_it = Runtime().actor_by_uid.find(actor_uid);
            Actor *actor_ptr =
                (actor_ptr_it == Runtime().actor_by_uid.end()) ? nullptr
                                                              : actor_ptr_it->second;
            if (actor_ptr != nullptr) {
                actor_ptr->runtime_destroyed = true;
            }
            Runtime().actor_by_uid.erase(actor_uid);
            Runtime().actor_components.erase(actor_uid);
            Runtime().component_index_by_key.erase(actor_uid);
            Runtime().component_first_key_by_type.erase(actor_uid);
            Runtime().component_keys_by_type.erase(actor_uid);
            Runtime().actor_order_by_uid.erase(actor_uid);
            Runtime().actor_uids_sorted.erase(
                std::remove(Runtime().actor_uids_sorted.begin(),
                            Runtime().actor_uids_sorted.end(), actor_uid),
                Runtime().actor_uids_sorted.end());
            ++Runtime().actor_order_version;
        }

        for (Actor::UID actor_uid : destroyed_actor_uids) {
            Runtime().pending_destroy_actor_uids.erase(actor_uid);
        }
    }
}

// immediate physics event dispatch helpers
void ComponentManager::QueueCollisionEvent(const ActorHandle &actor, const ActorHandle &other,
                                     const Vector2 &point,
                                     const Vector2 &relative_velocity,
                                     const Vector2 &normal, bool is_enter) {
    Actor *receiver = FindEventActor(actor);
    if (receiver == nullptr) return;

    Collision collision;
    collision.other = FindEventActor(other);
    collision.other_handle = other;
    collision.point = point;
    collision.relative_velocity = relative_velocity;
    collision.normal = normal;
    DispatchCollisionEventToActor(
        PendingCollisionEvent(receiver->uid, is_enter, collision));
}

// immediate physics event dispatch helpers
void ComponentManager::QueueTriggerEvent(const ActorHandle &actor, const ActorHandle &other,
                                     const Vector2 &point,
                                     const Vector2 &relative_velocity,
                                     const Vector2 &normal, bool is_enter) {
    Actor *receiver = FindEventActor(actor);
    if (receiver == nullptr) return;

    Collision collision;
    collision.other = FindEventActor(other);
    collision.other_handle = other;
    collision.point = point;
    collision.relative_velocity = relative_velocity;
    collision.normal = normal;
    DispatchTriggerEventToActor(
        PendingTriggerEvent(receiver->uid, is_enter, collision));
}

// per-frame lifecycle entry points
void ComponentManager::FinalizeFrameMutations() {
    POCKET_ZONE("FinalizeFrameMutations");
    // 帧末统一收口 runtime mutation:
    // 1) 激活本帧新建 actor (下一帧开始参与生命周期)
    // 2) 清理 removed 组件
    // 3) 处理 Actor.Destroy 的最终移除
    if (Runtime().pending_actor_uids_to_activate.empty() &&
        Runtime().dirty_component_actor_uids.empty() &&
        Runtime().pending_destroy_actor_uids.empty()) {
        return;
    }

    std::unordered_set<Actor::UID> dirty_actor_uids =
        std::move(Runtime().dirty_component_actor_uids);
    Runtime().dirty_component_actor_uids.clear();

    for (Actor::UID actor_uid : Runtime().pending_actor_uids_to_activate) {
        if (Runtime().pending_destroy_actor_uids.find(actor_uid) !=
            Runtime().pending_destroy_actor_uids.end()) {
            continue;
        }
        if (Runtime().actor_order_by_uid.find(actor_uid) ==
            Runtime().actor_order_by_uid.end()) {
            Runtime().actor_order_by_uid[actor_uid] = Runtime().next_actor_order++;
        }
        auto actor_ptr_it = Runtime().actor_by_uid.find(actor_uid);
        if (actor_ptr_it == Runtime().actor_by_uid.end() ||
            actor_ptr_it->second == nullptr) {
            continue;
        }
        // Normally append; an immediate host duplicate can already follow this
        // delayed actor in publication order. Keep animation/physics traversal
        // consistent with the lifecycle caches in that mixed case.
        auto &active = Runtime().actor_uids_sorted;
        const auto before = [](Actor::UID a, Actor::UID b) {
            return Runtime().actor_order_by_uid.at(a) < Runtime().actor_order_by_uid.at(b);
        };
        if (active.empty() || !before(actor_uid, active.back())) active.push_back(actor_uid);
        else active.insert(std::lower_bound(active.begin(), active.end(), actor_uid, before), actor_uid);
        ++Runtime().actor_order_version;

        // Newly instantiated actors have no existing lifecycle entries yet.
        // Append them directly instead of forcing a full dirty-list rebuild/sort.
        auto actor_components_it = Runtime().actor_components.find(actor_uid);
        if (actor_components_it != Runtime().actor_components.end()) {
            for (std::unique_ptr<ComponentRecord> &component_ptr :
                 actor_components_it->second) {
                ComponentRecord &component = *component_ptr;
                if (component.removed) continue;
                if (component.has_on_update) {
                    AppendOrderedLifecycle(Runtime().on_update_components, actor_uid, &component);
                }
                if (component.has_on_late_update) {
                    AppendOrderedLifecycle(Runtime().on_late_update_components, actor_uid, &component);
                }
            }
        }

        dirty_actor_uids.erase(actor_uid);
    }
    Runtime().pending_actor_uids_to_activate.clear();

    // Snapshot-based teardown permits OnDestroy to add/remove components.
    for (Actor::UID actor_uid : dirty_actor_uids) DestroyRemovedComponents(actor_uid);
    RebuildLifecycleListsForDirtyActors(dirty_actor_uids);

    // Destruction callbacks may have removed components on another actor.
    // Reuse the same snapshot-safe drain instead of erasing new pending actors
    // without their OnDestroy or leaving cached lifecycle pointers dangling.
    FinalizePrePhysicsDestructions();
}
