#include "core/Engine.h"
#include "core/FrameClock.h"
#include "rendering/Renderer.h"
#include "particles/ParticleManager.h"
#include "rendering/SDLRenderHelper.h"
#include "shared/resources/ResourcePath.h"
#include "SDL3_image/SDL_image.h"
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>;
using Texture = std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)>;

void Check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message + ": " + SDL_GetError());
}

struct ProjectFixture {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("pocket2d-render-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));

    ProjectFixture() {
        std::filesystem::create_directories(path / "images");
        std::filesystem::create_directories(path / "scenes");
        std::ofstream(path / "game.config") <<
            R"({"game_title":"Pocket2D render test","initial_scene":"empty"})";
        std::ofstream(path / "rendering.config") <<
            R"({"x_resolution":64,"y_resolution":64,"clear_color_r":3,"clear_color_g":7,"clear_color_b":11})";
        std::ofstream(path / "scenes/empty.scene") << R"({"actors":[]})";
        Surface pixels(SDL_CreateSurface(8, 8, SDL_PIXELFORMAT_RGBA8888),
                       SDL_DestroySurface);
        Check(pixels != nullptr, "create fixture pixels");
        Check(SDL_FillSurfaceRect(pixels.get(), nullptr,
              SDL_MapSurfaceRGBA(pixels.get(), 240, 20, 30, 255)), "fill fixture pixels");
        Check(SDL_SaveBMP(pixels.get(), (path / "images/red.bmp").string().c_str()),
              "write fixture texture");
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                const Uint8 value = (x + y) % 2 == 0 ? 0 : 255;
                SDL_Rect rect{x, y, 1, 1};
                Check(SDL_FillSurfaceRect(pixels.get(), &rect,
                      SDL_MapSurfaceRGBA(pixels.get(), value, value, value, 255)),
                      "fill checker texture");
            }
        }
        Check(SDL_SaveBMP(pixels.get(), (path / "images/checker.bmp").string().c_str()),
              "write checker texture");
        ResourcePath::SetResourcesRootPath(path);
    }

    ~ProjectFixture() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

std::array<Uint8, 4> Pixel(SDL_Surface *surface, int x, int y) {
    std::array<Uint8, 4> color{};
    Check(SDL_ReadSurfacePixel(surface, x, y, &color[0], &color[1],
                              &color[2], &color[3]), "read pixel");
    return color;
}

Surface ReadTarget(SDL_Renderer *renderer, SDL_Texture *target) {
    Check(SDL_SetRenderTarget(renderer, target), "select readback target");
    Surface image(SDL_RenderReadPixels(renderer, nullptr), SDL_DestroySurface);
    Check(image != nullptr, "read renderer pixels");
    Check(SDL_SetRenderTarget(renderer, nullptr), "restore window target");
    return image;
}

