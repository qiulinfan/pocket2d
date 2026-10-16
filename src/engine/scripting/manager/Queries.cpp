#include "scripting/LuaBoundary.h"
#include "Internal.h"
#include "scene/Tilemap.h"
#include "rendering/SpriteAnimation.h"
#include "rendering/SpriteRenderer.h"
#include "scripting/ComponentManager.h"
#include "particles/ParticleSystem.h"
#include "physics/Rigidbody.h"
#include "scene/Transform.h"
#include "scene/ActorHandle.h"
#include <algorithm>
#include <unordered_set>

using namespace ManagerDetail;

namespace {

luabridge::LuaRef BuildChildrenResult(Actor::UID actor_uid) {
    if (actor_uid == Actor::kInvalidUID) return MakeEmptyArrayTable();

    luabridge::LuaRef result_table = MakeEmptyArrayTable();
    int lua_index = 1;
    for (Actor::UID candidate_uid : Runtime().actor_uids_sorted) {
        auto actor_it = Runtime().actor_by_uid.find(candidate_uid);
        if (actor_it == Runtime().actor_by_uid.end() ||
            actor_it->second == nullptr) {
            continue;
        }

        const Actor *candidate_actor = actor_it->second;
        if (candidate_actor->parent_uid != actor_uid) continue;
        result_table[lua_index++] =
            luabridge::LuaRef(Runtime().lua_state, ActorHandle(actor_it->second));
    }

    return result_table;
}

// Memoize ancestry only for this query. Each parent edge is walked at most
// once, while first-match queries can still stop without indexing the scene.
// Public parent fields and pending actors are observed immediately.
class DescendantQuery {
public:
    explicit DescendantQuery(Actor::UID root_uid) {
        states_.emplace(root_uid, State::Descendant);
    }

