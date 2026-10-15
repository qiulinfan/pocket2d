#include "core/Engine.h"
#include "core/TaskExecutor.h"
#include "particles/ParticleManager.h"
#include "particles/ParticleSystem.h"
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
template <typename F> void Reject(F &&function) {
    bool rejected = false;
    try { function(); } catch (const std::exception &) { rejected = true; }
    Check(rejected, "invalid particle operation was accepted");
}
void Lua(lua_State *state, const char *code) {
    const int top = lua_gettop(state);
    const int status = luaL_dostring(state, code);
    const std::string error = status == LUA_OK ? "" : lua_tostring(state, -1);
    lua_settop(state, top);
    if (status != LUA_OK) throw std::runtime_error(error);
}
void ConfigurationAndCapacity() {
    ParticleManager::Clear();
    ParticleSystem system;
    system.ValidateConfiguration();
    system.duration_frames = std::numeric_limits<int>::max();
    system.frames_between_bursts = 2;
    Reject([&] { system.OnStart(); }); // Never evaluate overflowing int ceil or reserve.
    system.duration_frames = 1; system.frames_between_bursts = 1;
    system.burst_quantity = 1000001;
    Reject([&] { system.OnStart(); });
    system.burst_quantity = 1;
    system.start_speed_min = system.start_speed_max = 1;
    system.drag_factor = 1e38f; system.duration_frames = 3;
    Reject([&] { system.ValidateConfiguration(); });
    system.drag_factor = 1; system.gravity_scale_x = 1e38f;
    Reject([&] { system.ValidateConfiguration(); });
    system.gravity_scale_x = 0; system.duration_frames = 100000;
    system.drag_factor = 0.99f; system.angular_drag_factor = 0.99f;
    system.ValidateConfiguration(); // Existing long-lived native benchmark remains valid.
    system.duration_frames = 1; system.drag_factor = 1;
    system.OnStart(); system.Burst();
    Check(ParticleManager::GetActiveParticleCount(0) == 1, "first burst missing");
    system.burst_quantity = 1000000;
    Reject([&] { system.Burst(); }); // Reject remaining-capacity deficit without allocating a million slots.
    Check(ParticleManager::GetActiveParticleCount(0) == 1, "failed burst changed live count");
    system.OnUpdate(); // Automatic cadence drops the entire over-budget burst.
    Check(ParticleManager::GetActiveParticleCount(0) == 1, "automatic cap did not drop whole burst");
    system.burst_quantity = 1; system.Burst();
    ParticleSpawnRequest after_failure;
    Check(ParticleManager::GetParticleState(0, 1, after_failure), "second burst missing");
    system.OnDestroy();
    ParticleSystem reference;
    reference.duration_frames = 1;
    reference.start_speed_min = reference.start_speed_max = 1;
    reference.angular_drag_factor = 0.99f;
    reference.OnStart(); reference.Burst(); reference.Burst();
    ParticleSpawnRequest expected;
    Check(ParticleManager::GetParticleState(0, 1, expected), "reference burst missing");
    Check(after_failure.x == expected.x && after_failure.y == expected.y &&
          after_failure.velocity_x == expected.velocity_x && after_failure.velocity_y == expected.velocity_y,
          "rejected burst consumed random state");
    reference.OnDestroy(); ParticleManager::Clear();
}
void ColorCompatibility() {
    ParticleManager::Clear();
    ParticleSystem system;
    system.duration_frames = 4;
    system.start_color_r = std::numeric_limits<int>::min();
    system.start_color_g = std::numeric_limits<int>::max();
    system.start_color_b = 255;
    system.start_color_a = 256;
    system.end_color_r = -2;
    system.end_color_g = 256;
    system.end_color_b = 0;
    system.end_color_a = -2;
    system.OnStart(); system.Burst();
    ParticleSpawnRequest particle;
    Check(ParticleManager::GetParticleState(0, 0, particle) &&
          particle.color_r == 0 && particle.color_g == 255 && particle.color_a == 255,
          "start colors no longer clamp to byte range");
    ParticleManager::Update(1.0f / 60);
    ParticleManager::Update(1.0f / 60);
    Check(ParticleManager::GetParticleState(0, 0, particle) &&
          particle.color_r == 0 && particle.color_g == 255 && particle.color_a == 255 &&
          particle.color_b == 191,
          "invalid end colors must disable only their channels; valid channels still interpolate");
    system.OnDestroy(); ParticleManager::Clear();
}
void ManagerIngressAndLifetime() {
    ParticleManager::Clear();
    const int emitter = ParticleManager::CreateEmitter();
    ParticleEmitterConfig config; config.duration_frames = 2;
    ParticleManager::ConfigureEmitter(emitter, config);
    ParticleSpawnRequest request; request.velocity_x = 1;
    ParticleManager::SpawnParticle(emitter, request);
    auto invalid = request; invalid.x = std::numeric_limits<float>::infinity();
    Reject([&] { ParticleManager::SpawnParticle(emitter, invalid); });
    Reject([&] { ParticleManager::RequireSpawnCapacity(emitter, ParticleManager::kMaxParticlesPerEmitter); });
    auto invalid_config = config; invalid_config.reserve_hint = ParticleManager::kMaxParticlesPerEmitter + 1;
    Reject([&] { ParticleManager::ConfigureEmitter(emitter, invalid_config); });
    invalid_config = config; invalid_config.drag_factor = 1e38f;
    Reject([&] { ParticleManager::ConfigureEmitter(emitter, invalid_config); });
    ParticleManager::Update(0); // Integration remains per tick, independent of dt.
    ParticleSpawnRequest current;
    Check(ParticleManager::GetParticleState(emitter, 0, current) && current.x == 1, "failed configuration changed integration");
    ParticleManager::Update(1.0f/60);
    Check(ParticleManager::GetParticleState(emitter, 0, current) && current.x == 2, "lifetime ended early");
    ParticleManager::Update(1.0f/60);
    Check(ParticleManager::GetActiveParticleCount(emitter) == 0, "bounded age did not expire");
    ParticleManager::SpawnParticle(emitter, request);
    ParticleManager::Update(1.0f/60);
    Check(ParticleManager::GetParticleState(emitter, 0, current) && current.x == 1, "recycled age not reset");
    ParticleManager::Clear();
}
void ParallelParity() {
    ParticleSpawnRequest samples[2];
    TaskExecutor executor;
    for (int mode = 0; mode < 2; ++mode) {
        ParticleManager::Clear(); const int emitter = ParticleManager::CreateEmitter();
        ParticleEmitterConfig config; config.duration_frames = 20; config.reserve_hint = 8192;
        config.drag_factor = 0.99f; config.gravity_scale_x = 0.01f;
        ParticleManager::ConfigureEmitter(emitter, config);
        ParticleSpawnRequest request; request.velocity_x = 1; request.angular_velocity = 0.3f;
        for (int i = 0; i < 8192; ++i) ParticleManager::SpawnParticle(emitter, request);
        for (int i = 0; i < 4; ++i) ParticleManager::Update(1.0f/60, mode ? &executor : nullptr);
        Check(ParticleManager::GetParticleState(emitter, 7000, samples[mode]), "parallel particle missing");
    }
    Check(samples[0].x == samples[1].x && samples[0].velocity_x == samples[1].velocity_x &&
          samples[0].rotation == samples[1].rotation, "serial/parallel particle integration differs");
    ParticleManager::Clear();
}
struct Fixture {
    std::filesystem::path previous = ResourcePath::ResourcesRootPath();
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("pocket-particle-validation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() {
        std::filesystem::create_directories(root / "scenes");
        std::filesystem::create_directories(root / "component_types");
        std::ofstream(root / "game.config") << R"({"initial_scene":"test"})";
        std::ofstream(root / "scenes/test.scene") << R"({"actors":[{"uid":1,"name":"owner","components":{"particle":{"type":"ParticleSystem"}}}]})";
        ResourcePath::SetResourcesRootPath(root);
    }
    ~Fixture() { ResourcePath::SetResourcesRootPath(previous); std::error_code error; std::filesystem::remove_all(root, error); }
};
void LuaWritesAndStaleBinding() {
    Fixture fixture; Engine engine; engine.InitializeHeadless();
    auto instance = ComponentManager::GetComponentByKey(1, "particle"); auto *state = instance.state();
    Lua(state, R"(
      p=Actor.Find('owner'):GetComponent('ParticleSystem')
      local function bad(field,value)
        local old=p[field];assert(not pcall(function()p[field]=value end));assert(p[field]==old)
      end
      bad('x',1e309);bad('y',0/0);bad('start_speed_min',1e100);bad('rotation','2')
      bad('burst_quantity',1.5);bad('duration_frames',2147483647);bad('frames_between_bursts',0)
      bad('sorting_order',1e309);bad('start_color_r',1.5)
      p.end_scale=0/0;assert(p.end_scale~=p.end_scale)
      p.start_speed_min=2;p.start_speed_max=3 -- Allow valid multi-property authoring sequence.
      p.duration_frames=1;p:OnStart();p:Burst()
    )");
    Check(ParticleManager::GetActiveParticleCount(0) == 1, "valid Lua burst failed");
    auto *system = instance.cast<ParticleSystem *>().value();
    ComponentManager::RemoveComponent(1, instance); ComponentManager::FinalizeFrameMutations();
    Check(ParticleManager::GetActiveParticleCount(0) == 0, "removed emitter retained particles");
    system->OnStart(); system->Play(); system->Burst();
    Check(!ParticleManager::IsEmitterAlive(0), "stale component resurrected emitter");
}
}
int main() {
    try {
        ConfigurationAndCapacity(); ColorCompatibility(); ManagerIngressAndLifetime(); ParallelParity(); LuaWritesAndStaleBinding();
        std::cout << "particle_validation_test: passed\n"; return 0;
    } catch (const std::exception &error) {
        std::cerr << "particle_validation_test: " << error.what() << '\n'; return 1;
    }
}
