#include "core/World.h"
#include "input/Input.h"
#include "particles/ParticleManager.h"
#include "rendering/Renderer.h"

thread_local World *World::bound_ = nullptr;

World::World()
    : input_state_(std::make_unique<InputState>()),
      particle_state_(std::make_unique<ParticleState>()),
      render_queue_state_(std::make_unique<RenderQueueState>()) {}
World::~World() = default;

World &World::Current() {
    if (bound_ != nullptr) return *bound_;
    thread_local World default_world;
    return default_world;
}
