#include "scripting/LuaBoundary.h"
#include "particles/ParticleSystem.h"
#include "particles/ParticleManager.h"
#include "glm/glm.hpp"
#include "scripting/ComponentManager.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {

bool HasEndScaleValue(float value) {
    return !std::isnan(value);
}

bool HasValidColorChannel(int value) {
    return value >= 0 && value <= 255;
}

ParticleEmitterConfig BuildEmitterConfig(const ParticleSystem &system) {
    ParticleEmitterConfig config;
    constexpr int cap = static_cast<int>(ParticleManager::kMaxParticlesPerEmitter);
    if (system.duration_frames < 1 || system.duration_frames > cap ||
        system.frames_between_bursts < 1 || system.frames_between_bursts > cap ||
        system.burst_quantity < 1 || system.burst_quantity > cap)
        throw std::runtime_error("ParticleSystem duration/interval/burst must be 1..1000000");
    config.duration_frames = system.duration_frames;
    const std::uint64_t bursts = (static_cast<std::uint64_t>(system.duration_frames) +
        system.frames_between_bursts - 1) / system.frames_between_bursts;
    config.reserve_hint = static_cast<size_t>(bursts * system.burst_quantity);
    if (config.reserve_hint > ParticleManager::kMaxParticlesPerEmitter)
        throw std::runtime_error("ParticleSystem live/reserve capacity exceeds 1000000");
    config.gravity_scale_x = system.gravity_scale_x;
    config.gravity_scale_y = system.gravity_scale_y;
    config.drag_factor = system.drag_factor;
    config.angular_drag_factor = system.angular_drag_factor;
    config.has_end_scale = HasEndScaleValue(system.end_scale);
    config.end_scale = config.has_end_scale ? system.end_scale : 1.0f;
    config.has_end_color_r = HasValidColorChannel(system.end_color_r);
    config.has_end_color_g = HasValidColorChannel(system.end_color_g);
    config.has_end_color_b = HasValidColorChannel(system.end_color_b);
    config.has_end_color_a = HasValidColorChannel(system.end_color_a);
    config.end_color_r = config.has_end_color_r ? system.end_color_r : 255;
    config.end_color_g = config.has_end_color_g ? system.end_color_g : 255;
    config.end_color_b = config.has_end_color_b ? system.end_color_b : 255;
    config.end_color_a = config.has_end_color_a ? system.end_color_a : 255;
    return config;
}

} // namespace

void ParticleSystem::ValidateConfiguration() const {
    const auto config = BuildEmitterConfig(*this);
    ParticleManager::ValidateEmitterConfig(config);
    for (const auto pair : {std::pair{start_scale_min, start_scale_max},
            std::pair{start_speed_min, start_speed_max}, std::pair{rotation_min, rotation_max},
            std::pair{rotation_speed_min, rotation_speed_max}, std::pair{emit_angle_min, emit_angle_max},
            std::pair{emit_radius_min, emit_radius_max}}) {
        if (!std::isfinite(pair.first) || !std::isfinite(pair.second) || pair.first > pair.second ||
            static_cast<double>(pair.second) - pair.first > std::numeric_limits<float>::max())
            throw std::runtime_error("ParticleSystem distribution range is nonfinite, reversed or overflows float");
    }
    if (emit_radius_min < 0) throw std::runtime_error("ParticleSystem emit radius must be nonnegative");
    const auto magnitude = [](float a, float b) { return std::max(std::abs(a), std::abs(b)); };
    const auto position_bound = [this](float origin) {
        const double value = std::abs(static_cast<double>(origin)) + emit_radius_max;
        if (!std::isfinite(value) || value > static_cast<double>(std::numeric_limits<float>::max()) / 256)
            throw std::runtime_error("ParticleSystem emission position exceeds finite range");
        return static_cast<float>(value);
    };
    ParticleSpawnRequest worst;
    worst.x = position_bound(x); worst.y = position_bound(y);
    worst.velocity_x = worst.velocity_y = magnitude(start_speed_min, start_speed_max);
    worst.scale = magnitude(start_scale_min, start_scale_max);
    worst.rotation = magnitude(rotation_min, rotation_max);
    worst.angular_velocity = magnitude(rotation_speed_min, rotation_speed_max);
    ParticleManager::ValidateParticle(config, worst);
}

bool ParticleSystem::HasLiveRuntimeBinding() const {
    if (actor_handle.generation == 0) return true;
    if (!actor_handle.IsValid()) return false;
    return ComponentManager::FindBuiltinComponent(actor_handle.uid, key) == this;
}

// Initialize the emitter on first use and sync its enabled state into runtime.
void ParticleSystem::OnStart() {
    if (!HasLiveRuntimeBinding()) return;
    ValidateConfiguration();
    InitializeIfNeeded();
    SyncRuntimeState();
}

// Advance automatic emission cadence once per simulation frame.
void ParticleSystem::OnUpdate() {
    if (!HasLiveRuntimeBinding()) return;
    InitializeIfNeeded();
    if (frames_between_bursts < 1 || frames_between_bursts > static_cast<int>(ParticleManager::kMaxParticlesPerEmitter))
        throw std::runtime_error("ParticleSystem burst interval must be 1..1000000");
    if (emission_allowed && local_frame_number % static_cast<std::uint64_t>(frames_between_bursts) == 0) {
        ValidateConfiguration();
        // Automatic cadence drops a whole burst at the live cap. Manual Burst rejects.
        if (static_cast<size_t>(burst_quantity) <= ParticleManager::kMaxParticlesPerEmitter -
            ParticleManager::GetActiveParticleCount(emitter_id)) EmitBurst(burst_quantity);
    }
    ++local_frame_number;
}