void TestFractionalTexturePath(SDL_Renderer *renderer) {
    Texture target(SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                    SDL_TEXTUREACCESS_TARGET, 64, 64),
                   SDL_DestroyTexture);
    Check(target != nullptr, "create native render target");
    SDL_Texture *sprite = Renderer::LoadTexture("red", renderer);
    Check(sprite != nullptr, "load real image texture");
    const SDL_FRect source{0.25f, 0.5f, 7.5f, 7.0f};
    const SDL_FRect destination{10.75f, 12.25f, 19.5f, 15.75f};
    const SDL_FPoint pivot{4.25f, 6.5f};
    auto draw = [&](bool helper) {
        Check(SDL_SetRenderTarget(renderer, target.get()), "select float target");
        Check(SDL_SetRenderScale(renderer, 1.0f, 1.0f), "reset scale");
        Check(SDL_SetRenderDrawColor(renderer, 3, 7, 11, 255), "set clear color");
        Check(SDL_RenderClear(renderer), "clear float target");
        if (helper) {
            SDLRenderHelper::SDL_RenderTextureRotated(Actor::kInvalidUID, "fixture",
                renderer, sprite, &source, &destination, 23.5f, &pivot, SDL_FLIP_NONE);
        } else {
            Check(SDL_RenderTextureRotated(renderer, sprite, &source, &destination,
                                           23.5, &pivot, SDL_FLIP_NONE), "native float draw");
        }
        return ReadTarget(renderer, target.get());
    };
    Surface actual = draw(true);
    Surface expected = draw(false);
    int painted_pixels = 0;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const auto actual_pixel = Pixel(actual.get(), x, y);
            Check(actual_pixel == Pixel(expected.get(), x, y),
                  "engine texture path must match native SDL3 fractional coordinates");
            if (actual_pixel[0] > 100) ++painted_pixels;
        }
    }
    Check(painted_pixels > 100, "fractional sprite must actually draw pixels");

    // Magnify a real loaded pixel asset. Every interior pixel must remain black
    // or white; SDL3's default linear sampling would introduce gray boundaries.
    SDL_Texture *checker = Renderer::LoadTexture("checker", renderer);
    Check(checker != nullptr, "load checker texture");
    Check(SDL_SetRenderTarget(renderer, target.get()), "select checker target");
    Check(SDL_SetRenderDrawColor(renderer, 3, 7, 11, 255), "set checker background");
    Check(SDL_RenderClear(renderer), "clear checker target");
    const SDL_FRect checker_destination{8, 8, 32, 32};
    SDLRenderHelper::SDL_RenderTexture(renderer, checker, nullptr, &checker_destination);
    Surface magnified = ReadTarget(renderer, target.get());
    int black = 0;
    int white = 0;
    for (int y = 9; y < 39; ++y) {
        for (int x = 9; x < 39; ++x) {
            const auto pixel = Pixel(magnified.get(), x, y);
            const bool is_black = pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0;
            const bool is_white = pixel[0] == 255 && pixel[1] == 255 && pixel[2] == 255;
            Check(is_black || is_white, "magnified pixel sprites must not blur");
            black += is_black;
            white += is_white;
        }
    }
    Check(black > 100 && white > 100, "checker magnification must preserve both colors");
}

void TestParticleBatchPath(SDL_Renderer *renderer) {
    Texture target(SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                    SDL_TEXTUREACCESS_TARGET, 64, 64), SDL_DestroyTexture);
    Check(target != nullptr, "create particle target");
    const int emitter = ParticleManager::CreateEmitter();
    ParticleEmitterConfig config; config.duration_frames = 4;
    ParticleManager::ConfigureEmitter(emitter, config);
    ParticleSpawnRequest particle; particle.image_name = "red";
    ParticleManager::SpawnParticle(emitter, particle);
    auto draw = [&](float camera_x) {
        Check(SDL_SetRenderTarget(renderer, target.get()), "select particle target");
        Check(SDL_SetRenderScale(renderer, 1, 1), "reset particle scale");
        Check(SDL_SetRenderDrawColor(renderer, 3, 7, 11, 255), "set particle background");
        Check(SDL_RenderClear(renderer), "clear particle target");
        ParticleManager::RenderEmitterBatch(emitter, renderer, camera_x, 0, 1, 64, 64);
        return ReadTarget(renderer, target.get());
    };
    const auto visible = draw(0);
    Check(Pixel(visible.get(), 32, 32) == std::array<Uint8, 4>{240, 20, 30, 255},
          "validated particle batch must draw the real sprite");
    Check(Pixel(visible.get(), 0, 0) == std::array<Uint8, 4>{3, 7, 11, 255},
          "particle batch must preserve background outside its sprite");
    // Finite world inputs can still overflow their projected float rectangle.
    // The batch must skip those draws before passing them to SDL.
    const auto projected_overflow = draw(std::numeric_limits<float>::max() / 2);
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x)
        Check(Pixel(projected_overflow.get(), x, y) == std::array<Uint8, 4>{3, 7, 11, 255},
              "nonfinite projected particle rectangle must not draw");
    ParticleManager::DestroyEmitter(emitter);
}

