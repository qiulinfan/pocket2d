#include "core/TaskExecutor.h"
#include "particles/ParticleManager.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
struct Result {
    double median_us = 0;
    double p95_us = 0;
    ParticleSpawnRequest sample;
};

Result Measure(size_t count, TaskExecutor *executor) {
    ParticleManager::Clear();
    const int emitter = ParticleManager::CreateEmitter();
    ParticleEmitterConfig config;
    config.duration_frames = 100000;
    config.reserve_hint = count;
    config.gravity_scale_x = 0.001f;
    config.gravity_scale_y = -0.001f;
    config.drag_factor = 0.99f;
    config.angular_drag_factor = 0.99f;
    config.has_end_scale = true;
    config.end_scale = 0.1f;
    config.has_end_color_r = config.has_end_color_g = true;
    config.has_end_color_b = config.has_end_color_a = true;
    config.end_color_r = config.end_color_g = config.end_color_b = 0;
    config.end_color_a = 0;
    ParticleManager::ConfigureEmitter(emitter, config);
    for (size_t i = 0; i < count; ++i) {
        ParticleSpawnRequest particle;
        particle.x = static_cast<float>(i % 100);
        particle.velocity_x = 0.1f;
        particle.velocity_y = -0.2f;
        particle.angular_velocity = 0.3f;
        ParticleManager::SpawnParticle(emitter, particle);
    }
    constexpr int warmup = 20;
    constexpr int samples = 120;
    for (int i = 0; i < warmup; ++i) ParticleManager::Update(1.0f/60.0f, executor);
    std::vector<double> times;
    times.reserve(samples);
    for (int i = 0; i < samples; ++i) {
        const auto start = std::chrono::steady_clock::now();
        ParticleManager::Update(1.0f/60.0f, executor);
        const auto stop = std::chrono::steady_clock::now();
        times.push_back(std::chrono::duration<double, std::micro>(stop-start).count());
    }
    if (ParticleManager::GetActiveParticleCount(emitter) != count)
        throw std::runtime_error("benchmark lost particles");
    std::sort(times.begin(), times.end());
    Result result;
    result.median_us = times[times.size()/2];
    result.p95_us = times[times.size()*95/100];
    if (!ParticleManager::GetParticleState(emitter, count/2, result.sample))
        throw std::runtime_error("benchmark sample missing");
    ParticleManager::Clear();
    return result;
}
}

int main() {
    try {
        TaskExecutor executor;
        std::cout << std::fixed << std::setprecision(3);
        for (size_t count : {1024, 8192, 16384, 65536, 262144}) {
            const auto serial = Measure(count, nullptr);
            const auto parallel = Measure(count, &executor);
            if (serial.sample.x != parallel.sample.x ||
                serial.sample.y != parallel.sample.y ||
                serial.sample.rotation != parallel.sample.rotation ||
                serial.sample.color_a != parallel.sample.color_a)
                throw std::runtime_error("parallel sample differs from serial");
            std::cout << "{\"particles\":" << count
                      << ",\"workers\":" << executor.WorkerCount()
                      << ",\"serial_median_us\":" << serial.median_us
                      << ",\"serial_p95_us\":" << serial.p95_us
                      << ",\"parallel_median_us\":" << parallel.median_us
                      << ",\"parallel_p95_us\":" << parallel.p95_us
                      << ",\"speedup\":" << serial.median_us / parallel.median_us
                      << "}\n";
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "particle_benchmark: " << error.what() << '\n';
        return 1;
    }
}
