#include "scripting/LuaBoundary.h"
#include "core/Profile.h"
#include "core/World.h"
#include "Internal.h"
#include "scripting/RuntimeActorAssets.h"
#include "scene/Tilemap.h"
#include "scene/Transform.h"
#include "shared/tilemap/TilemapAsset.h"
#include "scripting/ComponentManager.h"
#include <cmath>
#include <stdexcept>
#include <limits>
#include <unordered_map>

namespace {
using namespace ManagerDetail;
using PreparedMap = std::pair<std::string, TilemapAsset::Data>;
struct PreparedAssets { std::unordered_map<std::string, PreparedMap> prepared; };
auto &PreparedForWorld() { return World::Current().State<PreparedAssets>().prepared; }

std::string Identity(Actor::UID uid, const std::string &key) {
    return std::to_string(uid) + ":" + key;
}

std::string ResourceOf(const Actor::ComponentSpec &component) {
    for (const auto &property : component.overrides) {
        if (property.name != "map_resource") continue;
        const auto *value = std::get_if<std::string>(&property.value);
        if (value == nullptr) throw std::runtime_error("map_resource must be a string");
        return *value;
    }
    return {};
}

void ValidateOwner(const Actor &actor, const std::vector<Actor::ComponentSpec> &specs,
                   bool validate_fields = true) {
    int maps = 0;
    bool rigidbody = false;
    for (const auto &spec : specs) {
        maps += spec.type == "Tilemap" ? 1 : 0;
        rigidbody |= spec.type == "Rigidbody";
    }
    if (maps == 0) return;
    const std::string prefix = "Tilemap actor " + std::to_string(actor.uid) + ": ";
    if (maps != 1) throw std::runtime_error(prefix + "only one Tilemap is supported");
    if (rigidbody) throw std::runtime_error(prefix + "cannot share an actor with Rigidbody");
    if (actor.parent_uid != Actor::kInvalidUID)
        throw std::runtime_error(prefix + "must be a root actor");
    if (!validate_fields) return;
    for (const auto &spec : specs) {
        if (spec.type == "Tilemap") {
            for (const auto &property : spec.overrides) {
                if (property.name == "map_resource" && std::holds_alternative<std::string>(property.value)) continue;
                if (property.name == "enabled" && std::holds_alternative<bool>(property.value)) continue;
                throw std::runtime_error(prefix + "invalid or readonly Tilemap field '" + property.name + "'");
            }
        }
        if (spec.type != "Transform") continue;
        for (const auto &property : spec.overrides) {
            if (property.name == "enabled" && std::holds_alternative<bool>(property.value)) continue;
            if (property.name != "x" && property.name != "y" && property.name != "rotation")
                throw std::runtime_error(prefix + "invalid Transform field '" + property.name + "'");
            const double number = std::visit([](const auto &value) -> double {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, double> || std::is_same_v<T, int>) return value;
                throw std::runtime_error("Tilemap Transform coordinates must be numeric");
            }, property.value);
            if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max())
                throw std::runtime_error(prefix + "Transform coordinate exceeds finite float range");
            if (property.name == "rotation" && number != 0)
                throw std::runtime_error(prefix + "rotation must be zero");
        }
    }
}

void AuthoredOrigin(const Actor &actor, float &x, float &y, float &rotation) {
    x = y = rotation = 0;
    for (const auto &spec : actor.component_specs) {
        if (spec.type != "Transform") continue;
        for (const auto &property : spec.overrides) {
            float *target = property.name == "x" ? &x : property.name == "y" ? &y :
                            property.name == "rotation" ? &rotation : nullptr;
            if (!target) continue;
            if (const auto *number = std::get_if<double>(&property.value)) *target = *number;
            else if (const auto *number = std::get_if<int>(&property.value)) *target = *number;
            else throw std::runtime_error("Tilemap Transform coordinates must be numeric");
        }
        // Match the primary Transform lookup after binding: component specs
        // are in key order, so later Transform components must not override it.
        return;
    }
}

