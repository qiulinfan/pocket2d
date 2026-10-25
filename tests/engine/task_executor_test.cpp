#include "core/TaskExecutor.h"
#include "particles/ParticleManager.h"
#include <atomic>
#include <array>
#include <barrier>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void Check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

void TestExecutor() {
    TaskExecutor executor(3);
    std::vector<int> visits(10003, 0);
    executor.ParallelFor(visits.size(), 127, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) ++visits[i];
    });
    for (int count : visits) Check(count == 1, "range omission or overlap");

    // Four blocked first chunks prove actual simultaneous worker participation.
    std::barrier gate(4);
    std::mutex ids_mutex;
    std::set<std::thread::id> ids;
    executor.ParallelFor(20, 1, [&](size_t begin, size_t) {
        { std::lock_guard lock(ids_mutex); ids.insert(std::this_thread::get_id()); }
        if (begin < 4) gate.arrive_and_wait();
    });
    Check(ids.size() == 4, "worker pool did not execute in parallel");

    std::atomic<int> nested{0};
    executor.ParallelFor(12, 1, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i)
            executor.ParallelFor(3, 1, [&](size_t a, size_t b) { nested += static_cast<int>(b-a); });
    });
    Check(nested == 36, "nested work must complete without self-deadlock");

    bool failed = false;
    try {
        executor.ParallelFor(100, 1, [](size_t begin, size_t) {
            if (begin == 0) throw std::runtime_error("worker failure");
        });
    } catch (const std::runtime_error &) { failed = true; }
    Check(failed, "callback exception must return to submitting thread");
    std::atomic<int> completed{0};
    auto submit = [&] {
        for (int j = 0; j < 30; ++j)
            executor.ParallelFor(100, 7, [&](size_t a, size_t b) { completed += static_cast<int>(b-a); });
    };
    { std::jthread a(submit), b(submit); }
    Check(completed == 6000, "pool must recover after exception and serialize submissions");
    const auto caller = std::this_thread::get_id();
    executor.ParallelFor(2, 10, [&](size_t a, size_t b) {
        Check(a == 0 && b == 2 && std::this_thread::get_id() == caller,
              "small range must execute inline");
    });
    executor.ParallelFor(0, 1, [](size_t, size_t) { throw std::runtime_error("empty range ran"); });
    TaskExecutor serial(0);
    serial.ParallelFor(100, 1, [&](size_t, size_t) {
        Check(std::this_thread::get_id() == caller, "zero-worker executor must be serial");
    });
}

struct ParticleSnapshot {
    bool active = false;
    ParticleSpawnRequest state;
};

std::vector<ParticleSnapshot> RunParticles(TaskExecutor *executor) {
    ParticleManager::Clear();
    const int emitter = ParticleManager::CreateEmitter();
    ParticleEmitterConfig config;
    config.duration_frames = 4;
    config.reserve_hint = 16400;
    config.gravity_scale_x = 0.01f;
    config.gravity_scale_y = -0.02f;
    config.drag_factor = 0.98f;
    config.angular_drag_factor = 0.95f;
    config.has_end_scale = true;
    config.end_scale = 0.1f;
    config.has_end_color_a = true;
    config.end_color_a = 0;
    ParticleManager::ConfigureEmitter(emitter, config);
    for (int i = 0; i < 16384; ++i) {
        ParticleSpawnRequest request;
        request.x = static_cast<float>(i % 17);
        request.y = static_cast<float>(i % 13);
        request.velocity_x = static_cast<float>(i % 5) * 0.1f;
        request.velocity_y = -0.2f;
        request.angular_velocity = 0.3f;
        ParticleManager::SpawnParticle(emitter, request);
    }
    ParticleManager::Update(1.0f/60.0f, executor);
    ParticleSpawnRequest first;
    Check(ParticleManager::GetParticleState(emitter, 0, first), "first particle missing");
    Check(std::abs(first.x - 0.0098f) < 0.000001f, "particle integration changed");
    ParticleManager::SetEmitterRuntimeEnabled(emitter, false);
    ParticleManager::Update(1.0f/60.0f, executor);
    ParticleSpawnRequest paused;
    Check(ParticleManager::GetParticleState(emitter, 0, paused) && paused.x == first.x,
          "disabled emitter must not advance");
    ParticleManager::SetEmitterRuntimeEnabled(emitter, true);
    ParticleManager::Update(1.0f/60.0f, executor);
    std::vector<ParticleSnapshot> snapshots(16384);
    for (size_t i = 0; i < snapshots.size(); ++i)
        snapshots[i].active = ParticleManager::GetParticleState(emitter, i, snapshots[i].state);

    // Older particles expire before the newly emitted ones; recycle in index order.
    ParticleSpawnRequest younger;
    younger.x = 999.0f;
    ParticleManager::SpawnParticle(emitter, younger);
    for (int i = 0; i < 3; ++i) ParticleManager::Update(1.0f/60.0f, executor);
    Check(ParticleManager::GetActiveParticleCount(emitter) == 1, "particle lifetime mismatch");
    ParticleSpawnRequest recycled;
    recycled.x = 123.0f;
    ParticleManager::SpawnParticle(emitter, recycled);
    Check(ParticleManager::GetParticleState(emitter, 0, first) && first.x == 123.0f,
          "free-list reuse order must remain deterministic");
    ParticleManager::DestroyEmitter(emitter);
    Check(ParticleManager::GetActiveParticleCount(emitter) == 0, "destroy must clear particles");
    ParticleManager::Clear();
    return snapshots;
}

