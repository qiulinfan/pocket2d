#include "scripting/LuaBoundary.h"
#include "core/World.h"
#include "Internal.h"
#include "scripting/RuntimeActorAssets.h"
#include "rendering/SpriteAnimation.h"
#include "shared/animation/SpriteAnimationAsset.h"
#include "scripting/ComponentManager.h"
#include <algorithm>
#include <map>
#include <stdexcept>

namespace {
using namespace ManagerDetail;
struct Configuration { std::string resource, target = "sprite", clip; };
using SharedAnimationData = std::shared_ptr<const SpriteAnimationAsset::Data>;
struct Prepared { std::string resource; SharedAnimationData data; };
struct PreparedAssets { std::map<std::pair<Actor::UID, std::string>, Prepared> prepared; };
auto &PreparedForWorld() { return World::Current().State<PreparedAssets>().prepared; }
Configuration ReadConfiguration(const Actor::ComponentSpec &spec) {
    Configuration result;
    for (const auto &property : spec.overrides) {
        if (property.name == "enabled" && std::holds_alternative<bool>(property.value)) continue;
        auto *destination = property.name == "animation_resource" ? &result.resource :
                            property.name == "renderer_key" ? &result.target :
                            property.name == "clip" ? &result.clip : nullptr;
        const auto *value = std::get_if<std::string>(&property.value);
        if (!destination || !value) throw std::runtime_error("invalid or readonly SpriteAnimation field '" + property.name + "'");
        *destination = *value;
    }
    if (result.target.empty()) throw std::runtime_error("SpriteAnimation renderer_key must not be empty");
    return result;
}
void ValidateInitialClip(const SpriteAnimationAsset::Data &data, const std::string &clip) {
    if (!clip.empty() && SpriteAnimationAsset::FindClip(data, clip) == nullptr)
        throw std::runtime_error("SpriteAnimation unknown clip '" + clip + "'");
}
void ValidateTargets(const Actor &actor) {
    std::map<std::string, std::string> targets;
    for (const auto &spec : actor.component_specs) {
        if (spec.type != "SpriteAnimation") continue;
        const auto config = ReadConfiguration(spec);
        if (config.resource.empty()) continue;
        const auto target = std::find_if(actor.component_specs.begin(), actor.component_specs.end(),
            [&](const auto &candidate) { return candidate.key == config.target && candidate.type == "SpriteRenderer"; });
        if (target == actor.component_specs.end()) throw std::runtime_error("SpriteAnimation requires SpriteRenderer key '" + config.target + "'");
        if (!targets.emplace(config.target, spec.key).second)
            throw std::runtime_error("SpriteAnimation target '" + config.target + "' has multiple animators");
    }
}
void ValidateLiveTarget(Actor::UID uid, const std::string &key, const std::string &target) {
    const auto *renderer = FindComponentRecord(uid, target);
    if (!renderer || renderer->removed || renderer->type != "SpriteRenderer")
        throw std::runtime_error("SpriteAnimation requires live SpriteRenderer key '" + target + "'");
    auto found = Runtime().actor_components.find(uid);
    if (found == Runtime().actor_components.end()) return;
    for (const auto &record : found->second) {
        if (record->removed || record->type != "SpriteAnimation" || record->key == key) continue;
        auto *other = LuaBoundary::Cast<SpriteAnimation *>(record->instance_table);
        if (!other->animation_resource.empty() && other->renderer_key == target)
            throw std::runtime_error("SpriteAnimation target '" + target + "' has multiple animators");
    }
}
struct LiveAnimation {
    ActorHandle owner;
    std::string key;
    luabridge::LuaRef reference;
    SpriteAnimation *native;
};
std::vector<LiveAnimation> SnapshotAnimations() {
    ManagerDetail::RuntimeState &runtime = ManagerDetail::Runtime();
    std::vector<LiveAnimation> result;
    for (const auto uid : runtime.actor_uids_sorted) {
        const auto actor = runtime.actor_by_uid.find(uid);
        if (actor == runtime.actor_by_uid.end()) continue;
        ActorHandle handle(actor->second);
        if (!handle.IsValid()) continue;
        const auto components = runtime.actor_components.find(uid);
        if (components == runtime.actor_components.end()) continue;
        for (const auto &record : components->second)
            if (!record->removed && record->type == "SpriteAnimation")
                result.push_back({handle, record->key, record->instance_table,
                                  LuaBoundary::Cast<SpriteAnimation *>(record->instance_table)});
    }
    return result;
}
bool Alive(const LiveAnimation &animation) {
    return animation.owner.IsValid() && IsComponentRefAlive(animation.reference, animation.owner.uid, animation.key);
}
}

