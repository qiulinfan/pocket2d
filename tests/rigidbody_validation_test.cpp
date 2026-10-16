#include "core/Engine.h"
#include "physics/Rigidbody.h"
#include "physics/RayCast.h"
#include "scripting/ComponentManager.h"
#include "shared/resources/ResourcePath.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void Check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
template <typename F> void Reject(F &&operation) {
    bool rejected = false;
    try { operation(); } catch (const std::exception &) { rejected = true; }
    Check(rejected, "invalid Rigidbody operation accepted");
}
void Lua(lua_State *state, const char *code) {
    const int top = lua_gettop(state); const int status = luaL_dostring(state, code);
    const std::string error = status == LUA_OK ? "" : lua_tostring(state, -1);
    lua_settop(state, top); if (status != LUA_OK) throw std::runtime_error(error);
}
void Configuration() {
    Check(Rigidbody::ValidateNumericProperty("width", 0.001) == 0.001f,
          "documented minimum dimension rejected before float conversion");
    Reject([] { Rigidbody::ValidateNumericProperty("density", 1e-100); });
    Rigidbody body; body.has_trigger = false;
    body.width = 0; Reject([&] { body.OnStart(); });
    Check(!body.HasBody() && !Rigidbody::HasPhysicsWorld(), "invalid shape created physics world");
    body.width = 1; body.density = 1e-40f;
    Reject([&] { body.OnStart(); });
    Check(!body.HasBody(), "unsafe inverse mass created body");
    body.density = 0; body.ValidateConfiguration(); // Box2D zero-density fallback remains valid.
    body.density = 1; body.gravity_scale = 1e38f;
    Reject([&] { body.ValidateConfiguration(); });
    body.gravity_scale = 1e16f; body.ValidateConfiguration();
    body.gravity_scale = 0;
    body.collider_type = "triangle"; Reject([&] { body.ValidateConfiguration(); }); body.collider_type = "box";
    body.body_type = "invalid"; Reject([&] { body.ValidateConfiguration(); }); body.body_type = "dynamic";
    body.friction = -1; Reject([&] { body.ValidateConfiguration(); }); body.friction = 0.3f;
    body.bounciness = 1e38f; Reject([&] { body.ValidateConfiguration(); }); body.bounciness = 0.3f;
    body.angular_friction = -1; Reject([&] { body.ValidateConfiguration(); }); body.angular_friction = 0.3f;
    body.OnStart(); Check(body.HasBody(), "valid configuration could not start after rejection");
    body.OnDestroy(); Rigidbody::DestroyPhysicsWorld();
}
void MethodsAndForces() {
    Rigidbody body; body.has_trigger = false; body.gravity_scale = 0; body.OnStart();
    auto native = body.body;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    Reject([&] { body.SetPosition({nan, 0}); });
    Reject([&] { body.SetPosition({1e9f, 0}); });
    Reject([&] { body.SetVelocity({1e38f, 0}); });
    Reject([&] { body.SetVelocity({inf, 0}); });
    Reject([&] { body.SetRotation(nan); });
    Reject([&] { body.SetAngularVelocity(1e38f); });
    Reject([&] { body.SetGravityScale(1e38f); });
    Reject([&] { body.AddForce({inf, 0}); });
    Reject([&] { body.SetUpDirection({nan, 0}); });
    Reject([&] { body.SetRightDirection({0, inf}); });
    Reject([&] { body.SetEffectiveBodyType("wrong"); });
    Check(body.body == native && body.x == 0 && body.y == 0 && body.rotation == 0 &&
          body.gravity_scale == 0 && body.GetPosition() == Vector2() &&
          body.GetVelocity() == Vector2() && body.GetAngularVelocity() == 0,
          "rejected methods changed configuration or body");
    body.x = 10;
    Check(body.GetPosition() == Vector2(), "raw position unexpectedly changed live body");
    body.SetPosition({10, 0});
    Check(body.GetPosition() == Vector2(10, 0), "SetPosition compared pending field instead of live pose");
    const float live_mass = b2Body_GetMass(body.body);
    body.density = 1e-40f; // Pending configuration is invalid for a future reconstruction.
    body.SetEffectiveBodyType("dynamic");
    Check(body.body == native && b2Body_GetMass(body.body) == live_mass && body.density == 1e-40f,
          "no-op effective type consumed pending configuration");
    Reject([&] { body.ValidateConfiguration(); });
    body.density = 1;
    body.SetPosition({2, 3}); body.SetRotation(45); body.SetVelocity({1, 2});
    body.SetAngularVelocity(10); body.SetGravityScale(0.5f);
    Check(body.GetPosition() == Vector2(2, 3) && body.GetVelocity() == Vector2(1, 2) &&
          std::abs(body.GetRotation() - 45) < 0.001f && body.GetGravityScale() == 0.5f,
          "valid methods failed after rejected inputs");
    body.SetUpDirection({1e38f, 1e38f}); Check(std::isfinite(body.GetRotation()), "large finite direction overflowed");
    body.SetRightDirection({0, 0}); // Existing zero-direction no-op.
    body.SetAngularVelocity(0); body.SetVelocity({0, 0}); body.SetGravityScale(0);
    body.SetPosition({0, 0}); body.SetRotation(0);
    body.AddForce({4e19f, 0});
    Reject([&] { body.AddForce({4e19f, 0}); }); // Each single force fits; their same-step sum does not.
    body.AddForce({-4e19f, 0});
    Rigidbody::StepPhysicsWorld();
    Check(body.GetPosition() == Vector2() && body.GetVelocity() == Vector2(),
          "rejected force entered Box2D accumulation");
    body.AddForce({4e19f, 0}); Rigidbody::StepPhysicsWorld();
    Check(body.GetPosition().IsValid() && body.GetVelocity().IsValid(), "accepted force produced nonfinite state");
    body.SetVelocity({0, 0});
    body.AddForce({4e19f, 0}); // Step clears accumulator; this must not be rejected as a second same-step force.
    Rigidbody::StepPhysicsWorld();
    Check(body.body == native && body.GetPosition().IsValid(), "force epoch reset changed body identity/state");
    body.OnDestroy(); Rigidbody::DestroyPhysicsWorld();
}
void MomentumCombination() {
    Rigidbody light; light.has_trigger = false; light.gravity_scale = 0; light.OnStart();
    const Vector2 velocity(2e17f, 0);
    light.SetVelocity(velocity);
    Check(light.GetVelocity() == velocity, "test velocity is not independently valid");
    light.OnDestroy();
    Rigidbody heavy; heavy.has_trigger = false; heavy.gravity_scale = 0; heavy.density = 1e20f;
    heavy.ValidateConfiguration(); heavy.OnStart();
    auto native = heavy.body;
    const float mass = b2Body_GetMass(native);
    Reject([&] { heavy.SetVelocity(velocity); });
    Check(heavy.body == native && b2Body_GetMass(native) == mass && heavy.GetVelocity() == Vector2() &&
          heavy.GetPosition() == Vector2(),
          "rejected mass/velocity product changed live body");
    heavy.SetVelocity({1, 0});
    Check(heavy.GetVelocity() == Vector2(1, 0), "valid heavy-body control failed after rejection");
    heavy.OnDestroy(); Rigidbody::DestroyPhysicsWorld();
}
struct Fixture {
    std::filesystem::path previous = ResourcePath::ResourcesRootPath();
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("pocket-rigidbody-validation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() {
        std::filesystem::create_directories(root / "scenes");
        std::filesystem::create_directories(root / "component_types");
        std::ofstream(root / "game.config") << R"({"initial_scene":"test"})";
        std::ofstream(root / "scenes/test.scene") << R"({"actors":[{"uid":1,"name":"owner","components":{"body":{"type":"Rigidbody","gravity_scale":0,"width":0.001,"height":0.001,"has_trigger":false}}}]})";
        ResourcePath::SetResourcesRootPath(root);
    }
    ~Fixture() { ResourcePath::SetResourcesRootPath(previous); std::error_code error; std::filesystem::remove_all(root, error); }
};
void LuaTypesAndStaleReferences() {
    Fixture fixture; Engine engine; engine.InitializeHeadless(); engine.StepSimulation();
    auto instance = ComponentManager::GetComponentByKey(1, "body"); auto *state = instance.state();
    auto *body = instance.cast<Rigidbody *>().value(); auto native = body->body;
    Check(B2_IS_NON_NULL(native) && body->width == 0.001f && body->height == 0.001f,
          "scene preparation rejected minimum dimensions");
    Lua(state, R"(
      r=Actor.Find('owner'):GetComponent('Rigidbody')
      local function bad(field,value)
        local old=r[field];assert(not pcall(function()r[field]=value end));assert(r[field]==old)
      end
      bad('x','2');bad('y',1e309);bad('rotation',0/0);bad('width',0);bad('height',1e100)
      bad('gravity_scale',1e38);bad('density',-1);bad('density',1e-100);bad('angular_friction',-1)
      bad('friction',1e38);bad('bounciness',-1)
      bad('enabled',1);bad('precise','false');bad('has_collider',nil);bad('has_trigger',{})
      bad('body_type',1);bad('collider_type','triangle');bad('trigger_type',false)
      for _,name in ipairs({'SetRotation','SetAngularVelocity','SetGravityScale'}) do
        assert(not pcall(function()r[name](r,'2')end))
        assert(not pcall(function()r[name](r,1e309)end))
      end
      assert(not pcall(function()r:SetVelocity(Vector2(1e38,0))end))
      assert(not pcall(function()r:SetPosition(Vector2(1e9,0))end))
      assert(not pcall(function()r:SetGravityScale(1e-100)end));assert(r:GetGravityScale()==0)
      r.width=0.001;assert(r.width>=0.001 and r.width<0.001001)
      r.enabled=false;r.enabled=true;r.precise=false;r.precise=true
      r:SetPosition(Vector2(2,3));r:SetVelocity(Vector2(1,0));r:SetRotation(30)
      r:SetAngularVelocity(5);r:SetGravityScale(0)
    )");
    Check(body->body == native && body->GetPosition() == Vector2(2, 3), "Lua rejection or valid control replaced body");
    ComponentManager::RemoveComponent(1, instance); ComponentManager::FinalizeFrameMutations();
    Check(!body->HasBody(), "removed Rigidbody kept native body");
    const auto count = RayCast::GetBodyCount();
    body->OnStart(); body->SetPosition({1, 1}); body->SetVelocity({1, 1});
    body->SetGravityScale(0); body->AddForce({1, 1});
    Check(!body->HasBody() && RayCast::GetBodyCount() == count,
          "stale Rigidbody reference resurrected native body");
}
}
int main() {
    try {
        Configuration(); MethodsAndForces(); MomentumCombination(); LuaTypesAndStaleReferences();
        std::cout << "rigidbody_validation_test: passed\n"; return 0;
    } catch (const std::exception &error) {
        std::cerr << "rigidbody_validation_test: " << error.what() << '\n'; return 1;
    }
}
