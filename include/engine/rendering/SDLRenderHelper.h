#ifndef ENGINE_RENDERING_SDL_RENDER_HELPER_H
#define ENGINE_RENDERING_SDL_RENDER_HELPER_H

#include "core/FrameClock.h"
#include "input/SDLEventHelper.h"
#include "scene/Actor.h"
#include "SDL3/SDL.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

class SDLRenderHelper {
public:
    enum class RenderLoggerStatus {
        NotInitialized,
        NotEnabled,
        Enabled
    };

    inline static std::string frame_directory_relative_path = "frames";

    static SDL_Window *SDL_CreateWindow(const char *title, int w, int h,
                                        SDL_WindowFlags flags) {
        return ::SDL_CreateWindow(title, w, h, flags);
    }

    static SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, const char *name) {
        SDL_Renderer *renderer = ::SDL_CreateRenderer(window, name);
        if (renderer == nullptr) {
            std::cerr << "Failed to create renderer : " << SDL_GetError()
                      << std::endl;
        }
        return renderer;
    }

    // Presenting does not pace: Engine::GameLoop renders as often as it can
    // (or as game.config's max_fps/vsync ask), and the editor and --frames
    // loops, which run one tick per frame, pace themselves to 60 Hz.
    static void SDL_RenderPresent(SDL_Renderer *renderer,
                                  bool advance_gameplay_frame = true) {
        (void)advance_gameplay_frame;
        if (renderer == nullptr) {
            std::cout << "ERROR : renderer passed to SDL_RenderPresent is null."
                      << std::endl;
            return;
        }

        CaptureFrameIfRecordingEnabled(renderer);
        ::SDL_RenderPresent(renderer);
    }

    static void SDL_RenderTextureRotated(Actor::UID actor_uid,
                                 const std::string &actor_name,
                                 SDL_Renderer *renderer, SDL_Texture *texture,
                                 const SDL_FRect *srcrect,
                                 const SDL_FRect *dstrect, float angle,
                                 const SDL_FPoint *center,
                                 SDL_FlipMode flip) {
        ::SDL_RenderTextureRotated(renderer, texture, srcrect, dstrect, angle,
                                   center, flip);

        CheckForRenderLoggerInit();
        if (render_logger_mode_ != RenderLoggerStatus::Enabled) return;

        float x_scale = 1.0f;
        float y_scale = 1.0f;
        SDL_GetRenderScale(renderer, &x_scale, &y_scale);

        render_logging_file_ << FrameClock::GetFrameNumber() << ":" << actor_uid
                             << ":" << actor_name;
        if (dstrect != nullptr) {
            render_logging_file_ << " dstrect " << dstrect->x << " " << dstrect->y
                                 << " " << dstrect->w << " " << dstrect->h;
        }
        render_logging_file_ << " angle " << angle;
        if (center != nullptr) {
            render_logging_file_ << " center " << center->x << " " << center->y;
        }
        render_logging_file_ << " flip " << flip << " renderscale " << x_scale
                             << " " << y_scale << std::endl;
    }

    static void SDL_RenderTexture(SDL_Renderer *renderer, SDL_Texture *texture,
                               const SDL_FRect *srcrect,
                               const SDL_FRect *dstrect) {
        ::SDL_RenderTexture(renderer, texture, srcrect, dstrect);
    }

    static void SDL_GetTextureSize(SDL_Texture *texture, float *w, float *h) {
        if (texture != nullptr) ::SDL_GetTextureSize(texture, w, h);
    }

    static void Reset() {
        render_logging_file_.close();
        render_logging_file_.clear();
        render_logger_mode_ = RenderLoggerStatus::NotInitialized;
    }

private:
inline static RenderLoggerStatus render_logger_mode_ = RenderLoggerStatus::NotInitialized;
    inline static std::ofstream render_logging_file_;

    static bool IsEnvVariableSet(const char *name) {
        const char *value = std::getenv(name);
        return value != nullptr;
    }

    static void CheckForRenderLoggerInit() {
        if (render_logger_mode_ != RenderLoggerStatus::NotInitialized) return;

        if (!IsEnvVariableSet("RENDERLOGGER")) {
            render_logger_mode_ = RenderLoggerStatus::NotEnabled;
            return;
        }

        render_logger_mode_ = RenderLoggerStatus::Enabled;
        std::ofstream truncate_file("render_logger.txt", std::ios::out);
        render_logging_file_.open("render_logger.txt", std::ios::app);
        if (!render_logging_file_.is_open()) {
            std::cerr << "Error : failed to open render_logger.txt."
                      << std::endl;
            return;
        }

        render_logging_file_ << "== RENDER LOGGER ==" << std::endl;
        render_logging_file_
            << "Study SDL_RenderTextureRotated() calls for render debugging."
            << std::endl;
        render_logging_file_ << "Enable with RENDERLOGGER env var." << std::endl;
        render_logging_file_ << "frame:actor_uid:actor_name" << std::endl
                             << std::endl;
    }

    static void CaptureFrameIfRecordingEnabled(SDL_Renderer *renderer) {
        if (!SDLEventHelper::RECORDING_MODE) return;

        std::filesystem::create_directories(frame_directory_relative_path);
        SDL_Surface *surface = SDL_RenderReadPixels(renderer, nullptr);
        if (surface == nullptr) {
            SDL_Log("SDL_RenderReadPixels() failed: %s", SDL_GetError());
            return;
        }
        std::stringstream filename_stream;
        filename_stream << "frame_" << std::setw(5) << std::setfill('0')
                        << FrameClock::GetFrameNumber() << ".bmp";
        const std::string output_file_path =
            frame_directory_relative_path + "/" + filename_stream.str();
        if (!SDL_SaveBMP(surface, output_file_path.c_str())) {
            SDL_Log("SDL_SaveBMP() failed: %s", SDL_GetError());
        }
        SDL_DestroySurface(surface);
    }
};

#endif