void ComponentManager::ValidateSpriteAnimationActor(const Actor &actor) {
    ValidateTargets(actor);
    for (const auto &spec : actor.component_specs) {
        if (spec.type != "SpriteAnimation") continue;
        const auto config = ReadConfiguration(spec);
        if (!config.resource.empty()) ValidateInitialClip(SpriteAnimationAsset::LoadResource(config.resource), config.clip);
    }
}
void ComponentManager::PrepareSpriteAnimationAssets(const std::vector<Actor> &actors) {
    std::remove_reference_t<decltype(PreparedForWorld())> next;
    std::map<std::string, SharedAnimationData> resources;
    for (const auto &actor : actors) {
        ValidateTargets(actor);
        for (const auto &spec : actor.component_specs) {
            if (spec.type != "SpriteAnimation") continue;
            try {
                const auto config = ReadConfiguration(spec);
                if (config.resource.empty()) continue;
                auto &data = resources[config.resource];
                if (!data) data = std::make_shared<const SpriteAnimationAsset::Data>(SpriteAnimationAsset::LoadResource(config.resource));
                ValidateInitialClip(*data, config.clip);
                next.emplace(std::make_pair(actor.uid, spec.key), Prepared{config.resource, data});
            } catch (const std::exception &error) {
                throw std::runtime_error("SpriteAnimation actor " + std::to_string(actor.uid) + " component '" + spec.key + "': " + error.what());
            }
        }
    }
    PreparedForWorld() = std::move(next);
}
void ComponentManager::ClearPreparedSpriteAnimationAssets() { PreparedForWorld().clear(); }
SpriteAnimation *ComponentManager::GetSpriteAnimation(Actor::UID uid, const std::string &key) {
    const auto *record = FindComponentRecord(uid, key);
    return record && !record->removed && record->type == "SpriteAnimation" ? LuaBoundary::Cast<SpriteAnimation *>(record->instance_table) : nullptr;
}
void ComponentManager::InitializeSpriteAnimations(Actor::UID uid) {
    const auto actor = Runtime().actor_by_uid.find(uid);
    if (actor == Runtime().actor_by_uid.end()) return;
    ValidateTargets(*actor->second);
    auto records = Runtime().actor_components.find(uid);
    if (records == Runtime().actor_components.end()) return;
    for (const auto &record : records->second) {
        if (record->removed || record->type != "SpriteAnimation") continue;
        auto *animation = LuaBoundary::Cast<SpriteAnimation *>(record->instance_table);
        if (animation->HasAsset() || animation->animation_resource.empty()) continue;
        ValidateLiveTarget(uid, record->key, animation->renderer_key);
        const auto cached = PreparedForWorld().find({uid, record->key});
        auto data = cached != PreparedForWorld().end() && cached->second.resource == animation->animation_resource
            ? cached->second.data : std::make_shared<const SpriteAnimationAsset::Data>(SpriteAnimationAsset::LoadResource(animation->animation_resource));
        ValidateInitialClip(*data, animation->clip);
        animation->Initialize(data);
        animation->ApplyPose();
    }
}
bool ComponentManager::SetSpriteAnimationProperty(Actor::UID uid, const std::string &key,
    const std::string &name, const Actor::ComponentPropertyValue &value) {
    auto *animation = GetSpriteAnimation(uid, key);
    if (!animation) return false;
    if (name == "enabled") {
        const auto *enabled = std::get_if<bool>(&value);
        if (!enabled) return false;
        animation->enabled = *enabled;
        return true;
    }
    const auto *text = std::get_if<std::string>(&value);
    if (!text) return false;
    Configuration next{animation->animation_resource, animation->renderer_key, animation->clip};
    auto *field = name == "animation_resource" ? &next.resource : name == "renderer_key" ? &next.target : name == "clip" ? &next.clip : nullptr;
    if (!field) return false;
    if (*field == *text) return true;
    *field = *text;
    if (next.target.empty()) return false;
    if (next.resource.empty()) {
        animation->ClearAsset();
        animation->animation_resource.clear();
        animation->renderer_key = next.target;
        animation->clip = next.clip;
        return true;
    }
    ValidateLiveTarget(uid, key, next.target);
    const auto data = SpriteAnimationAsset::LoadResource(next.resource);
    ValidateInitialClip(data, next.clip);
    // All user input is validated before clearing the old authored preview.
    animation->Stop();
    animation->animation_resource = next.resource;
    animation->renderer_key = next.target;
    animation->clip = next.clip;
    animation->Initialize(data);
    animation->ApplyPose();
    return true;
}
void ComponentManager::ApplySpriteAnimationPoses() {
    for (const auto &animation : SnapshotAnimations())
        if (Alive(animation)) animation.native->ApplyPose();
}
void ComponentManager::ProcessSpriteAnimations() {
    ManagerDetail::RuntimeState &runtime = ManagerDetail::Runtime();
    struct Delivery { LiveAnimation animation; std::vector<SpriteAnimation::Event> events; };
    std::vector<Delivery> deliveries;
    for (auto &animation : SnapshotAnimations()) {
        if (!Alive(animation)) continue;
        auto events = animation.native->AdvanceTick();
        deliveries.push_back({std::move(animation), std::move(events)});
    }
    for (const auto &delivery : deliveries) {
        const auto &animation = delivery.animation;
        if (delivery.events.empty()) continue;
        // Retain values, not vector iterators or ComponentRecord pointers, across Lua calls.
        std::vector<std::pair<std::string, luabridge::LuaRef>> recipients;
        const auto found = runtime.actor_components.find(animation.owner.uid);
        if (found == runtime.actor_components.end()) continue;
        for (const auto &record : found->second) {
            if (record->removed || !record->on_start_called || IsBuiltinComponentType(record->type)) continue;
            // Do not read Lua fields while traversing this vector: __index can
            // add/remove components and invalidate the container's iterators.
            recipients.emplace_back(record->key, record->instance_table);
        }
        for (const auto &event : delivery.events) {
            for (const auto &[key, reference] : recipients) {
                if (!Alive(animation) || animation.native->GetPlaybackId() != event.playback_id) break;
                auto *record = FindComponentRecord(animation.owner.uid, key);
                if (!record || !record->on_start_called || record->removed ||
                    !IsComponentRefAlive(reference, animation.owner.uid, key)) continue;
                const int top = lua_gettop(runtime.lua_state);
                try {
                    const auto callback = ProtectedField(reference, "OnAnimationEvent");
                    if (!callback.isFunction()) continue;
                    if (!Alive(animation) || animation.native->GetPlaybackId() != event.playback_id) break;
                    if (!IsComponentRefAlive(reference, animation.owner.uid, key)) continue;
                    const auto enabled = ProtectedField(reference, "enabled");
                    if (!LuaBoundary::IsEnabled(enabled)) continue;
                    // Both getters are executable Lua. Recheck after discovery,
                    // using identities instead of the earlier native record.
                    if (!Alive(animation) || animation.native->GetPlaybackId() != event.playback_id) break;
                    if (!IsComponentRefAlive(reference, animation.owner.uid, key)) continue;
                    auto payload = luabridge::newTable(runtime.lua_state);
                    payload["type"] = event.type; payload["clip"] = event.clip;
                    payload["frame"] = event.frame; payload["marker"] = event.marker;
                    payload["component_key"] = animation.key;
                    LuaBoundary::CallDiscard(callback, reference, payload);
                } catch (const LuaBoundary::Error &error) {
                    ReportError(GetActorNameByUID(animation.owner.uid) + "/" + key + ".OnAnimationEvent", error);
                }
                lua_settop(runtime.lua_state, top);
            }
        }
    }
}