    bool Contains(Actor::UID uid) {
        bool descendant = false;
        while (uid != Actor::kInvalidUID) {
            const auto inserted = states_.emplace(uid, State::Visiting);
            if (!inserted.second) {
                descendant = inserted.first->second == State::Descendant;
                break;
            }
            path_.push_back(uid);
            const auto actor = Runtime().actor_by_uid.find(uid);
            if (actor == Runtime().actor_by_uid.end() || actor->second == nullptr) break;
            uid = actor->second->parent_uid;
        }
        // Encountering Visiting means a cycle not containing the root. Every
        // path ending in that cycle is outside the requested subtree.
        for (Actor::UID visited : path_) {
            states_[visited] = descendant ? State::Descendant : State::Outside;
        }
        path_.clear();
        return descendant;
    }

private:
    enum class State { Visiting, Outside, Descendant };
    std::unordered_map<Actor::UID, State> states_;
    std::vector<Actor::UID> path_;
};

luabridge::LuaRef BuildComponentsInChildrenResult(Actor::UID actor_uid,
                                                  const std::string &type_name,
                                                  bool first_only) {
    if (actor_uid == Actor::kInvalidUID || type_name.empty()) {
        return first_only ? MakeNilRef() : MakeEmptyArrayTable();
    }

    DescendantQuery descendants(actor_uid);
    luabridge::LuaRef result_table = first_only ? MakeNilRef() : MakeEmptyArrayTable();
    int lua_index = 1;

    // Traversal order is deliberately independent of ancestry resolution:
    // preserve scene order even when children precede their parents in a scene.
    for (Actor::UID candidate_uid : Runtime().actor_uids_sorted) {
        if (!descendants.Contains(candidate_uid)) continue;
        const auto actor_types = Runtime().component_keys_by_type.find(candidate_uid);
        if (actor_types == Runtime().component_keys_by_type.end()) continue;
        const auto keys = actor_types->second.find(type_name);
        if (keys == actor_types->second.end()) continue;
        for (const std::string &key : keys->second) {
            ComponentRecord *component = FindComponentRecord(candidate_uid, key);
            if (component == nullptr) continue;
            if (first_only) return component->instance_table;
            result_table[lua_index++] = component->instance_table;
        }
    }
    return result_table;
}

enum class LuaArrayType {
    Bool,
    Int,
    Double,
    String,
    Unknown
};

LuaArrayType ExpectedArrayType(const Actor::ComponentPropertyValue *expected_value) {
    if (expected_value == nullptr) return LuaArrayType::Unknown;
    if (std::holds_alternative<Actor::BoolArray>(*expected_value)) {
        return LuaArrayType::Bool;
    }
    if (std::holds_alternative<Actor::IntArray>(*expected_value)) {
        return LuaArrayType::Int;
    }
    if (std::holds_alternative<Actor::DoubleArray>(*expected_value)) {
        return LuaArrayType::Double;
    }
    if (std::holds_alternative<Actor::StringArray>(*expected_value)) {
        return LuaArrayType::String;
    }
    return LuaArrayType::Unknown;
}

LuaArrayType InferArrayTypeFromFirstLuaElement(int table_index) {
    lua_rawgeti(Runtime().lua_state, table_index, 1);
    LuaArrayType array_type = LuaArrayType::Unknown;
    const int value_type = lua_type(Runtime().lua_state, -1);
    if (value_type == LUA_TBOOLEAN) {
        array_type = LuaArrayType::Bool;
    } else if (lua_isinteger(Runtime().lua_state, -1)) {
        array_type = LuaArrayType::Int;
    } else if (lua_isnumber(Runtime().lua_state, -1)) {
        array_type = LuaArrayType::Double;
    } else if (value_type == LUA_TSTRING) {
        array_type = LuaArrayType::String;
    }
    lua_pop(Runtime().lua_state, 1);
    return array_type;
}

bool TryReadArrayValueFromLuaStack(int stack_index,
                                   const Actor::ComponentPropertyValue *expected_value,
                                   Actor::ComponentPropertyValue &out_value) {
    const int table_index = lua_absindex(Runtime().lua_state, stack_index);
    if (!lua_istable(Runtime().lua_state, table_index)) return false;

    const int array_length =
        static_cast<int>(lua_rawlen(Runtime().lua_state, table_index));
    LuaArrayType array_type = ExpectedArrayType(expected_value);
    if (array_type == LuaArrayType::Unknown) {
        if (array_length <= 0) return false;
        array_type = InferArrayTypeFromFirstLuaElement(table_index);
    }
    if (array_type == LuaArrayType::Unknown) return false;

    switch (array_type) {
    case LuaArrayType::Bool: {
        Actor::BoolArray values;
        values.reserve(array_length);
        for (int index = 1; index <= array_length; ++index) {
            lua_rawgeti(Runtime().lua_state, table_index, index);
            if (lua_type(Runtime().lua_state, -1) != LUA_TBOOLEAN) {
                lua_pop(Runtime().lua_state, 1);
                return false;
            }
            values.emplace_back(lua_toboolean(Runtime().lua_state, -1) != 0);
            lua_pop(Runtime().lua_state, 1);
        }
        out_value = std::move(values);
        return true;
    }
    case LuaArrayType::Int: {
        Actor::IntArray values;
        values.reserve(array_length);
        for (int index = 1; index <= array_length; ++index) {
            lua_rawgeti(Runtime().lua_state, table_index, index);
            if (!lua_isinteger(Runtime().lua_state, -1)) {
                lua_pop(Runtime().lua_state, 1);
                return false;
            }
            values.emplace_back(
                static_cast<int>(lua_tointeger(Runtime().lua_state, -1)));
            lua_pop(Runtime().lua_state, 1);
        }
        out_value = std::move(values);
        return true;
    }
    case LuaArrayType::Double: {
        Actor::DoubleArray values;
        values.reserve(array_length);
        for (int index = 1; index <= array_length; ++index) {
            lua_rawgeti(Runtime().lua_state, table_index, index);
            if (!lua_isnumber(Runtime().lua_state, -1)) {
                lua_pop(Runtime().lua_state, 1);
                return false;
            }
            values.emplace_back(lua_tonumber(Runtime().lua_state, -1));
            lua_pop(Runtime().lua_state, 1);
        }
        out_value = std::move(values);
        return true;
    }
    case LuaArrayType::String: {
        Actor::StringArray values;
        values.reserve(array_length);
        for (int index = 1; index <= array_length; ++index) {
            lua_rawgeti(Runtime().lua_state, table_index, index);
            if (lua_type(Runtime().lua_state, -1) != LUA_TSTRING) {
                lua_pop(Runtime().lua_state, 1);
                return false;
            }
            values.emplace_back(lua_tostring(Runtime().lua_state, -1));
            lua_pop(Runtime().lua_state, 1);
        }
        out_value = std::move(values);
        return true;
    }
    case LuaArrayType::Unknown:
    default:
        return false;
    }
}

bool TryReadPropertyValueFromLuaStack(
    int stack_index, Actor::ComponentPropertyValue &out_value,
    const Actor::ComponentPropertyValue *expected_value = nullptr) {
    const int value_type = lua_type(Runtime().lua_state, stack_index);
    switch (value_type) {
    case LUA_TBOOLEAN:
        out_value = lua_toboolean(Runtime().lua_state, stack_index) != 0;
        return true;
    case LUA_TNUMBER:
        if (lua_isinteger(Runtime().lua_state, stack_index)) {
            out_value = static_cast<int>(
                lua_tointeger(Runtime().lua_state, stack_index));
        } else {
            out_value = lua_tonumber(Runtime().lua_state, stack_index);
        }
        return true;
    case LUA_TSTRING:
        out_value = std::string(lua_tostring(Runtime().lua_state, stack_index));
        return true;
    case LUA_TTABLE:
        return TryReadArrayValueFromLuaStack(stack_index, expected_value,
                                             out_value);
    default:
        return false;
    }
}

bool TryReadPropertyValueFromLuaRef(
    const luabridge::LuaRef &value_ref, Actor::ComponentPropertyValue &out_value,
    const Actor::ComponentPropertyValue *expected_value = nullptr) {
    if (Runtime().lua_state == nullptr || value_ref.isNil()) return false;

    value_ref.push(Runtime().lua_state);
    const bool converted = TryReadPropertyValueFromLuaStack(
        -1, out_value, expected_value);
    lua_pop(Runtime().lua_state, 1);
    return converted;
}

std::vector<Actor::ComponentProperty> GetBuiltinComponentDefaultProperties(const std::string &type_name) {
    
    std::vector<Actor::ComponentProperty> properties;
    if (type_name == "SpriteAnimation") {
        return {{"enabled", true}, {"animation_resource", std::string{}},
                {"renderer_key", std::string{"sprite"}}, {"clip", std::string{}}};
    }
    if (type_name == "Tilemap") {
        return {{"enabled", true}, {"map_resource", std::string{}}};
    }
    if (type_name == "Transform") {
        const Transform defaults;
        properties = {
            {"enabled", defaults.enabled},
            {"x", static_cast<double>(defaults.x)},
            {"y", static_cast<double>(defaults.y)},
            {"rotation", static_cast<double>(defaults.rotation)},
        };
        return properties;
    }

    if (type_name == "SpriteRenderer") {
        const SpriteRenderer defaults;
        properties = {
            {"enabled", defaults.enabled},
            {"sprite", defaults.sprite},
            {"r", defaults.r},
            {"g", defaults.g},
            {"b", defaults.b},
            {"a", defaults.a},
            {"pivot_x", static_cast<double>(defaults.pivot_x)},
            {"pivot_y", static_cast<double>(defaults.pivot_y)},
            {"scale_x", static_cast<double>(defaults.scale_x)},
            {"scale_y", static_cast<double>(defaults.scale_y)},
            {"sprite_row", defaults.sprite_row},
            {"sprite_column", defaults.sprite_column},
            {"sorting_order", defaults.sorting_order},
            {"auto_sorting_order", defaults.auto_sorting_order},
        };
        return properties;
    }

    if (type_name == "Rigidbody") {
        const Rigidbody defaults;
        properties = {
            {"enabled", defaults.enabled},
            {"x", static_cast<double>(defaults.x)},
            {"y", static_cast<double>(defaults.y)},
            {"body_type", defaults.body_type},
            {"precise", defaults.precise},
            {"gravity_scale", static_cast<double>(defaults.gravity_scale)},
            {"density", static_cast<double>(defaults.density)},
            {"angular_friction",
             static_cast<double>(defaults.angular_friction)},
            {"linear_friction", static_cast<double>(defaults.linear_friction)},
            {"rotation", static_cast<double>(defaults.rotation)},
            {"has_collider", defaults.has_collider},
            {"has_trigger", defaults.has_trigger},
            {"collider_type", defaults.collider_type},
            {"width", static_cast<double>(defaults.width)},
            {"height", static_cast<double>(defaults.height)},
            {"radius", static_cast<double>(defaults.radius)},
            {"trigger_type", defaults.trigger_type},
            {"trigger_width", static_cast<double>(defaults.trigger_width)},
            {"trigger_height", static_cast<double>(defaults.trigger_height)},
            {"trigger_radius", static_cast<double>(defaults.trigger_radius)},
            {"friction", static_cast<double>(defaults.friction)},
            {"bounciness", static_cast<double>(defaults.bounciness)},
            {"collision_layer", defaults.collision_layer},
            {"collision_mask", defaults.collision_mask},
            {"collider_points", Actor::DoubleArray{}},
        };
        return properties;
    }

    if (type_name == "ParticleSystem") {
        const ParticleSystem defaults;
        properties = {
            {"enabled", defaults.enabled},
            {"x", static_cast<double>(defaults.x)},
            {"y", static_cast<double>(defaults.y)},
            {"frames_between_bursts", defaults.frames_between_bursts},
            {"burst_quantity", defaults.burst_quantity},
            {"duration_frames", defaults.duration_frames},
            {"start_scale_min", static_cast<double>(defaults.start_scale_min)},
            {"start_scale_max", static_cast<double>(defaults.start_scale_max)},
            {"start_speed_min", static_cast<double>(defaults.start_speed_min)},
            {"start_speed_max", static_cast<double>(defaults.start_speed_max)},
            {"rotation_min", static_cast<double>(defaults.rotation_min)},
            {"rotation_max", static_cast<double>(defaults.rotation_max)},
            {"rotation_speed_min",
             static_cast<double>(defaults.rotation_speed_min)},
            {"rotation_speed_max",
             static_cast<double>(defaults.rotation_speed_max)},
            {"start_color_r", defaults.start_color_r},
            {"start_color_g", defaults.start_color_g},
            {"start_color_b", defaults.start_color_b},
            {"start_color_a", defaults.start_color_a},
            {"end_color_r", defaults.end_color_r},
            {"end_color_g", defaults.end_color_g},
            {"end_color_b", defaults.end_color_b},
            {"end_color_a", defaults.end_color_a},
            {"image", defaults.image},
            {"sorting_order", defaults.sorting_order},
            {"emit_angle_min", static_cast<double>(defaults.emit_angle_min)},
            {"emit_angle_max", static_cast<double>(defaults.emit_angle_max)},
            {"emit_radius_min", static_cast<double>(defaults.emit_radius_min)},
            {"emit_radius_max", static_cast<double>(defaults.emit_radius_max)},
            {"gravity_scale_x",
             static_cast<double>(defaults.gravity_scale_x)},
            {"gravity_scale_y",
             static_cast<double>(defaults.gravity_scale_y)},
            {"drag_factor", static_cast<double>(defaults.drag_factor)},
            {"angular_drag_factor",
             static_cast<double>(defaults.angular_drag_factor)},
            {"end_scale", static_cast<double>(defaults.end_scale)},
        };
        return properties;
    }

    return properties;
}

void MergeLiveRuntimePropertyValues(
    const luabridge::LuaRef &reference, Actor::UID uid, const std::string &key,
    std::vector<Actor::ComponentProperty> &properties) {
    for (Actor::ComponentProperty &property : properties) {
        Actor::ComponentPropertyValue live_value;
        const auto field = ProtectedField(reference, property.name.c_str());
        if (!IsComponentRefAlive(reference, uid, key))
            throw std::runtime_error("component removed during property lookup");
        if (!TryReadPropertyValueFromLuaRef(field, live_value, &property.value)) {
            continue;
        }
        property.value = live_value;
    }
}

void MergeDynamicLuaTableProperties(
    const luabridge::LuaRef &reference,
    std::vector<Actor::ComponentProperty> &properties) {
    if (Runtime().lua_state == nullptr) return;

    std::unordered_set<std::string> known_names;
    known_names.reserve(properties.size());
    for (const Actor::ComponentProperty &property : properties) {
        known_names.insert(property.name);
    }

    reference.push(Runtime().lua_state);
    if (!lua_istable(Runtime().lua_state, -1)) {
        lua_pop(Runtime().lua_state, 1);
        return;
    }

    lua_pushnil(Runtime().lua_state);
    while (lua_next(Runtime().lua_state, -2) != 0) {
        if (lua_type(Runtime().lua_state, -2) != LUA_TSTRING) {
            lua_pop(Runtime().lua_state, 1);
            continue;
        }

        const std::string property_name = lua_tostring(Runtime().lua_state, -2);
        if (property_name == "actor" || property_name == "key") {
            lua_pop(Runtime().lua_state, 1);
            continue;
        }
        if (known_names.find(property_name) != known_names.end()) {
            lua_pop(Runtime().lua_state, 1);
            continue;
        }

        Actor::ComponentProperty property;
        property.name = property_name;
        if (!TryReadPropertyValueFromLuaStack(-1, property.value)) {
            lua_pop(Runtime().lua_state, 1);
            continue;
        }

        properties.emplace_back(std::move(property));
        known_names.insert(property_name);
        lua_pop(Runtime().lua_state, 1);
    }

    lua_pop(Runtime().lua_state, 1);
}

} // namespace

