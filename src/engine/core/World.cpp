#include "core/World.h"

thread_local World *World::bound_ = nullptr;

World::World() = default;
World::~World() = default;

World &World::Current() {
    if (bound_ != nullptr) return *bound_;
    thread_local World default_world;
    return default_world;
}