void Origin(Actor::UID uid, float &x, float &y, float &rotation) {
    x = y = rotation = 0;
    if (ComponentManager::TryGetRuntimeTransformWorld(uid, x, y, rotation)) return;
    // During scene assembly a map's key can sort before its Transform key.
    // Use the same primary authored Transform as scene prevalidation.
    const auto owner = Runtime().actor_by_uid.find(uid);
    if (owner != Runtime().actor_by_uid.end()) AuthoredOrigin(*owner->second, x, y, rotation);
}
}

void ComponentManager::PrepareTilemapAssets(const std::vector<Actor> &actors) {
    std::unordered_map<std::string, PreparedMap> next;
    for (const auto &actor : actors) {
        ValidateOwner(actor, actor.component_specs);
        for (const auto &spec : actor.component_specs) {
            if (spec.type != "Tilemap") continue;
            try {
                const auto resource = ResourceOf(spec);
                if (!resource.empty()) {
                    auto asset = TilemapAsset::LoadResource(resource);
                    float x, y, rotation;
                    AuthoredOrigin(actor, x, y, rotation);
                    // Validate every numeric initialization requirement before
                    // Engine clears the current scene or creates native bodies.
                    Tilemap::ValidateInitialization(asset, x, y);
                    next.emplace(Identity(actor.uid, spec.key), PreparedMap{resource, std::move(asset)});
                }
            } catch (const std::exception &error) {
                throw std::runtime_error("Tilemap actor " + std::to_string(actor.uid) +
                                         " component '" + spec.key + "': " + error.what());
            }
        }
    }
    PreparedForWorld() = std::move(next);
}

void ComponentManager::ClearPreparedTilemapAssets() { PreparedForWorld().clear(); }

Tilemap *ComponentManager::GetTilemap(Actor::UID uid, const std::string &key) {
    auto *record = ManagerDetail::FindComponentRecord(uid, key);
    if (record == nullptr || record->type != "Tilemap") return nullptr;
    return record->Native<Tilemap>();
}

void ComponentManager::InitializeTilemap(Actor::UID uid, const std::string &key) {
    auto *map = GetTilemap(uid, key);
    if (map == nullptr) return;
    const auto owner = ManagerDetail::Runtime().actor_by_uid.find(uid);
    if (owner == ManagerDetail::Runtime().actor_by_uid.end()) return;
    ValidateOwner(*owner->second, GetRuntimeComponentSpecs(uid), false);
    if (map->map_resource.empty()) return;
    float x, y, rotation;
    Origin(uid, x, y, rotation);
    if (rotation != 0) throw std::runtime_error("Tilemap rotation must be zero");
    const auto cached = PreparedForWorld().find(Identity(uid, key));
    if (cached != PreparedForWorld().end() && cached->second.first == map->map_resource)
        map->Initialize(cached->second.second, x, y);
    else map->Initialize(TilemapAsset::LoadResource(map->map_resource), x, y);
}

bool ComponentManager::SetTilemapResource(Actor::UID uid, const std::string &key,
                                         const std::string &resource) {
    auto *map = GetTilemap(uid, key);
    if (map == nullptr || resource.empty()) return false;
    // Parsing/validation belongs to this explicit authoring action, never a
    // per-frame polling fallback. Failed loads preserve the current map.
    auto asset = TilemapAsset::LoadResource(resource);
    float x, y, rotation;
    Origin(uid, x, y, rotation);
    if (rotation != 0) throw std::runtime_error("Tilemap rotation must be zero");
    Tilemap::ValidateInitialization(asset, x, y);
    if (map->HasAsset()) map->ReplaceAsset(asset);
    else {
        map->Initialize(asset, x, y);
    }
    map->map_resource = resource;
    return true;
}

void ComponentManager::CommitTilemaps() {
    POCKET_ZONE("CommitTilemaps");
    ManagerDetail::RuntimeState &runtime = ManagerDetail::Runtime();
    for (Actor::UID uid : ManagerDetail::ActorsWithNative(runtime, ManagerDetail::NativeKind::Tilemap)) {
        const auto owner = runtime.actor_by_uid.find(uid);
        if (owner == runtime.actor_by_uid.end() ||
            owner->second == nullptr || owner->second->runtime_destroyed ||
            runtime.pending_destroy_actor_uids.count(uid)) continue;
        const auto components = runtime.actor_components.find(uid);
        if (components == runtime.actor_components.end()) continue;
        for (auto &entry : components->second) {
            if (entry->removed || entry->Native<Tilemap>() == nullptr) continue;
            if (owner->second->parent_uid != Actor::kInvalidUID)
                throw std::runtime_error("Tilemap must remain a root actor");
            float x, y, rotation;
            Origin(uid, x, y, rotation);
            entry->Native<Tilemap>()->CommitPending(x, y, rotation);
        }
    }
}