// -----------------------------------------------------------------------------
// ComponentManager public API: queries
// -----------------------------------------------------------------------------

// component queries used by Actor's Lua-facing methods
luabridge::LuaRef ComponentManager::GetComponentByKey(Actor::UID actor_uid,
                                                      const std::string &key) {
    // removed 组件会被 FindComponentRecord 过滤, 查询结果为 nil
    ComponentRecord *component = FindComponentRecord(actor_uid, key);
    if (component == nullptr) return MakeNilRef();
    return component->instance_table;
}

// component queries used by Actor's Lua-facing methods
luabridge::LuaRef ComponentManager::GetComponentByType(Actor::UID actor_uid,
                                                       const std::string &type_name) {
    auto actor_it = Runtime().component_first_key_by_type.find(actor_uid);
    if (actor_it == Runtime().component_first_key_by_type.end()) return MakeNilRef();

    auto type_it = actor_it->second.find(type_name);
    if (type_it == actor_it->second.end()) return MakeNilRef();

    ComponentRecord *component = FindComponentRecord(actor_uid, type_it->second);
    if (component == nullptr) return MakeNilRef();
    return component->instance_table;
}

// component queries used by Actor's Lua-facing methods
luabridge::LuaRef ComponentManager::GetComponentsByType(Actor::UID actor_uid,
                                                        const std::string &type_name) {
    auto actor_it = Runtime().component_keys_by_type.find(actor_uid);
    if (actor_it == Runtime().component_keys_by_type.end()) {
        return MakeEmptyArrayTable();
    }

    auto type_it = actor_it->second.find(type_name);
    if (type_it == actor_it->second.end()) return MakeEmptyArrayTable();

    luabridge::LuaRef result_table = MakeEmptyArrayTable();
    int lua_index = 1;
    for (const std::string &component_key : type_it->second) {
        ComponentRecord *component = FindComponentRecord(actor_uid,
                                                         component_key);
        if (component == nullptr) continue;
        // Lua arrays are 1-indexed for ipairs().
        result_table[lua_index++] = component->instance_table;
    }
    return result_table;
}

