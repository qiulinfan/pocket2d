#ifndef ENGINE_CORE_HELPER_H
#define ENGINE_CORE_HELPER_H

// Compatibility shim:
// The old monolithic Helper API is now split across engine modules.
// 兼容层: 旧的 Helper API 已拆分到各个 engine 模块中.

#include "core/FrameClock.h"
#include "input/SDLEventHelper.h"
#include "rendering/SDLRenderHelper.h"
#include "particles/RandomEngine.h"
#include "SDL3/SDL.h"
#include "SDL3_image/SDL_image.h"
#include <string>

class Helper {
public:
    static int GetFrameNumber() {
        return FrameClock::GetFrameNumber();
    }

    static SDL_Window *SDL_CreateWindow(const char *title, int w, int h,
                                        SDL_WindowFlags flags) {
        return SDLRenderHelper::SDL_CreateWindow(title, w, h, flags);
    }

    static SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, const char *name) {
        return SDLRenderHelper::SDL_CreateRenderer(window, name);
    }

    static int SDL_PollEvent(SDL_Event *event) {
        return SDLEventHelper::SDL_PollEvent(event);
    }

    static void SDL_RenderPresent(SDL_Renderer *renderer,
                                  bool advance_gameplay_frame = true) {
        SDLRenderHelper::SDL_RenderPresent(renderer, advance_gameplay_frame);
    }

    static void SDL_RenderTextureRotated(Actor::UID actor_uid,
                                 const std::string &actor_name,
                                 SDL_Renderer *renderer, SDL_Texture *texture,
                                 const SDL_FRect *srcrect,
                                 const SDL_FRect *dstrect, float angle,
                                 const SDL_FPoint *center,
                                 SDL_FlipMode flip) {
        SDLRenderHelper::SDL_RenderTextureRotated(actor_uid, actor_name, renderer,
                                          texture,
                                          srcrect, dstrect, angle, center, flip);
    }

    static void SDL_RenderTexture(SDL_Renderer *renderer, SDL_Texture *texture,
                               const SDL_FRect *srcrect,
                               const SDL_FRect *dstrect) {
        SDLRenderHelper::SDL_RenderTexture(renderer, texture, srcrect, dstrect);
    }

    static void SDL_GetTextureSize(SDL_Texture *texture, float *w, float *h) {
        SDLRenderHelper::SDL_GetTextureSize(texture, w, h);
    }
};

#endif
