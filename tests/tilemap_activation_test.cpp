#include "core/Engine.h"
#include "physics/Rigidbody.h"
#include "physics/RayCast.h"
#include "scene/ActorHandle.h"
#include "scene/Tilemap.h"
#include "scripting/ComponentManager.h"
#include "scripting/LuaBoundary.h"
#include "shared/resources/ResourcePath.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
void Check(bool condition, const std::string &message) { if (!condition) throw std::runtime_error(message); }
void Write(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path()); std::ofstream stream(path); stream << text;
    Check(stream.good(), "cannot write temporary activation fixture");
}
void Lua(lua_State *state, const char *source) {
    const int top = lua_gettop(state);
    if (luaL_loadstring(state, source) != LUA_OK) {
        const std::string message = lua_tostring(state, -1); lua_settop(state, top); throw std::runtime_error(message);
    }
    LuaBoundary::ProtectedCall(state, 0, 0);
    Check(lua_gettop(state) == top, "activation request leaked the Lua stack");
}
int Integer(const luabridge::LuaRef &value, const char *field) { return LuaBoundary::Cast<int>(value[field]); }
bool Boolean(const luabridge::LuaRef &value, const char *field) { return LuaBoundary::Cast<bool>(value[field]); }
struct Fixture {
    fs::path previous = ResourcePath::ResourcesRootPath();
    fs::path root = fs::temp_directory_path() / ("pocket-map-activation-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    explicit Fixture(const std::string &mode) {
        Write(root / "game.config", R"({"initial_scene":"main"})");
        Write(root / "scenes/main.scene", std::string(R"({"actors":[
          {"uid":1,"name":"player","components":{
            "body":{"type":"Rigidbody","x":1.25,"y":0.5,"gravity_scale":0,"collider_type":"circle","radius":0.5,"has_trigger":false},
            "transform":{"type":"Transform","x":1.25,"y":0.5},"probe":{"type":"Probe","mode":")") + mode + R"("}}},
          {"uid":2,"name":"terrain","components":{"map":{"type":"Tilemap","map_resource":"test.tilemap.json"},"transform":{"type":"Transform"}}}
        ]})");
        Write(root / "maps/test.tilemap.json", R"({"version":1,"width":2,"height":2,"cell_size":1,
          "atlas":{"image":"pixel.bmp","columns":1,"rows":1,"tile_width_px":1,"tile_height_px":1},
          "layers":{"ground":{"sorting_order":-1,"cells":[1,1,1,1]},"solid":{"sorting_order":0,"cells":[1,0,0,0]}}})");
        fs::create_directories(root / "images"); unsigned char bmp[58]{};
        bmp[0]='B';bmp[1]='M';bmp[2]=58;bmp[10]=54;bmp[14]=40;bmp[18]=1;bmp[22]=1;bmp[26]=1;bmp[28]=24;bmp[34]=4;
        bmp[54]=255;bmp[55]=255;bmp[56]=255;
        std::ofstream image(root / "images/pixel.bmp", std::ios::binary);
        image.write(reinterpret_cast<const char *>(bmp), sizeof(bmp)); image.close();
        Write(root / "component_types/Probe.lua", R"(
Probe={mode='',enters=0,exits=0,request=false,armed=false,completed=false,rejected=false,during_cell=-1,
 OnCollisionEnter=function(self,event) self.enters=self.enters+1 end,
 OnCollisionExit=function(self,event)
  self.exits=self.exits+1
  if not self.armed then return end
  self.armed=false
  local map=Actor.Find('terrain'):GetComponent('Tilemap')
  local body=self.actor:GetComponent('Rigidbody')
  if self.mode=='guard_map' then
   local ok=pcall(function()map:OnDestroy()end);self.rejected=not ok
  elseif self.mode=='guard_body' then
   local ok=pcall(function()body:OnDestroy()end);self.rejected=not ok
  elseif self.mode=='deferred_map' then Actor.Destroy(event.other)
  elseif self.mode=='deferred_body' then self.actor:RemoveComponent(body)
  elseif self.mode=='onstart' then
   assert(map.CommitPending==nil,'this test must use only public Lua surface')
   map:OnStart()
  elseif self.mode=='queued_cell' then
   assert(map:SetCell('solid',0,0,0));self.during_cell=map:GetCell('solid',0,0)
  end
  self.completed=true
 end,
 OnUpdate=function(self)
  if not self.request then return end
  self.request=false;self.armed=true
  if self.mode=='guard_body' or self.mode=='deferred_body' then
   Actor.Find('terrain'):GetComponent('Tilemap').enabled=false
  else self.actor:GetComponent('Rigidbody').enabled=false end
 end}
)");
        ResourcePath::SetResourcesRootPath(root);
    }
    ~Fixture() { ResourcePath::SetResourcesRootPath(previous); std::error_code error; fs::remove_all(root, error); }
};
void Run(const std::string &mode) {
    Fixture fixture(mode); Engine engine; engine.InitializeHeadless();
    auto probe = ComponentManager::GetComponentByKey(1, "probe");
    auto body_ref = ComponentManager::GetComponentByKey(1, "body");
    auto map_ref = ComponentManager::GetComponentByKey(2, "map");
    auto *body = LuaBoundary::Cast<Rigidbody *>(body_ref); auto *map = LuaBoundary::Cast<Tilemap *>(map_ref);
    const ActorHandle map_owner(engine.GetRuntimeActorByUID(2));
    engine.StepSimulation(); engine.StepSimulation();
    Check(Integer(probe, "enters") >= 1 && body->body && body->body->GetContactList(), "fixture did not establish a real body/map contact");
    auto *original_body = body->body; const int body_count = RayCast::GetPhysicsWorld()->GetBodyCount();
    Check(body_count == 2 && map->GetCell("solid",0,0) == 1, "unexpected fixture body/map state");
    Lua(probe.state(), "Actor.Find('player'):GetComponent('Probe').request=true");
    engine.StepSimulation();
    Check(Boolean(probe,"completed") && Integer(probe,"exits") >= 1, mode + ": activation exit callback did not finish");
    Check(ComponentManager::GetRuntimeErrorCount() == 0 && !engine.RequiresSceneReset(), mode + ": expected contained callback behavior failed");
    if (mode == "guard_map" || mode == "guard_body") {
        Check(Boolean(probe,"rejected"), mode + ": unsafe direct destruction was accepted");
        Check(map_owner.IsValid() && map->GetCell("solid",0,0) == 1 && body->body == original_body &&
              RayCast::GetPhysicsWorld()->GetBodyCount() == body_count, mode + ": rejection changed native participants/data");
        Check(body_ref.rawequal(ComponentManager::GetComponentByKey(1,"body")) &&
              map_ref.rawequal(ComponentManager::GetComponentByKey(2,"map")), "rejection replaced native userdata");
        if (mode == "guard_map") {
            Check(!body->body->IsEnabled(), "requested body disable did not complete");
            const auto hit=RayCast::Cast(Vector2(-1,0.5f),Vector2(1,0),3);
            Check(hit && hit->actor_handle.uid==2, "blocked map destruction removed collision geometry");
        } else Check(body->body->IsEnabled() && !map->enabled, "requested map disable or surviving body state is wrong");
    } else if (mode == "deferred_map") {
        Check(!map_owner.IsValid() && engine.GetRuntimeActorByUID(2)==nullptr &&
              RayCast::GetPhysicsWorld()->GetBodyCount()==body_count-1, "deferred map deletion did not safely finish");
    } else if (mode == "deferred_body") {
        Check(ComponentManager::GetComponentByKey(1,"body").isNil() && body->body==nullptr &&
              RayCast::GetPhysicsWorld()->GetBodyCount()==body_count-1, "deferred body removal did not safely finish");
    } else if (mode == "queued_cell") {
        Check(Integer(probe,"during_cell")==1 && map->GetCell("solid",0,0)==1,
              "callback cell edit committed inside the active contact traversal");
    } else Check(map->GetCell("solid",0,0)==1 && RayCast::GetPhysicsWorld()->GetBodyCount()==body_count,
                 "public OnStart unexpectedly changed live collision geometry");
    engine.StepSimulation();
    if (mode == "queued_cell") Check(map->GetCell("solid",0,0)==0, "queued cell edit did not commit at the next normal boundary");
    Check(ComponentManager::GetRuntimeErrorCount()==0 && !engine.RequiresSceneReset(), "subsequent frame did not remain usable");
}
}
int main() {
    try {
        for (const auto mode : {"guard_map","guard_body","deferred_map","deferred_body","onstart","queued_cell"}) Run(mode);
        std::cout << "tilemap_activation_test: cross-owner activation guards, deferred cleanup and queued edits passed\n"; return 0;
    } catch (const std::exception &error) { std::cerr << "tilemap_activation_test: " << error.what() << '\n'; return 1; }
}
