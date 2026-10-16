#include "core/Engine.h"
#include "physics/Rigidbody.h"
#include "physics/RayCast.h"
#include "scene/Transform.h"
#include "scripting/ComponentManager.h"
#include "scripting/LuaBoundary.h"
#include "shared/resources/ResourcePath.h"
#include "../src/engine/scripting/manager/Internal.h"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
// Number of touching Box2D contacts on a body.
int Touching(b2BodyId body) {
    std::vector<b2ContactData> contacts(static_cast<std::size_t>(b2Body_GetContactCapacity(body)));
    return b2Body_GetContactData(body, contacts.data(), static_cast<int>(contacts.size()));
}
void Check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void Lua(lua_State *state, const char *source) {
    const int top = lua_gettop(state);
    if (luaL_loadstring(state, source) != LUA_OK) {
        const std::string error = lua_tostring(state, -1); lua_settop(state, top); throw std::runtime_error(error);
    }
    LuaBoundary::ProtectedCall(state, 0, 0);
    Check(lua_gettop(state) == top, "activation script leaked stack");
}
struct Fixture {
    std::filesystem::path previous = ResourcePath::ResourcesRootPath();
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("pocket-rigidbody-activation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() {
        std::filesystem::create_directories(root / "scenes");
        std::filesystem::create_directories(root / "component_types");
        std::ofstream(root / "game.config") << R"({"initial_scene":"test"})";
        std::ofstream(root / "scenes/test.scene") << R"({"actors":[
          {"uid":1,"name":"controller","components":{"logic":{"type":"ActivationControl"}}},
          {"uid":2,"name":"early","components":{
            "body":{"type":"Rigidbody","enabled":false,"gravity_scale":0,"has_trigger":false,"width":0.5,"height":0.5},
            "probe":{"type":"ActivationProbe","enabled":false},"transform":{"type":"Transform"}}},
          {"uid":3,"name":"late","components":{
            "body":{"type":"Rigidbody","enabled":false,"gravity_scale":0,"has_trigger":false,"x":20,"width":0.5,"height":0.5},
            "probe":{"type":"ActivationProbe","enabled":false},"transform":{"type":"Transform","x":20}}},
          {"uid":4,"name":"wall","components":{
            "body":{"type":"Rigidbody","body_type":"static","gravity_scale":0,"has_trigger":false,"x":10},
            "transform":{"type":"Transform","x":10}}}
        ]})";
        std::ofstream(root / "component_types/ActivationProbe.lua") << R"(
          ActivationProbe={starts=0,enters=0,exits=0,exit_mode="",rejections=0,deferred_requests=0,
            OnStart=function(self)self.starts=self.starts+1 end,
            OnCollisionEnter=function(self,event)if event.other:GetName()=='wall' then self.enters=self.enters+1 end end,
            OnCollisionExit=function(self,event)
              if event.other:GetName()~='wall' then return end
              self.exits=self.exits+1
              local mode=self.exit_mode
              self.exit_mode=''
              if mode=='guard' then
                local body=self.actor:GetComponent('Rigidbody')
                local other=event.other:GetComponent('Rigidbody')
                local own_ok=pcall(function()body:OnDestroy()end)
                local other_ok=pcall(function()other:OnDestroy()end)
                if not own_ok then self.rejections=self.rejections+1 end
                if not other_ok then self.rejections=self.rejections+1 end
              elseif mode=='deferred' then
                self.actor:RemoveComponent(self.actor:GetComponent('Rigidbody'))
                self.deferred_requests=self.deferred_requests+1
                Actor.Destroy(event.other)
                self.deferred_requests=self.deferred_requests+1
              end
            end}
        )";
        std::ofstream(root / "component_types/ActivationControl.lua") << R"(
          ActivationControl={drive=true,
            OnStart=function(self)
              early=Actor.Find('early'):GetComponent('Rigidbody')
              late=Actor.Find('late'):GetComponent('Rigidbody')
              early.enabled=true
              Actor.Find('early'):GetComponent('ActivationProbe').enabled=true
            end,
            OnUpdate=function(self)
              if early.enabled then early:SetVelocity(Vector2(self.drive and 2 or 0,0)) end
              if late.enabled then late:SetVelocity(Vector2(self.drive and 1 or 0,0)) end
            end}
        )";
        ResourcePath::SetResourcesRootPath(root);
    }
    ~Fixture() { ResourcePath::SetResourcesRootPath(previous); std::error_code error; std::filesystem::remove_all(root, error); }
};
int Field(const luabridge::LuaRef &reference, const char *name) { return LuaBoundary::Cast<int>(reference[name]); }
void Step(Engine &engine, int frames) { for (int i = 0; i < frames; ++i) engine.StepSimulation(); }
void Run() {
    Fixture fixture; Engine engine; engine.InitializeHeadless();
    auto early_ref = ComponentManager::GetComponentByKey(2, "body");
    auto late_ref = ComponentManager::GetComponentByKey(3, "body");
    auto early_probe = ComponentManager::GetComponentByKey(2, "probe");
    auto late_probe = ComponentManager::GetComponentByKey(3, "probe");
    auto wall_ref = ComponentManager::GetComponentByKey(4, "body");
    auto *wall = LuaBoundary::Cast<Rigidbody *>(wall_ref);
    auto *early = LuaBoundary::Cast<Rigidbody *>(early_ref); auto *late = LuaBoundary::Cast<Rigidbody *>(late_ref);
    auto *state = early_ref.state();
    Step(engine, 1);
    Check(early->enabled && early->HasBody() && Field(early_probe, "starts") == 1,
          "OnStart activation did not start initially disabled body/script");
    auto early_body = early->body; const float first_x = early->GetPosition().x;
    Step(engine, 5);
    Check(early->GetPosition().x > first_x + 0.1f,
          "OnStart enabled body failed to advance: physics hierarchy remained disabled");
    auto transform_ref = ComponentManager::GetComponentByKey(2, "transform");
    auto *transform = LuaBoundary::Cast<Transform *>(transform_ref);
    Check(std::abs(transform->x - early->GetPosition().x) < 0.001f &&
          engine.GetRuntimePhysicsHierarchyStateByUID(2).effective_body_type == "dynamic",
          "enabled dynamic motion did not update Transform/cache");
    Check(!late->HasBody() && Field(late_probe, "starts") == 0 &&
          !ManagerDetail::FindComponentRecord(3, "body")->on_start_called &&
          !ManagerDetail::FindComponentRecord(3, "probe")->on_start_called,
          "disabled component consumed its one-shot pending start");
    const int bodies = RayCast::GetBodyCount();
    Lua(state, "late.enabled=true;Actor.Find('late'):GetComponent('ActivationProbe').enabled=true");
    Step(engine, 1);
    Check(late->HasBody() && Field(late_probe, "starts") == 1 && RayCast::GetBodyCount() == bodies + 1,
          "later activation did not create exactly one body/start");
    auto late_body = late->body; const float late_x = late->GetPosition().x;
    Step(engine, 5);
    Check(late->GetPosition().x > late_x + 0.05f && Field(late_probe, "starts") == 1,
          "late body is not moving or its Start repeated");
    // An explicit move into a collider while disabled must not produce contacts.
    Lua(state, R"(
      early.enabled=false
      local transform=Actor.Find('early'):GetComponent('Transform')
      transform.x=10;transform.y=0;early:SetPosition(Vector2(10,0))
    )");
    Step(engine, 1); const auto stopped = early->GetPosition(); const int enters = Field(early_probe, "enters");
    Check(early->body == early_body && !b2Body_IsEnabled(early_body) && Touching(early_body) == 0,
          "disabling body did not suspend Box2D contacts without reconstruction");
    Step(engine, 3);
    Check(early->GetPosition() == stopped && Field(early_probe, "enters") == enters,
          "disabled body moved or generated collision enter");
    Lua(state, "early.enabled=true"); Step(engine, 2);
    Check(early->body == early_body && b2Body_IsEnabled(early_body) && Field(early_probe, "enters") > enters,
          "reenabled body did not recover real collision participation");
    Check(Field(early_probe, "starts") == 1, "reenable restarted already-started script");
    Check(Touching(early_body) > 0,
          "guard regression requires a real touching Box2D contact");
    auto wall_body = wall->body;
    const int before_guard_exits = Field(early_probe, "exits");
    Lua(state, R"(
      Actor.Find('controller'):GetComponent('ActivationControl').drive=false
      Actor.Find('early'):GetComponent('ActivationProbe').exit_mode='guard'
      early.enabled=false
    )");
    Step(engine, 1);
    Check(Field(early_probe, "exits") == before_guard_exits + 1 && Field(early_probe, "rejections") == 2,
          "activation EndContact must reject direct destruction of current and adjacent bodies");
    Check(early->body == early_body && wall->body == wall_body &&
          RayCast::GetBodyCount() == bodies + 1,
          "rejected EndContact destruction changed native body identity or lifetime");
    Lua(state, "early.enabled=true"); Step(engine, 1);

    for (int i = 0; i < 3; ++i) {
        Lua(state, "late.enabled=false;Actor.Find('late'):GetComponent('ActivationProbe').enabled=false"); Step(engine, 2);
        Lua(state, "late.enabled=true;Actor.Find('late'):GetComponent('ActivationProbe').enabled=true"); Step(engine, 2);
    }
    Check(late->body == late_body && Field(late_probe, "starts") == 1 &&
          RayCast::GetBodyCount() == bodies + 1, "activation churn leaked/recreated bodies or repeated Start");
    Lua(state, R"(
      Actor.Find('controller'):GetComponent('ActivationControl').drive=false
      local transform=Actor.Find('early'):GetComponent('Transform')
      transform.x=2;transform.y=5;early:SetPosition(Vector2(2,5));early.body_type='kinematic'
    )");
    Step(engine, 1);
    Check(early->body == early_body && b2Body_GetType(early_body) == b2_kinematicBody &&
          engine.GetRuntimePhysicsHierarchyStateByUID(2).effective_body_type == "kinematic",
          "Lua body_type write did not update actual body and hierarchy cache");
    Check(early->GetPosition() == Vector2(2, 5), "kinematic body did not follow authored Transform");
    Lua(state, "early.body_type='dynamic';Actor.Find('controller'):GetComponent('ActivationControl').drive=true");
    Step(engine, 4);
    Check(b2Body_GetType(early_body) == b2_dynamicBody && early->GetPosition().x > 2.05f &&
          std::abs(transform->x - early->GetPosition().x) < 0.001f,
          "dynamic body_type restoration did not resume physics-owned motion");
    // Recreate contact, then request both supported deferred cleanup paths from
    // the synchronous EndContact caused by SetEnabled(false).
    Lua(state, R"(
      Actor.Find('controller'):GetComponent('ActivationControl').drive=false
      local t=Actor.Find('early'):GetComponent('Transform')
      t.x=10;t.y=0;early:SetPosition(Vector2(10,0));early:SetVelocity(Vector2(0,0))
      saved_wall=Actor.Find('wall')
    )");
    Step(engine, 1);
    Check(Touching(early_body) > 0,
          "deferred cleanup regression requires a real touching Box2D contact");
    const int before_deferred_exits = Field(early_probe, "exits");
    Lua(state, "Actor.Find('early'):GetComponent('ActivationProbe').exit_mode='deferred';early.enabled=false");
    Step(engine, 1);
    Check(Field(early_probe, "exits") == before_deferred_exits + 1 && Field(early_probe, "deferred_requests") == 2,
          "EndContact did not accept both deferred component and actor destruction requests");
    Check(!early->HasBody() && !wall->HasBody() &&
          ComponentManager::GetComponentByKey(2, "body").isNil() &&
          ComponentManager::GetComponentByKey(4, "body").isNil() &&
          RayCast::GetBodyCount() == bodies - 1,
          "deferred EndContact cleanup failed to remove exactly the two native bodies");
    Lua(state, "assert(not saved_wall:IsValid());assert(Actor.Find('early'):IsValid())");
    Step(engine, 3);
    Check(Field(early_probe, "exits") == before_deferred_exits + 1 &&
          RayCast::GetBodyCount() == bodies - 1,
          "destroyed bodies participated in later physics or cleanup repeated");
    Check(ComponentManager::GetRuntimeErrorCount() == 0, "activation callbacks reported runtime errors");
}
}
int main() {
    try { Run(); std::cout << "rigidbody_activation_test: deferred start, activation, contacts, reentry and deferred cleanup passed\n"; return 0; }
    catch (const std::exception &error) { std::cerr << "rigidbody_activation_test: " << error.what() << '\n'; return 1; }
}
