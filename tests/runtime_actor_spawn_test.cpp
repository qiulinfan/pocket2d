#include "core/Engine.h"
#include "scene/ActorHandle.h"
#include "physics/Rigidbody.h"
#include "scripting/ComponentManager.h"
#include "shared/resources/ResourcePath.h"
#include "../src/engine/scripting/manager/Internal.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <tuple>

namespace {
namespace fs = std::filesystem;
using namespace ManagerDetail;
void Check(bool value, const std::string &message) { if (!value) throw std::runtime_error(message); }
void Put(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path()); std::ofstream(path) << text;
}
void Lua(const std::string &text) {
    auto *state = Runtime().lua_state;
    const int top = lua_gettop(state);
    if (luaL_dostring(state, text.c_str()) != LUA_OK) {
        std::string error = lua_tostring(state, -1); lua_settop(state, top);
        throw std::runtime_error(error);
    }
    lua_settop(state, top);
}
struct Fixture {
    fs::path previous = ResourcePath::ResourcesRootPath();
    fs::path root = fs::temp_directory_path() / ("pocket-spawn-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() {
        Put(root / "game.config", R"({"initial_scene":"main"})");
        Put(root / "scenes/main.scene", R"({"actors":[{"uid":1,"name":"source","components":{"a_good":{"type":"Good"},"b_body":{"type":"Rigidbody","body_type":"static","has_trigger":false},"z_late":{"type":"Late"}}}]})");
        Put(root / "component_types/Good.lua", R"(
AUDIT={starts=0,destroys=0}
Good={health=100,starts=0,updates=0,
 OnStart=function(self) self.starts=self.starts+1; AUDIT.starts=AUDIT.starts+1 end,
 OnUpdate=function(self) self.updates=self.updates+1 end,
 OnDestroy=function(self)
  AUDIT.destroys=AUDIT.destroys+1
  local source=Actor.Find('source')
  if source then source:GetComponent('Good').health=source:GetComponent('Good').health-1 end
 end}
)");
        Put(root / "component_types/Late.lua", "Late={value=3}");
        Put(root / "component_types/Bad.lua", "Bad={owned=Rigidbody()}");
        Template("Valid", R"("good":{"type":"Good"})");
        Template("BadDefault", R"("a_good":{"type":"Good"},"z_bad":{"type":"Bad"})");
        Template("MissingType", R"("a_good":{"type":"Good"},"z_bad":{"type":"NeverRegistered"})");
        Template("BadBody", R"("a_good":{"type":"Good"},"z_bad":{"type":"Rigidbody","width":0})");
        Template("BadParticles", R"("a_good":{"type":"Good"},"z_bad":{"type":"ParticleSystem","duration_frames":0})");
        Template("MissingMap", R"("a_good":{"type":"Good"},"z_bad":{"type":"Tilemap","map_resource":"absent.tilemap.json"})");
        Template("MissingAnimation", R"("a_good":{"type":"Good"},"sprite":{"type":"SpriteRenderer"},"z_bad":{"type":"SpriteAnimation","animation_resource":"absent.animation.json","renderer_key":"sprite"})");
        Template("MissingTarget", R"("a_good":{"type":"Good"},"z_bad":{"type":"SpriteAnimation","renderer_key":"absent","animation_resource":"absent.animation.json"})");
        // Self-authored 1x1 white 24-bit BMP; no source-project art dependency.
        unsigned char bmp[58]{};
        bmp[0]='B'; bmp[1]='M'; bmp[2]=58; bmp[10]=54; bmp[14]=40;
        bmp[18]=1; bmp[22]=1; bmp[26]=1; bmp[28]=24; bmp[34]=4;
        bmp[54]=255; bmp[55]=255; bmp[56]=255;
        fs::create_directories(root / "images");
        std::ofstream image(root / "images/dot.bmp", std::ios::binary);
        image.write(reinterpret_cast<const char *>(bmp), sizeof(bmp)); image.close();
        Put(root / "animations/dot.animation.json", R"({"version":1,"image":"dot.bmp","columns":1,"rows":1,"tile_width_px":1,"tile_height_px":1,"clips":[{"name":"idle","loop":true,"frames":[{"row":1,"column":1,"ticks":2}]}]})");
        Template("MissingTarget", R"("a_good":{"type":"Good"},"z_bad":{"type":"SpriteAnimation","renderer_key":"absent","animation_resource":"dot.animation.json"})");
        Template("SharedTarget", R"("a_good":{"type":"Good"},"sprite":{"type":"SpriteRenderer"},"y_first":{"type":"SpriteAnimation","animation_resource":"dot.animation.json"},"z_bad":{"type":"SpriteAnimation","animation_resource":"dot.animation.json"})");
        Template("Animated", R"("anim":{"type":"SpriteAnimation","animation_resource":"dot.animation.json","clip":"idle"},"sprite":{"type":"SpriteRenderer"})");
        ResourcePath::SetResourcesRootPath(root);
    }
    void Template(const std::string &name, const std::string &components) {
        Put(root / "actor_templates" / (name + ".template"),
            "{\"name\":\"" + name + "\",\"components\":{" + components + "}}");
    }
    ~Fixture() { Actor::ClearTemplateCache(); ResourcePath::SetResourcesRootPath(previous);
        std::error_code error; fs::remove_all(root, error); }
};
auto State(const Engine &engine) {
    std::size_t records = 0;
    for (const auto &[uid, list] : Runtime().actor_components) records += list.size();
    return std::tuple{engine.GetRuntimeActors().size(), records,
        Runtime().actor_components.size(), Runtime().actor_by_uid.size(),
        Runtime().actors_by_name.size(), Runtime().component_index_by_key.size(),
        Runtime().actor_order_by_uid.size(), Runtime().actor_uids_sorted.size(),
        Runtime().pending_on_start.size(), Runtime().pending_actor_uids_to_activate.size(),
        Runtime().pending_destroy_actor_uids.size(), Runtime().dirty_component_actor_uids.size(),
        Runtime().on_update_components.size(), Runtime().on_late_update_components.size()};
}
void Rejection() {
    Fixture fixture; Engine engine; engine.InitializeHeadless(); engine.StepSimulation();
    const Actor *source = engine.GetRuntimeActorByUID(1);
    const ActorHandle handle(source);
    auto source_good = ComponentManager::GetComponentByKey(1, "a_good");
    auto source_body = ComponentManager::GetComponentByKey(1, "b_body");
    auto *body = source_body.cast<Rigidbody *>().value()->body;
    Check(body != nullptr, "source must have a real Box2D body before rejection");
    const auto before = State(engine);
    for (const std::string name : {"BadDefault", "MissingType", "BadBody", "BadParticles",
                                  "MissingMap", "MissingAnimation", "MissingTarget", "SharedTarget"}) {
        Lua("assert(not pcall(function() Actor.Instantiate('" + name + "') end)); assert(Actor.Find('" + name + "')==nil)");
        Check(State(engine) == before, name + " leaked records, indexes or pending lifecycle work");
        Lua("assert(AUDIT.starts==1 and AUDIT.destroys==0); assert(Actor.Find('source'):GetComponent('Good').health==100)");
        engine.StepSimulation();
        Check(State(engine) == before, name + " left delayed publication/cleanup");
        Check(source_good.rawequal(ComponentManager::GetComponentByKey(1, "a_good")), "rejection replaced source Lua instance");
        Check(source_body.rawequal(ComponentManager::GetComponentByKey(1, "b_body")) &&
              source_body.cast<Rigidbody *>().value()->body == body, "rejection replaced source native body");
        Check(engine.GetRuntimeActorByUID(1) == source && handle.IsValid(), "failure changed source identity");
        Lua("assert(AUDIT.starts==1 and AUDIT.destroys==0); assert(Actor.Find('source'):GetComponent('Good').health==100)");
    }
    Lua("assert(not pcall(function() Actor.Find('source'):AddComponent('Bad') end))");
    Check(State(engine) == before, "failed AddComponent changed queues or component registry");
    engine.StepSimulation();
    Check(State(engine) == before, "failed AddComponent left delayed work");
    Lua("Late.owned=Rigidbody()");
    Actor::UID copy_uid = 0;
    Check(!engine.DuplicateRuntimeActorByUID(1, &copy_uid), "bad duplicate must return false");
    Check(copy_uid == 0, "rejected duplicate wrote output identity");
    Check(!engine.GetLastRuntimeActorError().empty() && !engine.RequiresSceneReset(),
          "ordinary duplicate failure must report a recoverable diagnostic");
    Check(ComponentManager::FindActorByName("source (copy)").isNil(), "partial duplicate findable");
    Check(State(engine) == before, "duplicate rejection leaked live state");
    engine.StepSimulation();
    Check(State(engine) == before, "duplicate rejection published on next tick");
    Check(source_body.cast<Rigidbody *>().value()->body == body &&
          source_good.rawequal(ComponentManager::GetComponentByKey(1, "a_good")),
          "duplicate rejection replaced source native/Lua state");
    Lua("assert(AUDIT.starts==1 and AUDIT.destroys==0); assert(Actor.Find('source'):GetComponent('Good').health==100); Late.owned=nil");
    Check(engine.DuplicateRuntimeActorByUID(1, &copy_uid), "valid duplicate after rejection failed");
    Check(copy_uid != 0 && copy_uid != 1, "duplicate identity not unique");
    auto copy = ComponentManager::GetComponentByKey(copy_uid, "a_good");
    Check(copy["starts"].cast<int>().value() == 0, "duplicate called OnStart synchronously");
    engine.StepSimulation();
    Check(copy["starts"].cast<int>().value() == 1 && copy["updates"].cast<int>().value() == 1,
          "host duplicate must start and update on next step");
    fixture.Template("BadBody", R"("a_good":{"type":"Good"},"z_bad":{"type":"Rigidbody","width":1,"gravity_scale":0})");
    auto repaired = ComponentManager::InstantiateActor("BadBody");
    Check(repaired.cast<ActorHandle>().value().IsValid(), "rejected template poisoned live-spawn cache after file repair");
    Check(!ComponentManager::GetComponentByKey(repaired.cast<ActorHandle>().value().uid, "z_bad").isNil(),
          "repaired template missing valid native component");
    auto animated = ComponentManager::InstantiateActor("Animated");
    auto animation = ComponentManager::GetComponentByKey(animated.cast<ActorHandle>().value().uid, "anim");
    Check(animation["current_clip"].cast<std::string>().value() == "idle" && animation["frame"].cast<int>().value() == 1,
          "valid native animation must be initialized before Instantiate returns");
    engine.StepSimulation();
    Check(ComponentManager::GetComponentByKey(repaired.cast<ActorHandle>().value().uid, "z_bad").cast<Rigidbody *>().value()->body != nullptr,
          "repaired native spawn did not create its real body on deferred start");
    Check(ComponentManager::GetRuntimeErrorCount() == 0, "caught preparation failure poisoned runtime");
}
void PublishedBeforeStart() {
    Fixture fixture; Engine engine; engine.InitializeHeadless(); engine.StepSimulation();
    auto spawned = ComponentManager::InstantiateActor("Valid");
    const auto handle = spawned.cast<ActorHandle>().value();
    auto good = ComponentManager::GetComponentByKey(handle.uid, "good");
    Check(handle.IsValid() && !ComponentManager::FindActorByName("Valid").isNil(), "successful spawn not immediately published");
    Check(good["starts"].cast<int>().value() == 0, "spawn started synchronously");
    ComponentManager::DestroyActor(handle.Resolve());
    engine.StepSimulation();
    Check(!handle.IsValid() && engine.GetRuntimeActorByUID(handle.uid) == nullptr, "destroyed published actor survived");
    Lua("assert(AUDIT.starts==1 and AUDIT.destroys==1); assert(Actor.Find('source'):GetComponent('Good').health==99)");
    auto next = ComponentManager::InstantiateActor("Valid");
    auto next_good = ComponentManager::GetComponentByKey(next.cast<ActorHandle>().value().uid, "good");
    engine.StepSimulation();
    Check(next_good["starts"].cast<int>().value() == 1 && next_good["updates"].cast<int>().value() == 0,
          "host-idle Instantiate must start next tick before its deferred update-index activation");
    engine.StepSimulation();
    Check(next_good["starts"].cast<int>().value() == 1 && next_good["updates"].cast<int>().value() == 1,
          "published spawn lifecycle did not continue exactly once");
}
}
int main() {
    try { Rejection(); PublishedBeforeStart(); std::cout << "runtime_actor_spawn_test passed\n"; return 0; }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