int ComponentManager::GetChildCount(Actor::UID actor_uid) {
    if (actor_uid == Actor::kInvalidUID) return 0;

    int child_count = 0;
    for (Actor::UID candidate_uid : Runtime().actor_uids_sorted) {
        auto actor_it = Runtime().actor_by_uid.find(candidate_uid);
        if (actor_it == Runtime().actor_by_uid.end() ||
            actor_it->second == nullptr) {
            continue;
        }

        if (actor_it->second->parent_uid == actor_uid) {
            ++child_count;
        }
    }

    return child_count;
}

luabridge::LuaRef ComponentManager::GetChildren(Actor::UID actor_uid) {
    /*
    This is the direct-child query that pairs with GetChildCount. It does not
    recurse; callers that want descendants should use GetChildren recursively
    or the GetComponent(s)InChildren helpers above.
    */
    return BuildChildrenResult(actor_uid);
}

luabridge::LuaRef ComponentManager::GetComponentInChildrenByType(
    Actor::UID actor_uid, const std::string &type_name) {
    /*
    Match the familiar Unity-style query shape: inspect this actor first, then
    walk descendants in current runtime scene order until the first component
    of the requested type is found.
    */
    return BuildComponentsInChildrenResult(actor_uid, type_name, true);
}

luabridge::LuaRef ComponentManager::GetComponentsInChildrenByType(
    Actor::UID actor_uid, const std::string &type_name) {
    /*
    Child queries memoize ancestry within one query and emit matching
    components in runtime scene order.
    */
    return BuildComponentsInChildrenResult(actor_uid, type_name, false);
}

