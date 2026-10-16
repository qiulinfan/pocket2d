#include "core/Engine.h"
#include "scene/ActorHandle.h"
#include "core/FrameClock.h"
#include "physics/RayCast.h"
#include "particles/ParticleManager.h"
#include "scripting/ComponentManager.h"
#include "shared/resources/ResourcePath.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fixture {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("pocket2d-lifetime-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() {
        for (const char *directory : {"scenes", "component_types", "actor_templates"})
            std::filesystem::create_directories(path / directory);
        std::ofstream(path / "game.config") << R"({"initial_scene":"lifetime"})";
        std::ofstream(path / "scenes/lifetime.scene") << R"({"actors":[
            {"uid":1,"name":"target","components":{
                "body":{"type":"Rigidbody","body_type":"static","has_trigger":false},
                "particles":{"type":"ParticleSystem","burst_quantity":1},
                "transform":{"type":"Transform"}}},
            {"uid":2,"name":"watcher","components":{
                "logic":{"type":"LifetimeProbe"},
                "body":{"type":"Rigidbody","x":2,"gravity_scale":0,"has_trigger":false},
                "transform":{"type":"Transform","x":2}}},
            {"uid":3,"name":"trash","components":{}}
        ]})";
        std::ofstream(path / "component_types/LifetimeProbe.lua") << R"(
            LifetimeProbe = {
                top_level_frame = Application.GetFrame(),
                checks = 0,
                OnStart = function(self)
                    self.cached = Actor.Find("target")
                    self.spawned = Actor.Instantiate("Persistent")
                    self.spawned_uid = self.spawned:GetUID()
                end,
                OnCollisionEnter = function(self, collision)
                    self.last_collision = collision
                end,
                OnUpdate = function(self)
                    assert(self.cached:GetUID() == 1)
                    assert(self.cached:GetName() == "target")
                    assert(self.spawned:GetUID() == self.spawned_uid)
                    assert(self.spawned:GetName() == "persistent")
                    self.checks = self.checks + 1
                end
            }
        )";
        std::ofstream(path / "actor_templates/Persistent.template") <<
            R"({"name":"persistent","components":{"transform":{"type":"Transform"}}})";
        std::ofstream(path / "actor_templates/Trash.template") <<
            R"({"name":"trash","components":{}})";
        ResourcePath::SetResourcesRootPath(path);
    }
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void TestStableSurvivors() {
    Engine engine;
    engine.InitializeHeadless();
    engine.StepSimulation();
    const Actor *target = engine.GetRuntimeActorByUID(1);
    Check(target != nullptr, "target actor exists");
    auto hit = RayCast::Cast(Vector2(-2, 0), Vector2(1, 0), 4);
    Check(hit && hit->actor == target, "initial raycast resolves target");
    Check(engine.DeleteRuntimeActorByUID(3), "delete unrelated scene actor");
    Check(engine.GetRuntimeActorByUID(1) == target,
          "pruning unrelated actor must preserve live actor address");

    for (int iteration = 0; iteration < 200; ++iteration) {
        auto trash = ComponentManager::InstantiateActor("Trash");
        ComponentManager::DestroyActor(trash.cast<ActorHandle>().value().Resolve());
        engine.StepSimulation();
        Check(engine.GetRuntimeActorByUID(1) == target,
              "spawn/destroy churn must preserve survivor address");
        Check(engine.GetRuntimeActors().size() == 3,
              "dead nodes must be reclaimed instead of accumulating tombstones");
        auto current_hit = RayCast::Cast(Vector2(-2, 0), Vector2(1, 0), 4);
        Check(current_hit && current_hit->actor == target,
              "Box2D fixture metadata must stay bound after actor removal");
        Check(ComponentManager::GetRuntimeErrorCount() == 0,
              "cached Actor.Find/Instantiate userdata must remain valid");
    }
    Check(ComponentManager::GetComponentByKey(2, "logic")["checks"].cast<int>().value() == 201,
          "cached actor references must be exercised on every real Lua update");
}

void RunLua(lua_State *state, const char *source) {
    if (luaL_dostring(state, source) != LUA_OK) {
        const std::string error = lua_tostring(state, -1);
        lua_pop(state, 1);
        throw std::runtime_error(error);
    }
}