namespace ManagerDetail {
void PrepareDetachedAnimationAssets(const Actor &actor,
    const std::vector<std::unique_ptr<ComponentRecord>> &records) {
    ValidateTargets(actor);
    // Share immutable animation data only within this preparation. Never replace
    // the global cache belonging to an enclosing scene transaction.
    std::map<std::string, SharedAnimationData> resources;
    for (const auto &record : records) {
        if (record->type != "SpriteAnimation") continue;
        try {
            auto *animation = LuaBoundary::Cast<SpriteAnimation *>(record->instance_table);
            if (animation->actor != nullptr || animation->actor_handle.generation != 0 || animation->HasAsset())
                throw std::runtime_error("expected a fresh detached SpriteAnimation");
            if (animation->animation_resource.empty()) continue;
            const auto target = std::find_if(records.begin(), records.end(), [&](const auto &candidate) {
                return candidate->key == animation->renderer_key && candidate->type == "SpriteRenderer";
            });
            if (target == records.end()) throw std::runtime_error("SpriteAnimation detached renderer target is missing");
            auto &data = resources[animation->animation_resource];
            if (!data) data = std::make_shared<const SpriteAnimationAsset::Data>(
                SpriteAnimationAsset::LoadResource(animation->animation_resource));
            ValidateInitialClip(*data, animation->clip);
            // Generation zero cannot resolve a renderer: Initialize only stores
            // data/timeline here. Publication applies the pose after binding.
            animation->Initialize(data);
        } catch (const std::exception &error) {
            throw std::runtime_error("SpriteAnimation actor " + std::to_string(actor.uid) +
                " component '" + record->key + "': " + error.what());
        }
    }
}
void ApplyDetachedActorPoses(Actor::UID uid) {
    const auto owner = Runtime().actor_by_uid.find(uid);
    if (owner == Runtime().actor_by_uid.end() || !owner->second || owner->second->runtime_destroyed) return;
    const auto records = Runtime().actor_components.find(uid);
    if (records == Runtime().actor_components.end()) return;
    for (const auto &record : records->second) {
        if (!record->removed && record->type == "SpriteAnimation")
            LuaBoundary::Cast<SpriteAnimation *>(record->instance_table)->ApplyPose();
    }
}
}