void TestRuntime(Engine &engine, const std::string &backend,
                 const std::filesystem::path &screenshot) {
    SDL_Renderer *renderer = engine.GetRenderer();
    Check(renderer != nullptr && engine.GetWindow() != nullptr, "real graphics initialization");
    Check(SDL_GetRendererName(renderer) == backend, "explicit backend must be honored");
    Check(FrameClock::GetFrameNumber() == 0, "new renderer session starts at frame zero");
    engine.SetRenderRuntimeToTexture(true);
    engine.SetRuntimeRenderTargetSize(64, 64);
    ImageDrawRequest sprite;
    sprite.image_name = "red";
    sprite.x = 4.0f;
    sprite.y = 5.0f;
    Renderer::QueueUIImageDraw(sprite);
    Renderer::QueuePixelDraw({30, 30, 20, 230, 40, 255});
    engine.RunSingleFrame(false);
    Check(FrameClock::GetFrameNumber() == 1, "rendered simulation advances once");
    Surface image = ReadTarget(renderer, engine.GetRuntimeRenderTarget());
    Check(Pixel(image.get(), 6, 7) == std::array<Uint8, 4>{240, 20, 30, 255},
          "loaded sprite has expected pixels");
    Check(Pixel(image.get(), 30, 30) == std::array<Uint8, 4>{20, 230, 40, 255},
          "Lua-style pixel request reaches render target");
    Check(Pixel(image.get(), 50, 50) == std::array<Uint8, 4>{3, 7, 11, 255},
          "runtime clear color reaches render target");
    if (!screenshot.empty()) {
        if (!screenshot.parent_path().empty())
            std::filesystem::create_directories(screenshot.parent_path());
        Check(IMG_SavePNG(image.get(), screenshot.string().c_str()), "save test-only screenshot");
    }
    engine.PresentFrame(true);
    engine.PresentFrame(false);
    Check(FrameClock::GetFrameNumber() == 1, "presentation must not double-step simulation");
    engine.RunRenderFrozenFrame(false, false);
    Check(FrameClock::GetFrameNumber() == 1, "frozen editor render must not advance simulation");
    Surface cleared = ReadTarget(renderer, engine.GetRuntimeRenderTarget());
    Check(Pixel(cleared.get(), 6, 7) == std::array<Uint8, 4>{3, 7, 11, 255},
          "frame requests must not leak into next render");
    engine.StepSimulation();
    Check(FrameClock::GetFrameNumber() == 2, "nonrendered step advances exactly once");
    TestFractionalTexturePath(renderer);
    TestParticleBatchPath(renderer);
}
} // namespace

int main(int argc, char **argv) {
    const std::string backend = argc > 1 ? argv[1] : "software";
    const std::filesystem::path screenshot = argc > 2 ? argv[2] : "";
    try {
        ProjectFixture fixture;
        Check(SDL_setenv_unsafe("POCKET_RENDER_DRIVER", backend.c_str(), 1) == 0,
              "set requested backend");
        Check(SDL_setenv_unsafe("POCKET_RENDER_VSYNC", "0", 1) == 0, "disable vsync");
        {
            Engine engine;
            Check(engine.GetLastSceneLoadError().empty(), engine.GetLastSceneLoadError());
            try {
                engine.InitializeRuntime();
            } catch (const std::exception &error) {
                if (backend != "software") {
                    std::cout << "SKIP backend " << backend << ": " << error.what() << '\n';
                    return 77;
                }
                throw;
            }
            TestRuntime(engine, backend, screenshot);
        }
        Check(SDL_setenv_unsafe("POCKET_RENDER_DRIVER", "pocket2d_invalid_backend", 1) == 0,
              "set invalid backend");
        bool rejected = false;
        {
            Engine engine;
            try {
                engine.InitializeRuntime();
            } catch (const std::exception &error) {
                rejected = std::string(error.what()).find("pocket2d_invalid_backend") !=
                           std::string::npos;
            }
        }
        Check(rejected, "unavailable explicit backend must fail without fallback");
        std::cout << "PASS render backend " << backend << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL render backend " << backend << ": " << error.what() << '\n';
        return 1;
    }
}