void TestDeadAndReusedHandles() {
    ActorHandle old_native_handle;
    {
        Engine engine;
        engine.InitializeHeadless();
        engine.StepSimulation();
        auto probe = ComponentManager::GetComponentByKey(2, "logic");
        lua_State *state = probe.state();
        old_native_handle = ActorHandle(engine.GetRuntimeActorByUID(1));
        RunLua(state, R"(
            cached_target = Actor.Find("target")
            cached_body = cached_target:GetComponent("Rigidbody")
            cached_particles = cached_target:GetComponent("ParticleSystem")
            cached_hit = Physics.Raycast(Vector2(-2, 0), Vector2(1, 0), 4)
            assert(cached_target == Actor.Find("target"))
            assert(cached_body.actor == cached_target)
            assert(cached_hit.actor == cached_target)
            assert(Actor.FindAll("target")[1] == cached_target)
            assert(cached_target.is_valid and cached_target:IsValid())
            assert(not pcall(function() cached_target.uid = 999 end))
            assert(not pcall(function() cached_target.generation = 999 end))
            Actor.Find("watcher"):GetComponent("Rigidbody"):SetVelocity(Vector2(-2, 0))
        )");
        for (int i = 0; i < 90; ++i) engine.StepSimulation();
        Check(ComponentManager::GetRuntimeErrorCount() == 0, "real collision setup must run");
        RunLua(state, R"(
            cached_collision = Actor.Find("watcher"):GetComponent("LifetimeProbe").last_collision
            assert(cached_collision ~= nil and cached_collision.other == cached_target)
            Actor.Find("watcher"):GetComponent("LifetimeProbe").enabled = false
            Actor.Destroy(cached_target)
            assert(not cached_target.is_valid)
            assert(cached_target:GetComponent("Rigidbody") == nil)
            assert(cached_target:AddComponent("Transform") == nil)
            assert(not cached_target:SetParent(nil))
            Scene.DontDestroy(cached_target)
        )");
        engine.StepSimulation();
        Check(engine.GetRuntimeActorByUID(1) == nullptr, "destroy must reclaim target");
        Check(!old_native_handle.IsValid(), "native handle invalidates after destroy");
        Check(!ParticleManager::IsEmitterAlive(0), "destroy must release target emitter");
        RunLua(state, R"(
            assert(not cached_target:IsValid())
            assert(not cached_body.actor:IsValid())
            assert(not cached_hit.actor:IsValid())
            assert(not cached_collision.other:IsValid())
            cached_body:OnStart()
            cached_body:SetPosition(Vector2(-5, 0))
            cached_body:SetVelocity(Vector2(1, 0))
            assert(cached_body:GetPosition().x == -5)
            assert(Physics.Raycast(Vector2(-7, 0), Vector2(1, 0), 4) == nil)
            cached_particles:OnStart()
            cached_particles:Play()
            cached_particles:OnUpdate()
            cached_particles:Burst()
        )");
        Check(!ParticleManager::IsEmitterAlive(0), "stale particle lifecycle must not revive emitter");
        engine.RequestSceneLoad("lifetime");
        engine.StepSimulation();
        Check(engine.GetRuntimeActorByUID(1) != nullptr, "scene reload recreates authored UID");
        RunLua(state, R"(
            local replacement = Actor.Find("target")
            assert(replacement:IsValid())
            assert(cached_target:GetUID() == replacement:GetUID())
            assert(cached_target ~= replacement)
            assert(not cached_target:IsValid())
            assert(cached_target:GetName() == "")
            assert(cached_target:GetComponent("Rigidbody") == nil)
            assert(not cached_body.actor:IsValid())
            assert(not cached_hit.actor:IsValid())
            assert(not cached_collision.other:IsValid())
            Actor.Destroy(cached_target)
            Scene.DontDestroy(cached_target)
            assert(replacement:IsValid())
            local watcher = Actor.Find("watcher")
            assert(replacement:SetParent(watcher))
            assert(watcher:GetChildren()[1] == replacement)
        )");
        Check(!engine.GetRuntimeActorByUID(1)->dont_destroy_on_scene_load,
              "stale DontDestroy must not mutate replacement actor");
    }
    Check(!old_native_handle.IsValid(), "handle must be safe after runtime shutdown");
    Engine reset;
    Check(ComponentManager::GetComponentByKey(2, "logic")["top_level_frame"].cast<int>().value() == 0,
          "new runtime must reset clock before executing top-level Lua");
    reset.InitializeHeadless();
    reset.StepSimulation();
    Check(!old_native_handle.IsValid(), "runtime reset must not resurrect old generations");
}

