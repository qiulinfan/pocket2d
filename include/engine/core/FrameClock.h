#ifndef ENGINE_CORE_FRAME_CLOCK_H
#define ENGINE_CORE_FRAME_CLOCK_H

#include "SDL3/SDL.h"
#include <algorithm>

namespace FrameClock {

// Runtime frame index visible to gameplay/debug APIs.
// 对外可见的运行时帧编号.
inline int frame_number = 0;
inline Uint64 current_frame_start_timestamp = 0;
inline bool initialized = false;

inline int GetFrameNumber() {
    return frame_number;
}

inline constexpr float GetDeltaTime() {
    return 1.0f / 60.0f;
}

inline void Reset() {
    current_frame_start_timestamp = SDL_GetTicksNS();
    frame_number = 0;
    initialized = true;
}

inline void EnsureInitialized() {
    if (initialized) return;
    Reset();
}

inline void DelayToMaintain60FPS() {
    EnsureInitialized();
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
    ++frame_number;
}

inline void DelayAndAdvanceFrame() {
    DelayToMaintain60FPS();
    AdvanceFrame();
}

} // namespace FrameClock

#endif
