#include "RegistrationDetail.h"
#include "scripting/LuaBoundary.h"
#include "particles/ParticleSystem.h"
#include "particles/ParticleManager.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace {
using APIRegistrationDetail::g_lua_state;
template <typename Component> ActorHandle GetComponentActor(const Component *component) { return component->actor_handle; }
template <typename Component> void SetComponentActor(Component *component, ActorHandle handle) {
    component->actor_handle = handle; component->actor = handle.Resolve();
}
template <typename T, T ParticleSystem::*Member>
T GetValue(const ParticleSystem *system) { return system->*Member; }
template <typename T, T ParticleSystem::*Member>
void SetValue(ParticleSystem *system, luabridge::LuaRef input) {
    if (input.type() != LUA_TNUMBER) throw std::runtime_error("ParticleSystem numeric property requires a number");
    const double value = LuaBoundary::Cast<double>(input);
    if constexpr (std::is_same_v<T, int>) {
        if (!std::isfinite(value) || std::trunc(value) != value ||
            value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
            throw std::runtime_error("ParticleSystem integer property must fit an integer");
        if constexpr (Member == &ParticleSystem::duration_frames || Member == &ParticleSystem::frames_between_bursts || Member == &ParticleSystem::burst_quantity) {
            if (value < 1 || value > ParticleManager::kMaxParticlesPerEmitter) throw std::runtime_error("ParticleSystem duration/interval/burst must be 1..1000000");
        }
    } else {
        if constexpr (Member == &ParticleSystem::end_scale) {
            if (std::isnan(value)) { system->*Member = std::numeric_limits<float>::quiet_NaN(); return; }
        }
        if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
            throw std::runtime_error("ParticleSystem float property must fit a finite float");
    }
    system->*Member = static_cast<T>(value);
}
}

