#include "core/Engine.h"
#include "scene/Tilemap.h"
#include "scripting/ComponentManager.h"
#include "shared/resources/ResourcePath.h"
#include "SDL3_image/SDL_image.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>;
void Check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message + ": " + SDL_GetError());
}
std::array<Uint8, 4> Pixel(SDL_Surface *surface, int x, int y) {
    std::array<Uint8, 4> value{};
    Check(SDL_ReadSurfacePixel(surface, x, y, &value[0], &value[1], &value[2], &value[3]), "read pixels");
    return value;
}
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("pocket2d-map-render-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TilemapAsset::Data data;
    Fixture() {
        for (const auto *directory : {"maps", "images", "scenes"})
            std::filesystem::create_directories(root / directory);
        ResourcePath::SetResourcesRootPath(root);
        Surface atlas(SDL_CreateSurface(64, 32, SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface);
        Check(atlas != nullptr, "create atlas");
        const std::array<std::array<Uint8, 4>, 8> colors{{
            {200,20,30,255}, {20,200,30,255}, {20,30,200,255}, {200,100,30,255},
            {0,0,255,128}, {0,0,0,0}, {100,20,200,255}, {230,220,10,255}}};
        for (int id = 0; id < 8; ++id) {
            SDL_Rect rect{(id % 4) * 16, (id / 4) * 16, 16, 16};
            const auto &c = colors[id];
            Check(SDL_FillSurfaceRect(atlas.get(), &rect, SDL_MapSurfaceRGBA(atlas.get(), c[0], c[1], c[2], c[3])), "fill atlas");
        }
        // Tile 5 has a transparent two-pixel border around a half-alpha center.
        SDL_Rect transparent{0, 16, 16, 16};
        SDL_FillSurfaceRect(atlas.get(), &transparent, SDL_MapSurfaceRGBA(atlas.get(), 0, 0, 0, 0));
        SDL_Rect center{2, 18, 12, 12};
        SDL_FillSurfaceRect(atlas.get(), &center, SDL_MapSurfaceRGBA(atlas.get(), 0, 0, 255, 128));
        Check(IMG_SavePNG(atlas.get(), (root / "images/atlas.png").string().c_str()), "write atlas");
        Surface sprite(SDL_CreateSurface(16, 16, SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface);
        Check(sprite != nullptr, "create sprite ordering fixture");
        for (int index = 0; index < 2; ++index) {
            const auto &color = colors[index];
            SDL_FillSurfaceRect(sprite.get(), nullptr, SDL_MapSurfaceRGBA(sprite.get(), color[0], color[1], color[2], color[3]));
            Check(IMG_SavePNG(sprite.get(), (root / "images" / (index == 0 ? "first.png" : "second.png")).string().c_str()), "write ordering sprite");
        }
        data.width = 4; data.height = 4; data.atlas = {"atlas.png", 4, 2, 16, 16};
        data.ground.cells.assign(16, 1); data.solid.cells.assign(16, 0);
        data.ground.cells[1] = 2; data.ground.cells.back() = 8; data.solid.cells[0] = 5;
        TilemapAsset::ValidateAtlasImage(data, root / "images");
        TilemapAsset::SaveResource("render.tilemap.json", data);
        std::ofstream(root / "game.config") << R"({"game_title":"Tilemap pixel test","initial_scene":"render"})";
        std::ofstream(root / "rendering.config") << R"({"x_resolution":400,"y_resolution":400,"clear_color_r":3,"clear_color_g":7,"clear_color_b":11})";
        std::ofstream(root / "scenes/render.scene") << R"({"actors":[{"uid":1,"name":"map","components":{
            "map":{"type":"Tilemap","map_resource":"render.tilemap.json"},
            "transform":{"type":"Transform","x":0,"y":0}}}]})";
        std::ofstream(root / "scenes/order.scene") << R"({"actors":[
            {"uid":100,"name":"first","components":{
                "transform":{"type":"Transform","x":1,"y":1},
                "sprite":{"type":"SpriteRenderer","sprite":"first.png","scale_x":6.25,"scale_y":6.25,"pivot_x":0,"pivot_y":0,"sorting_order":0}}},
            {"uid":1,"name":"second","components":{
                "transform":{"type":"Transform","x":1,"y":1},
                "sprite":{"type":"SpriteRenderer","sprite":"second.png","scale_x":6.25,"scale_y":6.25,"pivot_x":0,"pivot_y":0,"sorting_order":0}}}
        ]})";
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
};
Surface Render(Engine &engine, int size = 400) {
    engine.SetRuntimeRenderTargetSize(size, size);
    engine.RunRenderFrozenFrame(false, false);
    Check(SDL_SetRenderTarget(engine.GetRenderer(), engine.GetRuntimeRenderTarget()), "select native map target");
    Surface image(SDL_RenderReadPixels(engine.GetRenderer(), nullptr), SDL_DestroySurface);
    Check(image != nullptr && image->w == size && image->h == size, "read native map target");
    Check(SDL_SetRenderTarget(engine.GetRenderer(), nullptr), "restore target");
    return image;
}
void VerifyColors(SDL_Surface *image, int scale) {
    Check(Pixel(image, 150 * scale, 50 * scale) == std::array<Uint8, 4>{20,200,30,255}, "atlas ID2 selects correct column");
    Check(Pixel(image, 350 * scale, 350 * scale) == std::array<Uint8, 4>{230,220,10,255}, "atlas ID8 selects final row/column");
    Check(Pixel(image, 6 * scale, 6 * scale) == std::array<Uint8, 4>{200,20,30,255}, "transparent tile border preserves ground");
    const auto blended = Pixel(image, 50 * scale, 50 * scale);
    Check(blended[0] >= 98 && blended[0] <= 101 && blended[1] >= 9 && blended[1] <= 11 &&
          blended[2] >= 141 && blended[2] <= 145 && blended[3] == 255,
          "solid alpha blends above ground in correct layer order");
}
} // namespace
int main(int argc, char **argv) {
    const std::string backend = argc > 1 ? argv[1] : "software";
    try {
        Fixture fixture;
        SDL_setenv_unsafe("POCKET_RENDER_DRIVER", backend.c_str(), 1);
        SDL_setenv_unsafe("POCKET_RENDER_VSYNC", "0", 1);
        Engine engine;
        Check(engine.GetLastSceneLoadError().empty(), engine.GetLastSceneLoadError());
        try { engine.InitializeRuntime(); }
        catch (const std::exception &error) {
            if (backend != "software") { std::cout << "SKIP " << backend << ": " << error.what() << '\n'; return 77; }
            throw;
        }
        engine.SetRenderRuntimeToTexture(true); engine.SetCameraPosition(2, 2);
        auto map_ref = ComponentManager::GetComponentByKey(1, "map");
        auto *map = map_ref.cast<Tilemap *>().value();
        auto image = Render(engine); VerifyColors(image.get(), 1);
        Check(map->GetDiagnostics().visible_tiles == 17, "full 4x4 map plus one solid overlay submits 17 draws");
        if (argc > 2) {
            const std::filesystem::path screenshot = argv[2];
            if (!screenshot.parent_path().empty()) std::filesystem::create_directories(screenshot.parent_path());
            Check(IMG_SavePNG(image.get(), screenshot.string().c_str()), "save map-only screenshot");
        }
        auto doubled = Render(engine, 800); VerifyColors(doubled.get(), 2);
        engine.SetCameraZoom(4); engine.SetCameraPosition(0.5f, 0.5f);
        auto zoomed_double = Render(engine, 800);
        Check(map->GetDiagnostics().visible_tiles == 2 && Pixel(zoomed_double.get(), 799, 400) ==
              std::array<Uint8, 4>{200,20,30,255}, "2x target preserves world culling and exact zoom boundary");
        engine.SetCameraZoom(1); engine.SetCameraPosition(2, 2);
        auto tied = fixture.data; tied.ground.sorting_order = 0;
        map->ReplaceAsset(tied); auto same_order = Render(engine); VerifyColors(same_order.get(), 1);
        engine.SetCameraZoom(4); engine.SetCameraPosition(0.5f, 0.5f);
        auto zoomed = Render(engine);
        Check(map->GetDiagnostics().visible_tiles == 2, "exact cell viewport excludes adjacent tiles");
        Check(Pixel(zoomed.get(), 399, 200) == std::array<Uint8, 4>{200,20,30,255}, "zoomed tile edge stays in first cell");
        engine.SetCameraZoom(1); engine.SetCameraPosition(4.5f, 2);
        auto edge = Render(engine);
        Check(Pixel(edge.get(), 200, 100) == std::array<Uint8, 4>{3,7,11,255}, "outside map extent stays clear");
        engine.SetCameraPosition(10, 10); auto outside = Render(engine);
        Check(map->GetDiagnostics().visible_tiles == 0 && Pixel(outside.get(), 200, 200) ==
              std::array<Uint8, 4>{3,7,11,255}, "outside camera submits no map draws");
        auto invalid = fixture.data; invalid.atlas.tile_width_px = 15;
        bool rejected = false;
        try { TilemapAsset::ValidateAtlasImage(invalid, fixture.root / "images"); }
        catch (const std::exception &) { rejected = true; }
        Check(rejected, "invalid atlas dimensions must reject instead of render fallback");
        engine.SetCameraPosition(2, 2); engine.RequestSceneLoad("order");
        auto ordered = Render(engine);
        Check(Pixel(ordered.get(), 150, 150) == std::array<Uint8, 4>{20,200,30,255},
              "same sorting-order sprites must preserve scene order, not UID/hash order");
        std::cout << "PASS tilemap native rendering " << backend << ": atlas/layers/alpha/culling/2x-target/zoom\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL tilemap render " << backend << ": " << error.what() << '\n'; return 1;
    }
}