// actor-level helpers exposed through Actor static APIs
luabridge::LuaRef ComponentManager::FindActorByName(const std::string &name) {
    // 按维护的名字索引返回首个匹配 actor
    auto it = Runtime().actors_by_name.find(name);
    if (it == Runtime().actors_by_name.end() || it->second.empty()) {
        return MakeNilRef();
    }
    return luabridge::LuaRef(Runtime().lua_state, ActorHandle(it->second[0]));
}

// actor-level helpers exposed through Actor static APIs
luabridge::LuaRef ComponentManager::FindAllActorsByName( const std::string &name) {
    // 返回 Lua 数组(Note: 1-based), 便于 Lua 端 ipairs()
    auto it = Runtime().actors_by_name.find(name);
    luabridge::LuaRef result_table = MakeEmptyArrayTable();
    if (it == Runtime().actors_by_name.end()) return result_table;

    const std::vector<Actor *> &matched_actors = it->second;
    for (size_t i = 0; i < matched_actors.size(); i++) {
        result_table[static_cast<int>(i + 1)] = luabridge::LuaRef(Runtime().lua_state, ActorHandle(matched_actors[i]));
    }
    return result_table;
}

// editor-facing metadata queries for available component types/defaults
std::vector<std::string> ComponentManager::GetRegisteredComponentTypes() {
    std::vector<std::string> type_names;
    type_names.reserve(Runtime().component_type_tables.size() + 4);
    type_names.emplace_back("SpriteRenderer");
    type_names.emplace_back("ParticleSystem");
    type_names.emplace_back("Rigidbody");
    type_names.emplace_back("Transform");
    type_names.emplace_back("Tilemap");
    type_names.emplace_back("SpriteAnimation");

    for (const auto &entry : Runtime().component_type_tables) {
        type_names.emplace_back(entry.first);
    }

    std::sort(type_names.begin(), type_names.end());
    type_names.erase(std::unique(type_names.begin(), type_names.end()),
                     type_names.end());
    return type_names;
}