void APIRegistrationDetail::RegisterParticleSystemAPI() {
    luabridge::getGlobalNamespace(g_lua_state)
        .beginClass<ParticleSystem>("ParticleSystem")
        .addConstructor<void (*)()>()
        .addProperty("actor", &GetComponentActor<ParticleSystem>, &SetComponentActor<ParticleSystem>)
        .addPropertyReadWrite("key", &ParticleSystem::key)
        .addPropertyReadWrite("enabled", &ParticleSystem::enabled)
        .addProperty("x", &GetValue<float, &ParticleSystem::x>, &SetValue<float, &ParticleSystem::x>)
        .addProperty("y", &GetValue<float, &ParticleSystem::y>, &SetValue<float, &ParticleSystem::y>)
        .addProperty("frames_between_bursts", &GetValue<int, &ParticleSystem::frames_between_bursts>, &SetValue<int, &ParticleSystem::frames_between_bursts>)
        .addProperty("burst_quantity", &GetValue<int, &ParticleSystem::burst_quantity>, &SetValue<int, &ParticleSystem::burst_quantity>)
        .addProperty("duration_frames", &GetValue<int, &ParticleSystem::duration_frames>, &SetValue<int, &ParticleSystem::duration_frames>)
        .addProperty("start_scale_min", &GetValue<float, &ParticleSystem::start_scale_min>, &SetValue<float, &ParticleSystem::start_scale_min>)
        .addProperty("start_scale_max", &GetValue<float, &ParticleSystem::start_scale_max>, &SetValue<float, &ParticleSystem::start_scale_max>)
        .addProperty("start_speed_min", &GetValue<float, &ParticleSystem::start_speed_min>, &SetValue<float, &ParticleSystem::start_speed_min>)
        .addProperty("start_speed_max", &GetValue<float, &ParticleSystem::start_speed_max>, &SetValue<float, &ParticleSystem::start_speed_max>)
        .addProperty("rotation_min", &GetValue<float, &ParticleSystem::rotation_min>, &SetValue<float, &ParticleSystem::rotation_min>)
        .addProperty("rotation_max", &GetValue<float, &ParticleSystem::rotation_max>, &SetValue<float, &ParticleSystem::rotation_max>)
        .addProperty("rotation_speed_min", &GetValue<float, &ParticleSystem::rotation_speed_min>, &SetValue<float, &ParticleSystem::rotation_speed_min>)
        .addProperty("rotation_speed_max", &GetValue<float, &ParticleSystem::rotation_speed_max>, &SetValue<float, &ParticleSystem::rotation_speed_max>)
        .addProperty("start_color_r", &GetValue<int, &ParticleSystem::start_color_r>, &SetValue<int, &ParticleSystem::start_color_r>)
        .addProperty("start_color_g", &GetValue<int, &ParticleSystem::start_color_g>, &SetValue<int, &ParticleSystem::start_color_g>)
        .addProperty("start_color_b", &GetValue<int, &ParticleSystem::start_color_b>, &SetValue<int, &ParticleSystem::start_color_b>)
        .addProperty("start_color_a", &GetValue<int, &ParticleSystem::start_color_a>, &SetValue<int, &ParticleSystem::start_color_a>)
        .addProperty("end_color_r", &GetValue<int, &ParticleSystem::end_color_r>, &SetValue<int, &ParticleSystem::end_color_r>)
        .addProperty("end_color_g", &GetValue<int, &ParticleSystem::end_color_g>, &SetValue<int, &ParticleSystem::end_color_g>)
        .addProperty("end_color_b", &GetValue<int, &ParticleSystem::end_color_b>, &SetValue<int, &ParticleSystem::end_color_b>)
        .addProperty("end_color_a", &GetValue<int, &ParticleSystem::end_color_a>, &SetValue<int, &ParticleSystem::end_color_a>)
        .addProperty("emit_radius_min", &GetValue<float, &ParticleSystem::emit_radius_min>, &SetValue<float, &ParticleSystem::emit_radius_min>)
        .addProperty("emit_radius_max", &GetValue<float, &ParticleSystem::emit_radius_max>, &SetValue<float, &ParticleSystem::emit_radius_max>)
        .addProperty("emit_angle_min", &GetValue<float, &ParticleSystem::emit_angle_min>, &SetValue<float, &ParticleSystem::emit_angle_min>)
        .addProperty("emit_angle_max", &GetValue<float, &ParticleSystem::emit_angle_max>, &SetValue<float, &ParticleSystem::emit_angle_max>)
        .addProperty("gravity_scale_x", &GetValue<float, &ParticleSystem::gravity_scale_x>, &SetValue<float, &ParticleSystem::gravity_scale_x>)
        .addProperty("gravity_scale_y", &GetValue<float, &ParticleSystem::gravity_scale_y>, &SetValue<float, &ParticleSystem::gravity_scale_y>)
        .addProperty("drag_factor", &GetValue<float, &ParticleSystem::drag_factor>, &SetValue<float, &ParticleSystem::drag_factor>)
        .addProperty("angular_drag_factor", &GetValue<float, &ParticleSystem::angular_drag_factor>, &SetValue<float, &ParticleSystem::angular_drag_factor>)
        .addProperty("end_scale", &GetValue<float, &ParticleSystem::end_scale>, &SetValue<float, &ParticleSystem::end_scale>)
        .addPropertyReadWrite("image", &ParticleSystem::image)
        .addProperty("sorting_order", &GetValue<int, &ParticleSystem::sorting_order>, &SetValue<int, &ParticleSystem::sorting_order>)
        .addFunction("OnStart", &ParticleSystem::OnStart)
        .addFunction("OnUpdate", &ParticleSystem::OnUpdate)
        .addFunction("OnDestroy", &ParticleSystem::OnDestroy)
        .addFunction("Stop", &ParticleSystem::Stop)
        .addFunction("Play", &ParticleSystem::Play)
        .addFunction("Burst", &ParticleSystem::Burst)
        .endClass();
}