void ComponentManager::QueueTilemaps(float min_x, float min_y, float max_x, float max_y,
                                     bool scene_backed_only) {
    for (Actor::UID uid : ManagerDetail::ActorsWithNative(ManagerDetail::Runtime(), ManagerDetail::NativeKind::Tilemap)) {
        const auto owner = ManagerDetail::Runtime().actor_by_uid.find(uid);
        if (owner == ManagerDetail::Runtime().actor_by_uid.end() || owner->second == nullptr ||
            owner->second->runtime_destroyed || (scene_backed_only && !owner->second->scene_backed)) continue;
        const auto components = ManagerDetail::Runtime().actor_components.find(uid);
        if (components == ManagerDetail::Runtime().actor_components.end()) continue;
        for (auto &entry : components->second) {
            if (entry->removed || entry->Native<Tilemap>() == nullptr) continue;
            entry->Native<Tilemap>()->QueueVisible(min_x, min_y, max_x, max_y);
        }
    }
}

bool ComponentManager::IsSolidTileAt(float x, float y) {
    for (Actor::UID uid : ManagerDetail::ActorsWithNative(ManagerDetail::Runtime(), ManagerDetail::NativeKind::Tilemap)) {
        const auto owner = ManagerDetail::Runtime().actor_by_uid.find(uid);
        if (owner == ManagerDetail::Runtime().actor_by_uid.end() || owner->second == nullptr ||
            owner->second->runtime_destroyed) continue;
        const auto components = ManagerDetail::Runtime().actor_components.find(uid);
        if (components == ManagerDetail::Runtime().actor_components.end()) continue;
        for (auto &entry : components->second) {
            if (entry->removed || entry->Native<Tilemap>() == nullptr) continue;
            if (entry->Native<Tilemap>()->IsSolidAt(x, y)) return true;
        }
    }
    return false;
}

namespace ManagerDetail {
void PrepareDetachedTilemapAssets(const Actor &actor,
    const std::vector<std::unique_ptr<ComponentRecord>> &records) {
    ValidateOwner(actor, actor.component_specs);
    float x = 0, y = 0, rotation = 0;
    const ComponentRecord *primary = nullptr;
    for (const auto &record : records) {
        if (record->type == "Transform" && (!primary || record->key < primary->key)) primary = record.get();
    }
    if (primary) {
        const auto *transform = primary->Native<Transform>();
        x = transform->x; y = transform->y; rotation = transform->rotation;
    }
    for (const auto &record : records) {
        if (record->type != "Tilemap") continue;
        try {
            auto *map = record->Native<Tilemap>();
            if (map->actor != nullptr || map->actor_handle.generation != 0 || map->HasAsset())
                throw std::runtime_error("expected a fresh detached Tilemap");
            if (!std::isfinite(rotation) || rotation != 0)
                throw std::runtime_error("Tilemap rotation must be zero");
            if (map->map_resource.empty()) continue;
            // Deliberately independent of the outer scene's prepared cache.
            const auto asset = TilemapAsset::LoadResource(map->map_resource);
            Tilemap::ValidateInitialization(asset, x, y);
            map->Initialize(asset, x, y);
        } catch (const std::exception &error) {
            throw std::runtime_error("Tilemap actor " + std::to_string(actor.uid) +
                " component '" + record->key + "': " + error.what());
        }
    }
}
void PrepareDetachedActorAssets(const Actor &actor,
    const std::vector<std::unique_ptr<ComponentRecord>> &records) {
    PrepareDetachedTilemapAssets(actor, records);
    PrepareDetachedAnimationAssets(actor, records);
}
}