struct AsyncProbe {
    std::vector<std::atomic<int>> visits;
    std::array<std::atomic<int>, 8> active_by_worker{};
    std::atomic<bool> overlap{false};
    std::atomic<std::size_t> max_worker{0};
    explicit AsyncProbe(std::size_t count) : visits(count) {}
};

void VisitRange(std::size_t begin, std::size_t end, std::size_t worker, void *context) {
    auto &probe = *static_cast<AsyncProbe *>(context);
    if (probe.active_by_worker[worker].fetch_add(1) != 0) probe.overlap = true;
    std::size_t seen = probe.max_worker.load();
    while (worker > seen && !probe.max_worker.compare_exchange_weak(seen, worker)) {}
    for (std::size_t i = begin; i < end; ++i) probe.visits[i].fetch_add(1);
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    probe.active_by_worker[worker].fetch_sub(1);
}

struct Spinner {
    std::atomic<bool> release{false};
    std::atomic<int> started{0};
};

void SpinUntilReleased(std::size_t, std::size_t, std::size_t, void *context) {
    auto &spinner = *static_cast<Spinner *>(context);
    spinner.started.fetch_add(1);
    while (!spinner.release.load()) std::this_thread::yield();
}

void Release(std::size_t, std::size_t, std::size_t, void *context) {
    static_cast<Spinner *>(context)->release = true;
}

void TestAsync() {
    TaskExecutor executor(3);
    AsyncProbe probe(5000);
    auto *group = executor.Submit(probe.visits.size(), 64, &VisitRange, &probe);
    executor.Wait(group);
    for (const auto &count : probe.visits) Check(count.load() == 1, "async range omission or overlap");
    Check(!probe.overlap, "a worker index ran two ranges at once");
    Check(probe.max_worker <= executor.WorkerCount(), "worker index out of range");
    executor.Wait(executor.Submit(0, 1, &VisitRange, &probe));

    // Box2D's solver shape: workers spin until a task queued after them runs.
    // With every pool worker spinning, the waiter must run the releasing task.
    Spinner spinner;
    std::vector<TaskExecutor::Group *> spinners;
    for (std::size_t i = 0; i < executor.WorkerCount(); ++i)
        spinners.push_back(executor.Submit(1, 1, &SpinUntilReleased, &spinner));
    while (spinner.started.load() < static_cast<int>(executor.WorkerCount())) std::this_thread::yield();
    auto *release = executor.Submit(1, 1, &Release, &spinner);
    executor.Wait(release);
    for (auto *spinning : spinners) executor.Wait(spinning);
    Check(spinner.release.load(), "waiter ran its own queued task");

    // ParallelFor still works while asynchronous groups are pending.
    AsyncProbe pending(2000);
    auto *background = executor.Submit(pending.visits.size(), 16, &VisitRange, &pending);
    std::atomic<int> sum{0};
    executor.ParallelFor(1000, 10, [&](size_t begin, size_t end) { sum += static_cast<int>(end - begin); });
    executor.Wait(background);
    Check(sum == 1000, "ParallelFor alongside async groups");
    for (const auto &count : pending.visits) Check(count.load() == 1, "background group completed");
}

void TestParticleIntegration() {
    TaskExecutor executor(3);
    const auto serial = RunParticles(nullptr);
    const auto parallel = RunParticles(&executor);
    Check(serial.size() == parallel.size(), "snapshot size mismatch");
    for (size_t i = 0; i < serial.size(); ++i) {
        const auto &a = serial[i].state;
        const auto &b = parallel[i].state;
        Check(serial[i].active == parallel[i].active && a.x == b.x && a.y == b.y &&
              a.velocity_x == b.velocity_x && a.velocity_y == b.velocity_y &&
              a.scale == b.scale && a.rotation == b.rotation &&
              a.angular_velocity == b.angular_velocity && a.color_a == b.color_a &&
              a.color_r == b.color_r && a.color_g == b.color_g && a.color_b == b.color_b &&
              a.image_id == b.image_id && a.sorting_order == b.sorting_order,
              "parallel particles differ from serial simulation");
    }
}
}

int main() {
    try {
        TestExecutor();
        TestAsync();
        TestParticleIntegration();
        std::cout << "task_executor_test: threads, ranges, exceptions, nesting and particle determinism, async groups passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "task_executor_test: " << error.what() << '\n';
        return 1;
    }
}
