#include "core/Engine.h"
#include "core/FrameClock.h"
#include "core/World.h"
#include "scripting/APIRegistration.h"
#include "lua.hpp"
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
        // Fully transparent: a multiplied draw of it must leave the picture alone.
        Check(SDL_FillSurfaceRect(pixels.get(), nullptr, SDL_MapSurfaceRGBA(pixels.get(), 0, 0, 0, 0)),
              "clear hole texture");
        Check(SDL_SaveBMP(pixels.get(), (path / "images/hole.bmp").string().c_str()), "write hole texture");
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
    // UI scaling stretches from the top-left corner; a negative scale mirrors.
    int width = 0, height = 0;
    Check(Renderer::GetImageSize("red", width, height) && width == 8 && height == 8,
          "image size resolves without drawing");
    Check(!Renderer::GetImageSize("missing_image", width, height), "missing image has no size");
    ImageDrawRequest scaled;
    scaled.image_name = "red";
    scaled.x = 40.0f; scaled.y = 4.0f; scaled.scale_x = 2.0f; scaled.scale_y = 3.0f;
    Renderer::QueueUIImageDraw(scaled);
    ImageDrawRequest mirrored;
    mirrored.image_name = "checker";
    mirrored.x = 40.0f; mirrored.y = 40.0f; mirrored.scale_x = -1.0f;
    Renderer::QueueUIImageDraw(mirrored);
    engine.RunSingleFrame(false);
    Surface ui = ReadTarget(renderer, engine.GetRuntimeRenderTarget());
    Check(Pixel(ui.get(), 55, 27) == std::array<Uint8, 4>{240, 20, 30, 255}, "scaled UI covers 16x24");
    Check(Pixel(ui.get(), 56, 10) == std::array<Uint8, 4>{3, 7, 11, 255} &&
          Pixel(ui.get(), 45, 28) == std::array<Uint8, 4>{3, 7, 11, 255}, "scaled UI stops at its size");
    Check(Pixel(ui.get(), 40, 40) == std::array<Uint8, 4>{255, 255, 255, 255},
          "negative UI scale mirrors (checker corner turns white)");
    // Dynamic images draw by name and re-upload when their pixels change.
    Renderer::SetDynamicImage("dynamic:test", 2, 1, {0, 0, 255, 255, 255, 255, 0, 255});
    ImageDrawRequest generated;
    generated.image_name = "dynamic:test";
    generated.x = 2.0f; generated.y = 50.0f; generated.scale_x = 4.0f; generated.scale_y = 4.0f;
    Renderer::QueueUIImageDraw(generated);
    engine.RunSingleFrame(false);
    Surface first = ReadTarget(renderer, engine.GetRuntimeRenderTarget());
    Check(Pixel(first.get(), 3, 51) == std::array<Uint8, 4>{0, 0, 255, 255} &&
          Pixel(first.get(), 7, 51) == std::array<Uint8, 4>{255, 255, 0, 255}, "dynamic image pixels draw");
    Renderer::SetDynamicImage("dynamic:test", 2, 1, {0, 255, 0, 255, 0, 255, 0, 255});
    Renderer::QueueUIImageDraw(generated);
    engine.RunSingleFrame(false);
    Surface second = ReadTarget(renderer, engine.GetRuntimeRenderTarget());
    Check(Pixel(second.get(), 3, 51) == std::array<Uint8, 4>{0, 255, 0, 255}, "updated dynamic pixels re-upload");
    // A multiply light map scales what is under it, and smooth sampling
    // blends neighbouring cells instead of stepping between them.
    Renderer::SetDynamicImage("dynamic:light", 2, 1, {128, 255, 64, 255, 255, 255, 255, 255},
                              {.smooth = true, .multiply = true});
    ImageDrawRequest under;
    under.image_name = "red";
    under.x = 2.0f; under.y = 60.0f; under.scale_x = 4.0f; under.scale_y = 2.0f;
    Renderer::QueueUIImageDraw(under);
    ImageDrawRequest light;
    light.image_name = "dynamic:light";
    light.x = 2.0f; light.y = 60.0f; light.scale_x = 16.0f; light.scale_y = 16.0f;
    light.sorting_order = 1;
    Renderer::QueueUIImageDraw(light);
    engine.RunSingleFrame(false);
    Surface lit = ReadTarget(renderer, engine.GetRuntimeRenderTarget());
    const auto near = [](Uint8 value, int expected) { return std::abs(static_cast<int>(value) - expected) <= 3; };
    const auto dim = Pixel(lit.get(), 3, 61);
    Check(near(dim[0], 120) && near(dim[1], 20) && near(dim[2], 8), "multiply scales the colour under it");
    const auto middle = Pixel(lit.get(), 18, 61);
    Check(middle[0] > 140 && middle[0] < 220, "smooth sampling blends between neighbouring pixels");
    Check(Pixel(lit.get(), 31, 61)[0] >= 237, "a white light cell leaves the colour unchanged");
    // Additive draws add their light (scaled by alpha) to what is under them,
    // in draw order, and leave the texture blending normally for later draws.
    const auto ui_draw = [](float x, int a, ImageBlend blend, int order, float y = 20.0f) {
        ImageDrawRequest request;
        request.image_name = "red";
        request.x = x; request.y = y; request.a = a; request.blend = blend; request.sorting_order = order;
        Renderer::QueueUIImageDraw(request);
    };
    ui_draw(10.0f, 255, ImageBlend::Normal, 0);
    ui_draw(10.0f, 255, ImageBlend::Add, 1);
    ui_draw(30.0f, 128, ImageBlend::Add, 0);
    ui_draw(50.0f, 128, ImageBlend::Normal, 1);
    // Multiplied draws scale what is under them by their colour and let alpha
    // through: under * colour + under * (1 - alpha).
    ui_draw(10.0f, 255, ImageBlend::Normal, 0, 40.0f);
    ui_draw(10.0f, 255, ImageBlend::Multiply, 1, 40.0f);
    ui_draw(30.0f, 255, ImageBlend::Normal, 0, 40.0f);
    ui_draw(30.0f, 128, ImageBlend::Multiply, 1, 40.0f);

    engine.RunSingleFrame(false);
    Surface added = ReadTarget(renderer, engine.GetRuntimeRenderTarget());
    const auto near_pixel = [&](std::array<Uint8, 4> pixel, int r, int g, int b) {
        return near(pixel[0], r) && near(pixel[1], g) && near(pixel[2], b);
    };
    Check(near_pixel(Pixel(added.get(), 12, 22), 255, 40, 60), "additive draw sums onto the sprite under it");
    Check(near_pixel(Pixel(added.get(), 32, 22), 123, 17, 26), "additive draw scales its light by alpha");
    Check(near_pixel(Pixel(added.get(), 52, 22), 121, 13, 20), "a normal draw after an additive one alpha blends");
    Check(near_pixel(Pixel(added.get(), 12, 42), 226, 2, 4), "a multiplied draw scales the sprite under it by its colour");
    Check(near_pixel(Pixel(added.get(), 32, 42), 255, 11, 17), "a multiplied draw lets its alpha through");

    // Rotated, a transparent multiplied image changes nothing, also where SDL's
    // software renderer would fill the rotated rectangle's corners.
    {
        ImageDrawRequest under;
        under.image_name = "red";
        under.scale_x = under.scale_y = 8.0f;
        Renderer::QueueSceneImageDraw(under);
        ImageDrawRequest hole;
        hole.image_name = "hole";
        hole.scale_x = -4.0f; hole.scale_y = 4.0f; hole.rotation_degrees = 30.0f;
        hole.blend = ImageBlend::Multiply; hole.sorting_order = 1;
        Renderer::QueueSceneImageDraw(hole);
        engine.SetCameraZoom(1.7f); // the software renderer's bug needs a render scale
        engine.RunSingleFrame(false);
        engine.SetCameraZoom(1.0f);
        Surface rotated = ReadTarget(renderer, engine.GetRuntimeRenderTarget());
        Check(near_pixel(Pixel(rotated.get(), 56, 56), 240, 20, 30) && near_pixel(Pixel(rotated.get(), 32, 32), 240, 20, 30),
              "a rotated transparent multiplied draw changes nothing");
    }
    // Image files sample nearest by default; smooth images blend neighbouring pixels.
    {
        const auto draw_checker = [&]() {
            ImageDrawRequest c;
            c.image_name = "checker";
            c.scale_x = c.scale_y = 4.0f;
            Renderer::QueueUIImageDraw(c);
            engine.RunSingleFrame(false);
            return ReadTarget(renderer, engine.GetRuntimeRenderTarget());
        };
        const auto grey = [](std::array<Uint8, 4> p) { return p[0] > 40 && p[0] < 215; };
        Surface sharp = draw_checker();
        Check(!grey(Pixel(sharp.get(), 4, 2)) && !grey(Pixel(sharp.get(), 3, 2)), "nearest sampling keeps the image's pixels");
        Renderer::SetSmoothImages(true);
        Surface smooth = draw_checker();
        Renderer::SetSmoothImages(false);
        Check(grey(Pixel(smooth.get(), 4, 2)), "smooth images blend neighbouring pixels");
        Surface again = draw_checker();
        Check(!grey(Pixel(again.get(), 4, 2)), "images loaded while smooth sample nearest again");
    }
    // Lua names the blend with the optional last argument.
    lua_State *state = luaL_newstate();
    luaL_openlibs(state);
    APIRegistration::Initialize(state);
    const auto lua = [&](const char *code) { return luaL_dostring(state, code) == LUA_OK; };
    Check(lua(R"(
        Image.DrawEx("red", 0, 0, 0, 1, 1, 0.5, 0.5, 255, 255, 255, 255, 0, nil, "add")
        Image.DrawEx("red", 0, 0, 0, 1, 1, 0.5, 0.5, 255, 255, 255, 255, 0, "fx", "normal")
        Image.DrawUIScaled("red", 0, 0, 1, 1, 255, 255, 255, 255, 0, "add")
        Image.DrawUIScaled("red", 0, 0, 1, 1, 255, 255, 255, 255, 0)
        Image.DrawEx("red", 0, 0, 0, 1, 1, 0.5, 0.5, 255, 255, 255, 255, 0, nil, "multiply")
    )"), "Image.DrawEx and DrawUIScaled take an optional blend");
    const RenderQueueState &queued = World::Current().State<RenderQueueState>();
    Check(queued.scene_draw_requests.size() == 3 && queued.scene_draw_requests[0].blend == ImageBlend::Add &&
              queued.scene_draw_requests[1].blend == ImageBlend::Normal &&
              queued.scene_draw_requests[2].blend == ImageBlend::Multiply &&
              queued.ui_draw_requests.size() == 2 && queued.ui_draw_requests[0].blend == ImageBlend::Add &&
              queued.ui_draw_requests[1].blend == ImageBlend::Normal,
          "Lua blend names reach the draw requests");
    Check(!lua(R"(Image.DrawUIScaled("red", 0, 0, 1, 1, 255, 255, 255, 255, 0, "screen"))"),
          "an unknown blend is an error");
    lua_close(state);
    Renderer::DiscardFrameRequests();
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
