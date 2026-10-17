#include "rendering/FrameRateOverlay.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>

namespace {
constexpr Uint64 kWindowNs = 500000000ULL;
}

void FrameRateOverlay::RecordFrame(Uint64 work_ns, int ticks, Uint64 now_ns) {
    if (window_start_ns_ == 0) {
        window_start_ns_ = now_ns;
        return;
    }
    ++frames_;
    ticks_ += ticks;
    work_total_ns_ += work_ns;
    work_worst_ns_ = std::max(work_worst_ns_, work_ns);
    const Uint64 elapsed = now_ns - window_start_ns_;
    if (elapsed < kWindowNs) return;
    fps_ = static_cast<float>(frames_ * 1e9 / static_cast<double>(elapsed));
    ticks_per_second_ = static_cast<float>(ticks_ * 1e9 / static_cast<double>(elapsed));
    average_ms_ = static_cast<float>(work_total_ns_ / 1e6 / frames_);
    worst_ms_ = static_cast<float>(work_worst_ns_ / 1e6);
    measured_ = true;
    window_start_ns_ = now_ns;
    frames_ = ticks_ = 0;
    work_total_ns_ = work_worst_ns_ = 0;
}

void FrameRateOverlay::Draw(SDL_Renderer *renderer) const {
    if (!visible || renderer == nullptr) return;
    char fps_line[32], time_line[48];
    if (measured_) {
        std::snprintf(fps_line, sizeof fps_line, "%.0f FPS  %.0f ticks/s", fps_, ticks_per_second_);
        std::snprintf(time_line, sizeof time_line, "frame %.1f ms  worst %.1f", average_ms_, worst_ms_);
    } else {
        std::snprintf(fps_line, sizeof fps_line, "-- FPS");
        std::snprintf(time_line, sizeof time_line, "frame -- ms");
    }

    // Draw over the whole output in pixels, then put the renderer's state back.
    SDL_Texture *target = SDL_GetRenderTarget(renderer);
    SDL_Rect viewport;
    SDL_GetRenderViewport(renderer, &viewport);
    float scale_x = 1.0f, scale_y = 1.0f;
    SDL_GetRenderScale(renderer, &scale_x, &scale_y);
    Uint8 r, g, b, a;
    SDL_GetRenderDrawColor(renderer, &r, &g, &b, &a);
    SDL_BlendMode blend;
    SDL_GetRenderDrawBlendMode(renderer, &blend);

    SDL_SetRenderTarget(renderer, nullptr);
    SDL_SetRenderViewport(renderer, nullptr);
    int out_w = 0, out_h = 0;
    SDL_GetCurrentRenderOutputSize(renderer, &out_w, &out_h);
    // The 8-pixel debug font, at a size that reads at any window size.
    const float scale = static_cast<float>(std::max(2, out_h / 480));
    SDL_SetRenderScale(renderer, scale, scale);
    const float glyph = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
    const float width = static_cast<float>(std::max(SDL_strlen(fps_line), SDL_strlen(time_line))) * glyph + 8.0f;
    const float height = glyph * 2.0f + 10.0f;
    const float x = (out_w / scale - width) * 0.5f, y = out_h / scale - height - 4.0f;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 160);
    const SDL_FRect panel{x, y, width, height};
    SDL_RenderFillRect(renderer, &panel);
    // Green while gameplay keeps 60 ticks a second, yellow when it falls a
    // little behind, red when well below.
    if (!measured_ || ticks_per_second_ >= 57.0f) SDL_SetRenderDrawColor(renderer, 120, 230, 120, 255);
    else if (ticks_per_second_ >= 45.0f) SDL_SetRenderDrawColor(renderer, 240, 210, 90, 255);
    else SDL_SetRenderDrawColor(renderer, 240, 100, 90, 255);
    SDL_RenderDebugText(renderer, x + (width - SDL_strlen(fps_line) * glyph) * 0.5f, y + 4.0f, fps_line);
    SDL_SetRenderDrawColor(renderer, 230, 230, 230, 255);
    SDL_RenderDebugText(renderer, x + 4.0f, y + glyph + 6.0f, time_line);

    SDL_SetRenderScale(renderer, scale_x, scale_y);
    SDL_SetRenderTarget(renderer, target);
    SDL_SetRenderViewport(renderer, &viewport);
    SDL_SetRenderDrawColor(renderer, r, g, b, a);
    SDL_SetRenderDrawBlendMode(renderer, blend);
}
