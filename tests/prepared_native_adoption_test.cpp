#include "core/Engine.h"
#include "physics/Rigidbody.h"
#include "physics/RayCast.h"
#include "rendering/SpriteAnimation.h"
#include "rendering/SpriteRenderer.h"
#include "scene/Tilemap.h"
#include "scripting/ComponentManager.h"
#include "shared/resources/ResourcePath.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
template <typename F> void Reject(F &&operation) {
    bool rejected = false; try { operation(); } catch (const std::exception &) { rejected = true; }
    Check(rejected, "invalid adoption was accepted");
}
TilemapAsset::Data MapData(int width, int tile) {
    TilemapAsset::Data data; data.width = data.height = width;
    data.atlas = {"deliberately-not-on-disk.bmp", 4, 2, 2, 2};
    data.ground.cells.assign(static_cast<std::size_t>(width) * width, tile);
    data.solid.cells.assign(static_cast<std::size_t>(width) * width, 0); data.solid.cells[0] = 1;
    return data;
}
SpriteAnimationAsset::Data AnimationData(const char *marker) {
    SpriteAnimationAsset::Data data;
    data.image = "deliberately-not-on-disk.bmp";
    data.columns = 4; data.rows = 2; data.tile_width_px = data.tile_height_px = 2;
    data.clips = {{"idle", true, {{1, 2, 5, marker}, {1, 3, 5, "later"}}}};
    return data;
}
struct Fixture {
    std::filesystem::path previous = ResourcePath::ResourcesRootPath();
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("pocket-native-adoption-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() {
        std::filesystem::create_directories(root / "scenes");
        std::filesystem::create_directories(root / "component_types");
        std::ofstream(root / "game.config") << R"({"initial_scene":"test"})";
        std::ofstream(root / "scenes/test.scene") << R"({"actors":[{"uid":1,"name":"owner","components":{
            "map":{"type":"Tilemap"},"animation":{"type":"SpriteAnimation"},"sprite":{"type":"SpriteRenderer"}}}]})";
        ResourcePath::SetResourcesRootPath(root);
    }
    ~Fixture() { ResourcePath::SetResourcesRootPath(previous); std::error_code error; std::filesystem::remove_all(root, error); }
};
void TilemapAdoption() {
    Fixture fixture; Engine engine; engine.InitializeHeadless();
    auto reference = ComponentManager::GetComponentByKey(1, "map");
    auto *map = reference.cast<Tilemap *>().value(); const auto owner = map->actor_handle; auto *actor = map->actor;
    map->map_resource = "old.tilemap.json"; map->Initialize(MapData(2, 1)); map->CommitPending();
    const b2WorldId world = RayCast::GetPhysicsWorld(); const b2BodyId old_body = map->GetChunkBody(0, 0);
    Check(B2_IS_NON_NULL(world) && B2_IS_NON_NULL(old_body), "old map collision missing");
    // Map collision is a hollow outline, so the visitor straddles its top edge.
    b2BodyDef definition = b2DefaultBodyDef(); definition.type = b2_dynamicBody; definition.position = {0.5f, 0.0f};
    definition.gravityScale = 0;
    const b2BodyId visitor = b2CreateBody(world, &definition);
    b2ShapeDef shape_def = b2DefaultShapeDef(); shape_def.enableContactEvents = true;
    const b2Polygon shape = b2MakeBox(0.1f, 0.1f);
    b2CreatePolygonShape(visitor, &shape_def, &shape);
    b2World_Step(world, 1.0f / 60, 4);
    Check(b2World_GetContactEvents(world).beginCount > 0, "test did not establish real Box2D contact");
    map->ReplaceAsset(MapData(2, 7)); Check(map->SetCell("ground", 0, 0, 8), "queue old map edit");
    Tilemap prepared; prepared.key = "detached-key"; prepared.map_resource = "new.tilemap.json";
    prepared.Initialize(MapData(4, 2), 10, 20);
    Check(RayCast::GetBodyCount() == 2 && map->GetCell("ground", 0, 0) == 1 && prepared.GetFixtureCount() == 0,
          "preparing candidate changed old map/native bodies");
    const auto revision = map->GetRevision();
    map->AdoptPreparedState(prepared);
    Check(ComponentManager::GetTilemap(1, "map") == map && map->actor == actor && (map->actor_handle.uid == owner.uid && map->actor_handle.generation == owner.generation) && map->key == "map",
          "map adoption changed live userdata identity");
    Check(map->GetWidth() == 4 && map->GetCell("ground", 0, 0) == 2 && map->map_resource == "new.tilemap.json" &&
          map->GetFixtureCount() == 0 && RayCast::GetBodyCount() == 1 && map->GetRevision() == revision + 1 && !prepared.HasAsset(),
          "map adoption retained old edits/bodies or did not consume candidate");
    // Destroying the old body ended the touch; Box2D reports it after the next
    // step, and a handler then sees and queues against the adopted base.
    b2World_Step(world, 1.0f / 60, 4);
    Check(b2World_GetContactEvents(world).endCount > 0, "adoption did not end the old contact");
    Check(map->GetWidth() == 4 && map->GetCellCenter(0, 0) == Vector2(10.5f, 20.5f),
          "end-contact handler did not observe the newly adopted base");
    map->ReplaceAsset(MapData(3, 3));
    Check(map->SetCell("ground", 2, 2, 4), "handler cell edit was not queued");
    map->enabled = false;
    Check(!map->CollectVisible(10, 20, 14, 24).empty(), "handler desired flag changed committed rendering early");
    map->CommitPending();
    Check(map->GetWidth() == 3 && map->GetCell("ground", 2, 2) == 4 && map->GetRevision() == revision + 2 &&
          B2_IS_NON_NULL(map->GetChunkBody(0, 0)) && !b2Body_IsEnabled(map->GetChunkBody(0, 0)) &&
          map->CollectVisible(10, 20, 13, 23).empty(),
          "adoption discarded handler replacement/cell/enabled commands");
    Reject([&] { map->AdoptPreparedState(prepared); });
    Tilemap empty; empty.enabled = false;
    map->AdoptPreparedState(empty);
    Check(!map->HasAsset() && map->GetWidth() == 0 && map->GetFixtureCount() == 0 && map->key == "map" && (map->actor_handle.uid == owner.uid && map->actor_handle.generation == owner.generation),
          "empty map adoption failed to clear data while preserving identity");
}
void AnimationAdoption() {
    Fixture fixture; Engine engine; engine.InitializeHeadless();
    auto reference = ComponentManager::GetComponentByKey(1, "animation");
    auto sprite_ref = ComponentManager::GetComponentByKey(1, "sprite");
    auto *animation = reference.cast<SpriteAnimation *>().value(); auto *sprite = sprite_ref.cast<SpriteRenderer *>().value();
    const auto owner = animation->actor_handle; auto *actor = animation->actor;
    animation->animation_resource = "old.animation.json"; animation->clip = "idle";
    animation->Initialize(AnimationData("old")); animation->Restart("idle");
    const auto old_events = animation->AdvanceTick(); Check(!old_events.empty(), "old event missing");
    const auto old_playback = animation->GetPlaybackId(); const auto pose_owner = sprite->GetAnimationOverrideOwner();
    animation->Pause();
    SpriteAnimation prepared; prepared.key = "candidate"; prepared.animation_resource = "new.animation.json"; prepared.clip = "idle";
    prepared.enabled = false; prepared.Initialize(AnimationData("new"));
    Check(sprite->GetAnimationOverrideOwner() == pose_owner && animation->GetElapsedTicks() == 1,
          "detached animation preparation changed live pose/timeline");
    animation->AdoptPreparedState(prepared);
    Check(ComponentManager::GetSpriteAnimation(1, "animation") == animation && animation->actor == actor &&
          (animation->actor_handle.uid == owner.uid && animation->actor_handle.generation == owner.generation) && animation->key == "animation" && !sprite->HasAnimationOverride(),
          "animation adoption changed identity or applied pose before peer publication");
    Check(animation->GetPlaybackId() == old_playback + 1 && animation->GetFrame() == 1 && animation->GetElapsedTicks() == 0 &&
          !animation->IsPaused() && !animation->enabled && !prepared.HasAsset() && prepared.GetFrame() == 0,
          "animation adoption did not transfer prepared state/invalidate old playback");
    animation->ApplyPose();
    Check(sprite->HasAnimationOverride() && sprite->GetAnimationOverrideOwner() == pose_owner,
          "animation adoption copied candidate pose ownership");
    animation->enabled = true;
    const auto events = animation->AdvanceTick();
    Check(events.size() == 1 && events[0].marker == "new" && events[0].playback_id == animation->GetPlaybackId() &&
          events[0].playback_id != old_events[0].playback_id, "new pending events retained candidate/stale playback identity");
    Reject([&] { animation->AdoptPreparedState(prepared); });
    SpriteAnimation empty; animation->AdoptPreparedState(empty);
    Check(!animation->HasAsset() && animation->GetFrame() == 0 && !sprite->HasAnimationOverride() && animation->key == "animation",
          "empty animation adoption failed to stop/clear without losing identity");
}
}
int main() {
    try { TilemapAdoption(); AnimationAdoption(); std::cout << "prepared_native_adoption_test: passed\n"; return 0; }
    catch (const std::exception &error) { std::cerr << "prepared_native_adoption_test: " << error.what() << '\n'; return 1; }
}