void TestRemovedComponentCannotRestart() {
    Engine engine;
    engine.InitializeHeadless();
    engine.StepSimulation();
    auto probe = ComponentManager::GetComponentByKey(2, "logic");
    lua_State *state = probe.state();
    RunLua(state, R"(
        owner = Actor.Find("target")
        removed_body = owner:GetComponent("Rigidbody")
        removed_particles = owner:GetComponent("ParticleSystem")
        owner:RemoveComponent(removed_body)
        owner:RemoveComponent(removed_particles)
        assert(owner:IsValid())
    )");
    engine.StepSimulation();
    RunLua(state, R"(
        assert(removed_body.actor:IsValid())
        assert(removed_particles.actor:IsValid())
        removed_body:OnStart()
        removed_body:SetPosition(Vector2(-5, 0))
        assert(Physics.Raycast(Vector2(-7, 0), Vector2(1, 0), 4) == nil)
        removed_particles:OnStart()
        removed_particles:OnUpdate()
        removed_particles:Burst()
    )");
    Check(!ParticleManager::IsEmitterAlive(0), "removed component must not recreate emitter");

    auto replacement_body = ComponentManager::AddComponent(1, "Rigidbody");
    auto replacement_particles = ComponentManager::AddComponent(1, "ParticleSystem");
    const auto body_key = replacement_body["key"].cast<std::string>().value();
    const auto particles_key = replacement_particles["key"].cast<std::string>().value();
    Check(ComponentManager::RenameComponentKey(1, body_key, "body"), "reuse removed body key");
    Check(ComponentManager::RenameComponentKey(1, particles_key, "particles"),
          "reuse removed emitter key");
    engine.StepSimulation();
    Check(ParticleManager::IsEmitterAlive(0), "replacement emitter must run normally");
    RunLua(state, R"(
        assert(owner:GetComponentByKey("body") ~= removed_body)
        assert(owner:GetComponentByKey("particles") ~= removed_particles)
        removed_body:OnStart()
        removed_body:SetPosition(Vector2(-5, 0))
        assert(Physics.Raycast(Vector2(-7, 0), Vector2(1, 0), 4) == nil)
        removed_particles:OnStart()
        removed_particles:Play()
        removed_particles:OnUpdate()
        removed_particles:Burst()
    )");
    Check(!ParticleManager::IsEmitterAlive(1), "same-key replacement must not validate old instance");
    Check(ComponentManager::GetRuntimeErrorCount() == 0, "component lifetime Lua checks must succeed");
}

void TestConstructorRecovery(const Fixture &fixture) {
    std::ofstream(fixture.path / "game.config") << R"({"initial_scene":"broken"})";
    std::ofstream(fixture.path / "scenes/broken.scene") << "{ invalid json";
    bool rejected = false;
    try {
        Engine broken;
    } catch (const std::exception &) {
        rejected = true;
    }
    Check(rejected, "invalid scene must reject construction");
    std::ofstream(fixture.path / "game.config") << R"({"initial_scene":"lifetime"})";
    Engine recovered;
    recovered.InitializeHeadless();
    Check(FrameClock::GetFrameNumber() == 0, "retry starts a clean clock");
    recovered.StepSimulation();
    Check(recovered.GetActorCount() == 4, "constructor retry builds a clean real world");
    Check(ComponentManager::GetRuntimeErrorCount() == 0,
          "failed construction must not retain invalid Lua or physics state");
}
} // namespace

int main() {
    try {
        Fixture fixture;
        TestStableSurvivors();
        TestDeadAndReusedHandles();
        TestRemovedComponentCannotRestart();
        TestConstructorRecovery(fixture);
        std::cout << "PASS actor lifetime: live cached Lua refs, raycast, reclamation, constructor recovery\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL actor lifetime: " << error.what() << '\n';
        return 1;
    }
}
