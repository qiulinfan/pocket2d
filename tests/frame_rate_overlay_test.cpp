// The frame-rate readout: its figures over a half-second window, and that it
// draws only when shown and leaves the renderer's state as it found it.
#include "rendering/FrameRateOverlay.h"

#include <SDL3/SDL.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
void Check(bool condition, const char *message) {
    if (condition) return;
    std::fprintf(stderr, "frame_rate_overlay_test: %s\n", message);
    std::exit(1);
}

bool Near(float a, float b) { return std::fabs(a - b) < 0.05f; }

// Whether any pixel of the surface differs from opaque black.
bool Drawn(SDL_Surface *surface) {
    for (int y = 0; y < surface->h; ++y)
        for (int x = 0; x < surface->w; ++x) {
            Uint8 r, g, b, a;
            SDL_ReadSurfacePixel(surface, x, y, &r, &g, &b, &a);
            if (r != 0 || g != 0 || b != 0) return true;
        }
    return false;
}
}  // namespace

int main() {
    // 60 frames over a second: 4 ms of work each, one of 10 ms.
    FrameRateOverlay overlay;
    constexpr Uint64 tick = 16666667ULL;   // 1/60 s, rounded up so 30 fill the window
    Uint64 now = 1;
    overlay.RecordFrame(0, 0, now);   // starts the window
    for (int frame = 1; frame <= 30; ++frame) {
        now += tick;
        overlay.RecordFrame(frame == 7 ? 10000000ULL : 4000000ULL, frame % 2, now);
    }
    Check(Near(overlay.Fps(), 60.0f), "30 frames in half a second are 60 FPS");
    Check(Near(overlay.TicksPerSecond(), 30.0f), "15 ticks in half a second are 30 ticks a second");
    Check(Near(overlay.AverageMs(), 4.2f), "the average frame time covers the window");
    Check(Near(overlay.WorstMs(), 10.0f), "the worst frame time is the slowest frame");
    for (int frame = 1; frame <= 15; ++frame) {   // half the speed
        now += tick * 2;
        overlay.RecordFrame(4000000ULL, 1, now);
    }
    Check(Near(overlay.Fps(), 30.0f) && Near(overlay.WorstMs(), 4.0f), "each window starts afresh");

    SDL_Surface *surface = SDL_CreateSurface(640, 360, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer *renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
    Check(renderer != nullptr, "a software renderer is needed");
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    overlay.Draw(renderer);
    SDL_RenderPresent(renderer);
    Check(!Drawn(surface), "a hidden readout draws nothing");

    const SDL_Rect viewport{10, 20, 100, 50};
    SDL_SetRenderViewport(renderer, &viewport);
    SDL_SetRenderScale(renderer, 3.0f, 3.0f);
    SDL_SetRenderDrawColor(renderer, 1, 2, 3, 4);
    overlay.visible = true;
    overlay.Draw(renderer);
    SDL_RenderPresent(renderer);
    Check(Drawn(surface), "a shown readout draws");
    SDL_Rect after{};
    SDL_GetRenderViewport(renderer, &after);
    float sx = 0, sy = 0;
    SDL_GetRenderScale(renderer, &sx, &sy);
    Uint8 r, g, b, a;
    SDL_GetRenderDrawColor(renderer, &r, &g, &b, &a);
    Check(after.x == 10 && after.y == 20 && after.w == 100 && after.h == 50, "the viewport is restored");
    Check(sx == 3.0f && sy == 3.0f, "the render scale is restored");
    Check(r == 1 && g == 2 && b == 3 && a == 4, "the draw color is restored");

    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(surface);
    std::puts("frame_rate_overlay_test: ok");
    return 0;
}
