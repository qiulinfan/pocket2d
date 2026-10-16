#ifndef ENGINE_CORE_FRAME_CLOCK_H
#define ENGINE_CORE_FRAME_CLOCK_H

#include "SDL3/SDL.h"
#include "core/World.h"
#include <algorithm>

namespace FrameClock {

// The frame index and the pacing timestamp belong to the current World.
// Runtime frame index visible to gameplay/debug APIs.
// 对外可见的运行时帧编号.
inline int GetFrameNumber() {
    return World::Current().frame_number;
}

inline constexpr float GetDeltaTime() {
    return 1.0f / 60.0f;
}

inline void Reset() {
    World &world = World::Current();
    world.frame_start_timestamp = SDL_GetTicksNS();
    world.frame_number = 0;
    world.frame_clock_initialized = true;
}

inline void EnsureInitialized() {
    if (World::Current().frame_clock_initialized) return;
    Reset();
}

inline void DelayToMaintain60FPS() {
    EnsureInitialized();
    Uint64 &current_frame_start_timestamp = World::Current().frame_start_timestamp;
    constexpr Uint64 interval_ns = 1000000000ULL / 60;
    const Uint64 deadline = current_frame_start_timestamp + interval_ns;
    const Uint64 now = SDL_GetTicksNS();
    if (now < deadline) SDL_DelayNS(deadline - now);
    // Preserve the cadence across normal timer overshoot; do not burst to catch
    // up after a long stall. Simulation always advances in fixed 1/60 ticks.
    current_frame_start_timestamp =
        now > deadline + interval_ns ? now : deadline;
}

inline void AdvanceFrame() {
    EnsureInitialized();
    ++World::Current().frame_number;
}

inline void DelayAndAdvanceFrame() {
    DelayToMaintain60FPS();
    AdvanceFrame();
}

} // namespace FrameClock

#endif
