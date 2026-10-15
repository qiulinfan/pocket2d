#include "core/Engine.h"
#include "input/Input.h"
#include "physics/Rigidbody.h"
#include <cmath>
#include "scripting/ComponentManager.h"
#include "shared/resources/ResourcePath.h"
#include "SDL3_image/SDL_image.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>;
using Color = std::array<Uint8, 4>;
void Check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message + ": " + SDL_GetError());
}
Surface Read(Engine& engine) {
    Check(ComponentManager::GetRuntimeErrorCount() == 0, ComponentManager::GetLastRuntimeError());
    Check(SDL_SetRenderTarget(engine.GetRenderer(), engine.GetRuntimeRenderTarget()), "select target");
    Surface image(SDL_RenderReadPixels(engine.GetRenderer(), nullptr), SDL_DestroySurface);
    Check(image && image->w == 960 && image->h == 720, "read two-world target");
    Check(SDL_SetRenderTarget(engine.GetRenderer(), nullptr), "restore target");
    return image;
}
Color Pixel(SDL_Surface* image, int x, int y) {
    Color value{};
    Check(SDL_ReadSurfacePixel(image, x, y, &value[0], &value[1], &value[2], &value[3]), "read pixel");
    return value;
}
int Count(SDL_Surface* image, Color color, int x0, int y0, int x1, int y1) {
    int count = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            count += Pixel(image, x, y) == color;
    return count;
}
void Key(Engine& engine, SDL_Scancode code, bool down) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.scancode = code; event.key.down = down;
    Input::ProcessEvent(event);
    engine.RunSingleFrame(false);
}
}

namespace {
void Controls(Engine& engine, int x, int y, bool attack = false) {
    for (auto [code, down] : std::array<std::pair<SDL_Scancode, bool>, 5>{{
             {SDL_SCANCODE_LEFT, x < 0}, {SDL_SCANCODE_RIGHT, x > 0},
             {SDL_SCANCODE_UP, y < 0}, {SDL_SCANCODE_DOWN, y > 0},
             {SDL_SCANCODE_SPACE, attack}}}) {
        SDL_Event e{}; e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.scancode = code; e.key.down = down; Input::ProcessEvent(e);
    }
    engine.RunSingleFrame(false);
}
void MoveTo(Engine& engine, float x, float y, bool attack = false) {
    auto* body = ComponentManager::GetComponentByKey(2, "body").cast<Rigidbody*>().value();
    for (int i = 0; i < 600; ++i) {
        auto p = body->GetPosition();
        const float dx = x - p.x, dy = y - p.y;
        if (std::abs(dx) <= .08f && std::abs(dy) <= .08f) { Controls(engine, 0, 0, attack); return; }
        Controls(engine, std::abs(dx) <= .08f ? 0 : dx > 0 ? 1 : -1,
                 std::abs(dy) <= .08f ? 0 : dy > 0 ? 1 : -1, attack);
        Check(ComponentManager::GetRuntimeErrorCount() == 0, ComponentManager::GetLastRuntimeError());
    }
    throw std::runtime_error("player could not navigate to rendered landmark");
}
}

int main(int argc, char** argv) {
    try {
        Check(argc >= 2, "fixture path required");
        const std::string backend = argc > 2 ? argv[2] : "software";
        ResourcePath::SetResourcesRootPath(argv[1]);
        SDL_setenv_unsafe("POCKET_RENDER_DRIVER", backend.c_str(), 1);
        SDL_setenv_unsafe("POCKET_RENDER_VSYNC", "0", 1);
        Engine engine;
        Check(engine.GetLastSceneLoadError().empty(), engine.GetLastSceneLoadError());
        engine.InitializeRuntime();
        Check(std::string(SDL_GetRendererName(engine.GetRenderer())) == backend, "requested renderer selected");
        Input::SetExternalInputEnabled(true);
        engine.SetRenderRuntimeToTexture(true); engine.SetRuntimeRenderTargetSize(960, 720);
        engine.RunSingleFrame(false);
        auto state = ComponentManager::GetComponentByKey(1, "state");
        auto initial = Read(engine);
        Check(Pixel(initial.get(), 22, 18) == Color{42, 101, 66, 255}, "village badge rendered");
        Check(Pixel(initial.get(), 602, 18) == Color{112, 74, 36, 255}, "open route badge rendered");
        Check(Count(initial.get(), Color{240, 196, 150, 255}, 80, 110, 880, 610) > 8,
              "real player visible on village map");
        Check(Pixel(initial.get(), 40, 657) == Color{239, 197, 100, 255}, "seed icon rendered");
        Check(Pixel(initial.get(), 224, 660) == Color{112, 194, 89, 255}, "food icon rendered");
        Check(Pixel(initial.get(), 403, 654) == Color{220, 230, 238, 255}, "sword icon rendered");
        Key(engine, SDL_SCANCODE_1, true); Key(engine, SDL_SCANCODE_1, false);
        Check(state["attack"].cast<int>().value() == 3, "real input equips sword");
        MoveTo(engine, 4.75f, 2.5f);
        for (int i = 0; i < 20 && state["transition_remaining"].cast<int>().value() == 0; ++i)
            Controls(engine, 1, 0);
        Check(state["transition_remaining"].cast<int>().value() > 0, "entered real portal");
        bool saw_blackout = false;
        for (int i = 0; i < 60 && state["transition_remaining"].cast<int>().value() > 0; ++i) {
            Controls(engine, 0, 0);
            auto frame = Read(engine);
            saw_blackout = saw_blackout || Pixel(frame.get(), 22, 18) == Color{0, 0, 0, 255};
        }
        Check(saw_blackout, "portal fade visibly reaches black");
        auto underground = Read(engine);
        Check(state["world_id"].cast<std::string>().value() == "underworld" &&
              Pixel(underground.get(), 22, 18) == Color{84, 53, 122, 255}, "world badge follows actual teleport");
        Check(Count(underground.get(), Color{240, 196, 150, 255}, 80, 110, 880, 610) > 8,
              "player visible after camera warp");
        MoveTo(engine, 26.4f, -.5f, true);
        for (int i = 0; i < 80 && !state["route_closed"].cast<bool>().value(); ++i)
            Controls(engine, 0, 0, true);
        Check(state["route_closed"].cast<bool>().value(), "real attack closes invasion route");
        Controls(engine, 0, 0);
        auto closed = Read(engine);
        Check(Pixel(closed.get(), 602, 18) == Color{28, 112, 104, 255}, "permanent closure badge rendered");
        Check(state["phase"].cast<std::string>().value() == "playing", "graphical route completed alive");
        if (argc > 3) {
            const std::filesystem::path root(argv[3]); std::filesystem::create_directories(root);
            Check(IMG_SavePNG(initial.get(), (root / "village.png").string().c_str()), "capture village");
            Check(IMG_SavePNG(underground.get(), (root / "underworld.png").string().c_str()), "capture underworld");
            Check(IMG_SavePNG(closed.get(), (root / "closed-route.png").string().c_str()), "capture closure");
        }
        std::cout << "PASS two-world " << backend << ": real input, maps, player, portal fade/camera and closure HUD\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL two-world render: " << e.what() << '\n'; return 1;
    }
}
