#ifndef ENGINE_CORE_WORLD_H
#define ENGINE_CORE_WORLD_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

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

    // Process-unique identifier, for diagnostics and agent sessions.
    std::uint64_t GetId() const { return id_; }

    // The World bound on this thread, or this thread's default World.
    static World &Current() { return bound_ != nullptr ? *bound_ : ThreadDefault(); }

    // This World's instance of a subsystem's state type, default-constructed on
    // first use. Subsystems define T privately, so World does not depend on
    // them. States are destroyed in reverse order of creation.
    template <class T> T &State();

    // Gameplay tick counter, advanced once per completed simulation step.
    int frame_number = 0;
    bool frame_clock_initialized = false;
    // Start of the current presented frame, for 60 Hz pacing (SDL ticks, ns).
    std::uint64_t frame_start_timestamp = 0;

    // Whether this World plays through the process audio device. Headless
    // Worlds turn it off so they never touch the shared mixer.
    bool audio_output_enabled = true;

private:
    struct SlotBase {
        virtual ~SlotBase() = default;
    };
    template <class T> struct Slot final : SlotBase {
        T value;
    };

    static World &ThreadDefault();
    static std::size_t NextSlotIndex();
    // Assigned during static initialization, so lookups need no guard.
    template <class T> static inline const std::size_t slot_index_ = NextSlotIndex();
    template <class T> [[gnu::noinline]] T &CreateState();

    std::uint64_t id_;
    std::vector<std::unique_ptr<SlotBase>> slots_;
    std::vector<std::size_t> creation_order_;

    friend class WorldScope;
    static thread_local World *bound_;
};

template <class T> T &World::State() {
    const std::size_t index = slot_index_<T>;
    if (index < slots_.size() && slots_[index]) [[likely]]
        return static_cast<Slot<T> &>(*slots_[index]).value;
    return CreateState<T>();
}

template <class T> T &World::CreateState() {
    const std::size_t index = slot_index_<T>;
    if (index >= slots_.size()) slots_.resize(index + 1);
    slots_[index] = std::make_unique<Slot<T>>();
    creation_order_.push_back(index);
    return static_cast<Slot<T> &>(*slots_[index]).value;
}

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
