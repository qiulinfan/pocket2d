#ifndef ENGINE_CORE_WORLD_H
#define ENGINE_CORE_WORLD_H

#include <memory>

struct InputState;
struct ParticleState;
struct RenderQueueState;

// A World owns one simulation's mutable runtime state. Engine subsystems keep
// their static entry points and resolve state through World::Current(), in the
// same way Dear ImGui resolves its current context.
//
// Threading: World::Current() is thread-local. Each thread lazily owns a default
// World, and WorldScope binds another one for a scope. One World is used by one
// thread at a time; different Worlds may step on different threads. Lua states,
// LuaRefs and physics worlds belong to their World and are never shared.
class World {
public:
    World();
    ~World();
    World(const World &) = delete;
    World &operator=(const World &) = delete;

    // The World bound on this thread, or this thread's default World.
    static World &Current();

    // Gameplay tick counter, advanced once per completed simulation step.
    int frame_number = 0;
    bool frame_clock_initialized = false;

    // Whether this World plays through the process audio device. Headless
    // Worlds turn it off so they never touch the shared mixer.
    bool audio_output_enabled = true;

    InputState &GetInputState() { return *input_state_; }
    ParticleState &GetParticleState() { return *particle_state_; }
    RenderQueueState &GetRenderQueueState() { return *render_queue_state_; }

private:
    std::unique_ptr<InputState> input_state_;
    std::unique_ptr<ParticleState> particle_state_;
    std::unique_ptr<RenderQueueState> render_queue_state_;

    friend class WorldScope;
    static thread_local World *bound_;
};

// Binds a World as current for the enclosing scope and restores the previous
// binding on exit. Scopes nest.
class WorldScope {
public:
    explicit WorldScope(World &world) noexcept : previous_(World::bound_) {
        World::bound_ = &world;
    }
    ~WorldScope() { World::bound_ = previous_; }
    WorldScope(const WorldScope &) = delete;
    WorldScope &operator=(const WorldScope &) = delete;

private:
    World *previous_;
};

#endif
