#include "core/Engine.h"
#include "core/FrameClock.h"
#include "physics/Rigidbody.h"
#include "physics/RayCast.h"
#include "rendering/SpriteAnimation.h"
#include "rendering/SpriteRenderer.h"
#include "scene/Tilemap.h"
#include "scripting/ComponentManager.h"
#include "scripting/RuntimeActorAssets.h"
#include "scripting/RuntimeActorPlan.h"
#include "scripting/SceneComponentPlan.h"
#include "shared/resources/ResourcePath.h"
#include "shared/tilemap/TilemapAsset.h"
#include "../src/engine/scripting/manager/Internal.h"
#include "SDL3/SDL.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace ManagerDetail;
void Check(bool value, const std::string &message) { if (!value) throw std::runtime_error(message); }
template <typename F> void Reject(F &&operation) {
    bool rejected = false; try { operation(); } catch (const std::exception &) { rejected = true; }
    Check(rejected, "invalid detached asset preparation accepted");
}
struct Fixture {
    std::filesystem::path previous = ResourcePath::ResourcesRootPath();
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("pocket-detached-assets-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() {
        for (const char *directory : {"scenes", "component_types", "images", "maps", "animations"})
            std::filesystem::create_directories(root / directory);
        ResourcePath::SetResourcesRootPath(root);
        auto *image = SDL_CreateSurface(8, 4, SDL_PIXELFORMAT_RGBA32);
        Check(image != nullptr, "create atlas");
        SDL_FillSurfaceRect(image, nullptr, SDL_MapSurfaceRGBA(image, 255, 255, 255, 255));
        const bool saved = SDL_SaveBMP(image, (root / "images/atlas.bmp").string().c_str());
        SDL_DestroySurface(image); Check(saved, "save atlas");
        for (const std::string name : {"outer", "local"}) {
            TilemapAsset::Data data; data.width = data.height = 4;
            data.atlas = {"atlas.bmp", 4, 2, 2, 2};
            data.ground.cells.assign(16, name == "outer" ? 2 : 3); data.solid.cells.assign(16, 0); data.solid.cells[0] = 1;
            TilemapAsset::SaveResource(name + ".tilemap.json", data);
            std::ofstream(root / "animations" / (name + ".animation.json")) <<
                R"({"version":1,"image":"atlas.bmp","columns":4,"rows":2,"tile_width_px":2,"tile_height_px":2,"clips":[{"name":"idle","loop":true,"frames":[{"row":1,"column":2,"ticks":5}]}]})";
        }
        std::ofstream(root / "maps/broken.tilemap.json") << "{";
        std::ofstream(root / "animations/broken.animation.json") << "{";
        std::ofstream(root / "game.config") << R"({"initial_scene":"old"})";
        std::ofstream(root / "scenes/old.scene") << R"({"actors":[
          {"uid":1,"name":"old","components":{"body":{"type":"Rigidbody","gravity_scale":0}}},
          {"uid":2,"name":"assets","components":{
            "animation":{"type":"SpriteAnimation","animation_resource":"outer.animation.json","clip":"idle"},
            "map":{"type":"Tilemap","map_resource":"outer.tilemap.json"},"sprite":{"type":"SpriteRenderer"},
            "transform":{"type":"Transform","x":-8,"y":-8}}}]})";
        std::ofstream(root / "component_types/BadDefaults.lua") << "BadDefaults={owned=Rigidbody()}";
        std::ofstream(root / "component_types/Dynamic.lua") << R"(
          DynamicLookups=0
          Dynamic=setmetatable({}, {__index=function() DynamicLookups=DynamicLookups+1;error('must not execute') end})
        )";
    }
    ~Fixture() { ResourcePath::SetResourcesRootPath(previous); std::error_code error; std::filesystem::remove_all(root, error); }
};
Actor Candidate(Actor::UID uid = Actor::kRuntimeGeneratedUIDStart) {
    Actor actor; actor.uid = uid; actor.actor_name = "detached";
    actor.component_specs = {
        {"animation", "SpriteAnimation", {{"animation_resource", std::string("local.animation.json")}, {"clip", std::string("idle")}}},
        {"map", "Tilemap", {{"map_resource", std::string("local.tilemap.json")}}},
        {"sprite", "SpriteRenderer", {}},
        {"transform", "Transform", {{"x", -4.0}, {"y", -6.0}}}
    };
    return actor;
}
struct Records {
    lua_State *state = ManagerDetail::Runtime().lua_state;
    std::vector<std::unique_ptr<ComponentRecord>> values;
    explicit Records(const Actor &actor) {
        PauseScenePreparationGC(state);
        try {
            for (const auto &spec : actor.component_specs) {
                const auto found = ManagerDetail::Runtime().component_type_tables.find(spec.type);
                const auto prototype = IsBuiltinComponentType(spec.type) ? luabridge::LuaRef(state) : found->second;
                values.push_back(PrepareComponentRecord(spec, prototype));
            }
        } catch (...) { values.clear(); ResumeScenePreparationGC(state); throw; }
    }
    ~Records() { values.clear(); ResumeScenePreparationGC(state); }
    template <typename T> T *Get(const std::string &key) {
        for (const auto &record : values) if (record->key == key) return record->instance_table.cast<T *>().value();
        throw std::runtime_error("missing detached record");
    }
};
void DetachedAndCache() {
    Fixture fixture; Engine engine; engine.InitializeHeadless(); engine.StepSimulation();
    auto *old_actor = engine.GetRuntimeActorByUID(1);
    auto old_body_ref = ComponentManager::GetComponentByKey(1, "body"); auto *old_body = old_body_ref.cast<Rigidbody *>().value()->body;
    auto *old_map = ComponentManager::GetTilemap(2, "map");
    auto *old_animation = ComponentManager::GetSpriteAnimation(2, "animation");
    auto sprite_ref = ComponentManager::GetComponentByKey(2, "sprite"); auto *old_sprite = sprite_ref.cast<SpriteRenderer *>().value();
    auto *world = RayCast::GetPhysicsWorld(); const int bodies = world->GetBodyCount();
    const auto pose_owner = old_sprite->GetAnimationOverrideOwner(); const int frame = FrameClock::GetFrameNumber();
    const auto actor_count = engine.GetActorCount(); const int stack_top = lua_gettop(ManagerDetail::Runtime().lua_state);
    const auto unchanged = [&] {
        Check(engine.GetActorCount() == actor_count && engine.GetRuntimeActorByUID(1) == old_actor &&
            old_body_ref.cast<Rigidbody *>().value()->body == old_body && RayCast::GetPhysicsWorld() == world &&
            world->GetBodyCount() == bodies && old_map->GetCell("ground", 0, 0) == 2 &&
            old_sprite->GetAnimationOverrideOwner() == pose_owner && FrameClock::GetFrameNumber() == frame &&
            lua_gettop(ManagerDetail::Runtime().lua_state) == stack_top, "detached preparation changed the running world");
    };
    auto outer = *engine.GetRuntimeActorByUID(2);
    ComponentManager::PrepareTilemapAssets({outer}); ComponentManager::PrepareSpriteAnimationAssets({outer});
    {
        auto actor = Candidate(2); // Even a colliding UID must not access existing live components here.
        actor.component_specs.push_back({"z_dynamic", "Dynamic", {}});
        Records records(actor); PrepareDetachedActorAssets(actor, records.values);
        auto *map = records.Get<Tilemap>("map"); auto *animation = records.Get<SpriteAnimation>("animation");
        Check(map->GetWidth() == 4 && map->GetCell("ground", 0, 0) == 3 &&
            map->GetCellCenter(0, 0) == Vector2(-3.5f, -5.5f) && map->GetFixtureCount() == 0,
            "detached map data/placement is not frame-zero ready");
        Check(animation->HasAsset() && animation->GetCurrentClip() == "idle" && animation->GetFrame() == 1 &&
            !records.Get<SpriteRenderer>("sprite")->HasAnimationOverride(), "detached animation applied a live pose");
        Check(luabridge::getGlobal(ManagerDetail::Runtime().lua_state, "DynamicLookups").cast<int>().value() == 0,
            "detached preparation executed dynamic getter");
        unchanged();
    }
    auto reject = [&](Actor actor, const char *expected) {
        std::string error; auto plan = PrepareRuntimeActor(std::move(actor), error);
        Check(!plan && error.find(expected) != std::string::npos, "unexpected runtime actor rejection: " + error); unchanged();
    };
    auto bad = Candidate(); bad.component_specs[1].overrides[0].value = std::string("broken.tilemap.json"); reject(bad, "Tilemap");
    bad = Candidate(); bad.component_specs[0].overrides[0].value = std::string("broken.animation.json"); reject(bad, "SpriteAnimation");
    bad = Candidate(); bad.component_specs[0].overrides[1].value = std::string("missing"); reject(bad, "unknown clip");
    bad = Candidate(); bad.component_specs.erase(bad.component_specs.begin() + 2); reject(bad, "requires SpriteRenderer");
    bad = Candidate(); auto duplicate = bad.component_specs[0]; duplicate.key = "animation2"; bad.component_specs.push_back(duplicate); reject(bad, "multiple animators");
    bad = Candidate(); bad.component_specs.push_back({"body", "Rigidbody", {}}); reject(bad, "cannot share");
    bad = Candidate(); bad.component_specs.push_back({"map2", "Tilemap", {}}); reject(bad, "only one Tilemap");
    bad = Candidate(); bad.component_specs[3].overrides.push_back({"rotation", 1.0}); reject(bad, "rotation must be zero");
    bad = Candidate(); bad.component_specs[3].overrides[0].value = std::numeric_limits<double>::infinity(); reject(bad, "finite");
    bad = Candidate(); bad.component_specs[1].overrides.push_back({"width", 4}); reject(bad, "readonly");
    bad = Candidate(); bad.component_specs.push_back({"z_bad", "BadDefaults", {}}); reject(bad, "BadDefaults");
    bad = Candidate(); bad.parent_uid = 1;
    { Records records(bad); Reject([&] { PrepareDetachedActorAssets(bad, records.values); }); unchanged(); }
    // A nested success or rejection must not clear/replace the scene preparation caches.
    std::filesystem::remove(fixture.root / "maps/outer.tilemap.json");
    std::filesystem::remove(fixture.root / "animations/outer.animation.json");
    ComponentManager::InitializeTilemap(2, "map");
    old_animation->ClearAsset(); ComponentManager::InitializeSpriteAnimations(2);
    Check(old_map->GetCell("ground", 0, 0) == 2 && old_animation->HasAsset() && old_sprite->HasAnimationOverride(),
          "nested detached preparation polluted outer scene asset caches");
}
void PublishPreparedWithoutIO() {
    Fixture fixture;
    struct Runtime {
        std::list<Actor> actors;
        Runtime() { ComponentManager::Initialize(); ComponentManager::BindActorsForScene(actors); }
        ~Runtime() { ComponentManager::Shutdown(); }
    } runtime;
    std::string error;
    auto plan = PrepareRuntimeActor(Candidate(), error);
    Check(plan != nullptr, error);
    Check(runtime.actors.empty() && ManagerDetail::Runtime().actor_components.empty() && !Rigidbody::HasPhysicsWorld(),
          "preparation published actor/native physics early");
    std::filesystem::remove(fixture.root / "maps/local.tilemap.json");
    std::filesystem::remove(fixture.root / "animations/local.animation.json");
    auto *actor = PublishRuntimeActor(*plan, runtime.actors, false);
    Check(actor && actor->runtime_generation != 0 && runtime.actors.size() == 1,
          "prepared actor was not published");
    auto *map = ComponentManager::GetTilemap(actor->uid, "map");
    auto *animation = ComponentManager::GetSpriteAnimation(actor->uid, "animation");
    auto sprite_ref = ComponentManager::GetComponentByKey(actor->uid, "sprite");
    Check(map && map->GetCell("ground", 0, 0) == 3 && map->GetWidth() == 4 &&
          map->GetFixtureCount() == 0 && !Rigidbody::HasPhysicsWorld(),
          "published map lacks frame-zero data or created physics before OnStart");
    Check(animation && animation->HasAsset() && animation->GetFrame() == 1 &&
          sprite_ref.cast<SpriteRenderer *>().value()->HasAnimationOverride(),
          "publication failed to apply already-prepared animation pose");
    Check(!FindComponentRecord(actor->uid, "map")->on_start_called &&
          !FindComponentRecord(actor->uid, "animation")->on_start_called,
          "asset publication executed lifecycle callbacks");
}

}
int main() {
    try { DetachedAndCache(); PublishPreparedWithoutIO(); std::cout << "runtime_actor_assets_test: passed\n"; return 0; }
    catch (const std::exception &error) { std::cerr << "runtime_actor_assets_test: " << error.what() << '\n'; return 1; }
}