// Release the backing emitter and reset transient runtime state.
void ParticleSystem::OnDestroy() {
    ParticleManager::DestroyEmitter(emitter_id);
    emitter_id = -1;
    local_frame_number = 0;
    emission_allowed = true;
    initialized = false;
}

// stop / resume automatic emission
void ParticleSystem::Stop() {
    emission_allowed = false;
}

// stop / resume automatic emission
void ParticleSystem::Play() {
    if (!HasLiveRuntimeBinding()) return;
    emission_allowed = true;
}

// Effect: 立即发出一波粒子, 无视 burst 间隔和当前帧数. 
// 可以在一些事件驱动的场景中使用, 比如玩家攻击时发出一波粒子.
void ParticleSystem::Burst() {
    if (!HasLiveRuntimeBinding()) return;
    ValidateConfiguration();
    if (initialized) ParticleManager::RequireSpawnCapacity(emitter_id, static_cast<size_t>(burst_quantity));
    InitializeIfNeeded();
    EmitBurst(burst_quantity);
    SyncRuntimeState();
}

// sync current enabled state into ParticleManager
// Effect: 将当前的 enabled 状态同步到 ParticleManager 中对应 emitter 的 runtime_enabled.
// 会影响 Update() 中粒子的更新和 QueueRenderBatches()/RenderEmitterBatch() 中的渲染,
// 但不会影响粒子的生成.
void ParticleSystem::SyncRuntimeState() {
    if (!initialized) return;
    ParticleManager::SetEmitterRuntimeEnabled(emitter_id, enabled);
}

// lazy setup of distributions and backing emitter
void ParticleSystem::InitializeIfNeeded() {
    if (initialized) return;

    ValidateConfiguration();
    // Configuration/distributions are snapshotted on initialization, as before.
    // Origin, colors, image, sorting and burst cadence remain live fields.
    const int candidate = ParticleManager::CreateEmitter();
    try {
        ParticleManager::ConfigureEmitter(candidate, BuildEmitterConfig(*this));
    } catch (...) {
        ParticleManager::DestroyEmitter(candidate);
        throw;
    }
    emit_angle_distribution = RandomEngine(emit_angle_min, emit_angle_max, 298);
    emit_radius_distribution = RandomEngine(emit_radius_min, emit_radius_max, 404);
    rotation_distribution = RandomEngine(rotation_min, rotation_max, 440);
    speed_distribution = RandomEngine(start_speed_min, start_speed_max, 498);
    rotation_speed_distribution = RandomEngine(rotation_speed_min, rotation_speed_max, 305);
    scale_distribution = RandomEngine(start_scale_min, start_scale_max, 494);
    emitter_id = candidate;
    ParticleManager::SetEmitterRuntimeEnabled(emitter_id, enabled);
    local_frame_number = 0;
    initialized = true;
}

// Emit one burst of particles using the current randomized distribution state.
void ParticleSystem::EmitBurst(int particle_count) {
    ValidateConfiguration();
    ParticleManager::RequireSpawnCapacity(emitter_id, static_cast<size_t>(particle_count));
    // Stage an entire burst against the initialized emitter's configuration.
    // Failed validation never consumes RNG or publishes a partial burst.
    auto angles = emit_angle_distribution;
    auto radii = emit_radius_distribution;
    auto rotations = rotation_distribution;
    auto speeds = speed_distribution;
    auto angular_speeds = rotation_speed_distribution;
    auto scales = scale_distribution;
    std::vector<ParticleSpawnRequest> pending;
    pending.reserve(static_cast<size_t>(particle_count));
    ParticleSpawnRequest request;
    request.color_r = ClampByte(start_color_r);
    request.color_g = ClampByte(start_color_g);
    request.color_b = ClampByte(start_color_b);
    request.color_a = ClampByte(start_color_a);
    request.sorting_order = sorting_order;

    for (int i = 0; i < particle_count; ++i) {
        const float angle_degrees = angles.Sample();
        const float angle_radians = glm::radians(angle_degrees);
        const float radius = radii.Sample();
        const float cos_angle = glm::cos(angle_radians);
        const float sin_angle = glm::sin(angle_radians);
        const float speed = speeds.Sample();

        request.x = x + cos_angle * radius;
        request.y = y + sin_angle * radius;
        request.velocity_x = cos_angle * speed;
        request.velocity_y = sin_angle * speed;
        request.scale = scales.Sample();
        request.rotation = rotations.Sample();
        request.angular_velocity = angular_speeds.Sample();

        // spawn particle: 启用 ECS 接口, 交给 ParticleManager 处理 emitter 内粒子数据的存储和 render request 的生成
        ParticleManager::ValidateSpawnRequest(emitter_id, request);
        pending.push_back(request);
    }
    const int image_id = ParticleManager::ResolveImageId(image);
    ParticleManager::ReserveSpawnCapacity(emitter_id, pending.size());
    for (auto &particle : pending) {
        particle.image_id = image_id;
        ParticleManager::SpawnParticle(emitter_id, particle);
    }
    emit_angle_distribution = angles;
    emit_radius_distribution = radii;
    rotation_distribution = rotations;
    speed_distribution = speeds;
    rotation_speed_distribution = angular_speeds;
    scale_distribution = scales;
}

// Clamp RGBA channel values into the byte range expected by the renderer.
int ParticleSystem::ClampByte(int value) {
    if (value < 0) return 0;
    if (value > 255) return 255;
    return value;
}