bool ComponentManager::IsRegisteredComponentType(const std::string &type_name) {
    if (IsBuiltinComponentType(type_name)) return true;
    return Runtime().component_type_tables.find(type_name) !=
           Runtime().component_type_tables.end();
}

// editor-facing metadata queries for available component types/defaults
std::vector<Actor::ComponentProperty>
ComponentManager::GetComponentTypeDefaultProperties( const std::string &type_name) {
    std::vector<Actor::ComponentProperty> properties;
    if (IsBuiltinComponentType(type_name)) {
        return GetBuiltinComponentDefaultProperties(type_name);
    }
    if (Runtime().lua_state == nullptr) return properties;

    auto type_it = Runtime().component_type_tables.find(type_name);
    if (type_it == Runtime().component_type_tables.end()) {
        return properties;
    }

    type_it->second.push(Runtime().lua_state);
    lua_pushnil(Runtime().lua_state);
    while (lua_next(Runtime().lua_state, -2) != 0) {
        if (lua_type(Runtime().lua_state, -2) != LUA_TSTRING) {
            lua_pop(Runtime().lua_state, 1);
            continue;
        }

        Actor::ComponentProperty property;
        property.name = lua_tostring(Runtime().lua_state, -2);

        if (!TryReadPropertyValueFromLuaStack(-1, property.value)) {
            lua_pop(Runtime().lua_state, 1);
            continue;
        }

        properties.emplace_back(std::move(property));
        lua_pop(Runtime().lua_state, 1);
    }
    lua_pop(Runtime().lua_state, 1);

    std::sort(properties.begin(), properties.end(),
              [](const Actor::ComponentProperty &a,
                 const Actor::ComponentProperty &b) {
                  return a.name < b.name;
              });
    return properties;
}

