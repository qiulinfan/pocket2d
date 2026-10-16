#include "core/World.h"

#include <atomic>

thread_local World *World::bound_ = nullptr;

World::World() {
    static std::atomic<std::uint64_t> next_id{1};
    id_ = next_id.fetch_add(1, std::memory_order_relaxed);
}

World::~World() {
    // Bind this World while its states are destroyed: their destructors may
    // reach other subsystems through World::Current().
    WorldScope scope(*this);
    for (auto it = creation_order_.rbegin(); it != creation_order_.rend(); ++it)
        slots_[*it].reset();
}

World &World::ThreadDefault() {
    thread_local World default_world;
    // Bind it so later lookups on this thread take the fast path. A WorldScope
    // that restores "unbound" simply leads back here.
    bound_ = &default_world;
    return default_world;
}

std::size_t World::NextSlotIndex() {
    static std::atomic<std::size_t> next{0};
    return next.fetch_add(1, std::memory_order_relaxed);
}