std::vector<Actor::ComponentSpec>
ComponentManager::GetRuntimeComponentSpecs(Actor::UID actor_uid) {
    std::vector<Actor::ComponentSpec> component_specs;
    auto actor_it = Runtime().actor_components.find(actor_uid);
    if (actor_it == Runtime().actor_components.end()) {
        return component_specs;
    }

    component_specs.reserve(actor_it->second.size());
    for (const std::unique_ptr<ComponentRecord> &component_ptr :
         actor_it->second) {
        if (component_ptr == nullptr || component_ptr->removed) continue;
        Actor::ComponentSpec component_spec;
        component_spec.key = component_ptr->key;
        component_spec.type = component_ptr->type;
        component_specs.emplace_back(std::move(component_spec));
    }

    std::sort(component_specs.begin(), component_specs.end(),
              [](const Actor::ComponentSpec &a,
                 const Actor::ComponentSpec &b) { return a.key < b.key; });
    return component_specs;
}

std::vector<Actor::ComponentProperty>
ComponentManager::GetRuntimeComponentProperties(Actor::UID actor_uid,
                                                const std::string &component_key) {
    std::vector<Actor::ComponentProperty> properties;
    ComponentRecord *component = FindComponentRecord(actor_uid, component_key);
    if (component == nullptr) return properties;
    const auto reference = component->instance_table;
    const auto type = component->type;

    if (IsBuiltinComponentType(type)) {
        properties = GetBuiltinComponentDefaultProperties(type);
    } else {
        properties = GetComponentTypeDefaultProperties(type);
    }

    MergeLiveRuntimePropertyValues(reference, actor_uid, component_key, properties);
    if (!IsComponentRefAlive(reference, actor_uid, component_key))
        throw std::runtime_error("component removed during property lookup");
    MergeDynamicLuaTableProperties(reference, properties);
    if (type == "SpriteAnimation") {
        auto *animation = LuaBoundary::Cast<SpriteAnimation *>(reference);
        properties.push_back({"current_clip", animation->GetCurrentClip()});
        properties.push_back({"frame", animation->GetFrame()});
        properties.push_back({"elapsed_ticks", animation->GetElapsedTicks()});
        properties.push_back({"playing", animation->IsPlaying()});
        properties.push_back({"paused", animation->IsPaused()});
        properties.push_back({"completed", animation->IsCompleted()});
    }
    if (type == "Tilemap") {
        auto *map = LuaBoundary::Cast<Tilemap *>(reference);
        properties.push_back({"width", map->GetWidth()});
        properties.push_back({"height", map->GetHeight()});
        properties.push_back({"cell_size", static_cast<double>(map->GetCellSize())});
        properties.push_back({"revision", static_cast<double>(map->GetRevision())});
    }

    std::sort(properties.begin(), properties.end(),
              [](const Actor::ComponentProperty &a,
                 const Actor::ComponentProperty &b) {
                  return a.name < b.name;
              });
    return properties;
}
