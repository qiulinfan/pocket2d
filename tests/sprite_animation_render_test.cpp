#include "core/Engine.h"
#include "core/FrameClock.h"
#include "rendering/SpriteAnimation.h"
#include "rendering/SpriteRenderer.h"
#include "scripting/ComponentManager.h"
#include "shared/resources/ResourcePath.h"
#include "SDL3_image/SDL_image.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>;
using Color = std::array<Uint8,4>;
void Check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message + ": " + SDL_GetError());
}
Color Pixel(SDL_Surface *surface, int x, int y) {
    Color color{};
    Check(SDL_ReadSurfacePixel(surface,x,y,&color[0],&color[1],&color[2],&color[3]), "read pixel");
    return color;
}
void Fill(SDL_Surface *surface, SDL_Rect rect, Color color) {
    Check(SDL_FillSurfaceRect(surface, &rect, SDL_MapSurfaceRGBA(surface,color[0],color[1],color[2],color[3])), "fill atlas");
}
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("pocket-animation-render-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() {
        for (auto *directory : {"images", "animations", "scenes"}) std::filesystem::create_directories(root/directory);
        Surface atlas(SDL_CreateSurface(48,32,SDL_PIXELFORMAT_RGBA32),SDL_DestroySurface);
        Check(atlas != nullptr,"create atlas");
        Fill(atlas.get(), {0,0,48,32},{255,0,255,255});
        Fill(atlas.get(), {16,0,16,16},{40,180,220,255});
        Fill(atlas.get(), {32,16,8,8},{255,0,0,255});
        Fill(atlas.get(), {40,16,8,8},{0,255,0,255});
        Fill(atlas.get(), {32,24,8,8},{0,0,255,255});
        Fill(atlas.get(), {40,24,8,8},{255,255,255,255});
        Check(IMG_SavePNG(atlas.get(),(root/"images/atlas.png").string().c_str()), "save atlas");
        Surface base(SDL_CreateSurface(16,16,SDL_PIXELFORMAT_RGBA32),SDL_DestroySurface);
        Check(base != nullptr,"create base");
        Fill(base.get(),{0,0,16,16},{220,30,180,255});
        Check(IMG_SavePNG(base.get(),(root/"images/base.png").string().c_str()), "save base");
        std::ofstream(root/"animations/test.animation.json") << R"({"version":1,"image":"atlas.png","columns":3,"rows":2,"tile_width_px":16,"tile_height_px":16,"clips":[{"name":"walk","loop":true,"frames":[{"row":1,"column":2,"ticks":2},{"row":2,"column":3,"ticks":3}]}]})";
        std::ofstream(root/"game.config") << R"({"game_title":"SpriteAnimation pixel test","initial_scene":"test"})";
        std::ofstream(root/"rendering.config") << R"({"x_resolution":400,"y_resolution":400,"clear_color_r":3,"clear_color_g":7,"clear_color_b":11})";
        std::ofstream(root/"scenes/test.scene") << R"({"actors":[{"uid":1,"name":"animated","components":{
            "animation":{"type":"SpriteAnimation","animation_resource":"test.animation.json","renderer_key":"sprite","clip":"walk"},
            "sprite":{"type":"SpriteRenderer","sprite":"base.png","scale_x":4,"scale_y":4,"pivot_x":0.25,"pivot_y":0.75},
            "transform":{"type":"Transform","x":2,"y":2}}}]})";
        ResourcePath::SetResourcesRootPath(root);
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root,error); }
};
Surface Render(Engine &engine, int size=400) {
    engine.SetRuntimeRenderTargetSize(size,size);
    engine.RunRenderFrozenFrame(false,false);
    Check(SDL_SetRenderTarget(engine.GetRenderer(),engine.GetRuntimeRenderTarget()), "set read target");
    Surface result(SDL_RenderReadPixels(engine.GetRenderer(),nullptr),SDL_DestroySurface);
    Check(result != nullptr && result->w == size && result->h == size, "read target");
    Check(SDL_SetRenderTarget(engine.GetRenderer(),nullptr), "restore target");
    return result;
}
void Corners(SDL_Surface *surface, int scale, std::array<Color,4> colors) {
    const std::array<std::pair<int,int>,4> positions{{{192,160},{240,160},{192,208},{240,208}}};
    for (int i=0;i<4;++i) {
        const auto actual = Pixel(surface,positions[i].first*scale,positions[i].second*scale);
        Check(actual == colors[i], "atlas corner " + std::to_string(i) + " actual " +
            std::to_string(actual[0])+","+std::to_string(actual[1])+","+std::to_string(actual[2]));
    }
    Check(Pixel(surface,183*scale,160*scale) == Color{3,7,11,255}, "authored pivot left extent preserved");
    Check(Pixel(surface,192*scale,151*scale) == Color{3,7,11,255}, "authored pivot top extent preserved");
}
}
int main(int argc,char **argv) {
    const std::string backend = argc>1 ? argv[1] : "software";
    try {
        Fixture fixture;
        SDL_setenv_unsafe("POCKET_RENDER_DRIVER",backend.c_str(),1);
        SDL_setenv_unsafe("POCKET_RENDER_VSYNC","0",1);
        Engine engine;
        Check(engine.GetLastSceneLoadError().empty(),engine.GetLastSceneLoadError());
        try { engine.InitializeRuntime(); }
        catch (const std::exception &error) {
            if (backend != "software") { std::cout << "SKIP " << backend << ": " << error.what() << '\n'; return 77; }
            throw;
        }
        engine.SetRenderRuntimeToTexture(true); engine.SetCameraPosition(2,2);
        const auto animation_ref = ComponentManager::GetComponentByKey(1,"animation");
        const auto renderer_ref = ComponentManager::GetComponentByKey(1,"sprite");
        auto *animation = animation_ref.cast<SpriteAnimation *>().value();
        auto *renderer = renderer_ref.cast<SpriteRenderer *>().value();
        Check(animation->GetFrame() == 1 && renderer->HasAnimationOverride(), "initial pose exists before first tick");
        auto first = Render(engine);
        Corners(first.get(),1,{Color{40,180,220,255},Color{40,180,220,255},Color{40,180,220,255},Color{40,180,220,255}});
        for (int i=0;i<3;++i) Render(engine);
        Check(animation->GetFrame() == 1 && animation->GetElapsedTicks() == 0, "frozen rendering does not advance");
        engine.StepSimulation(); engine.StepSimulation();
        Check(animation->GetFrame() == 2 && animation->GetElapsedTicks() == 0, "native pipeline advances exact duration");
        auto second = Render(engine);
        Corners(second.get(),1,{Color{255,0,0,255},Color{0,255,0,255},Color{0,0,255,255},Color{255,255,255,255}});
        renderer->scale_x=-4; renderer->scale_y=-4;
        renderer->r=128; renderer->b=64;
        auto mirrored = Render(engine);
        Corners(mirrored.get(),1,{Color{128,255,64,255},Color{0,0,64,255},Color{0,255,0,255},Color{128,0,0,255}});
        auto doubled = Render(engine,800);
        Corners(doubled.get(),2,{Color{128,255,64,255},Color{0,0,64,255},Color{0,255,0,255},Color{128,0,0,255}});
        renderer->a=128;
        auto alpha = Render(engine);
        const auto blended = Pixel(alpha.get(),192,160);
        // SDL backend integer blending may round by one; atlas selection above is exact.
        const std::array<int,3> expected{{66,131,38}};
        for (int i=0;i<3;++i) Check(std::abs(static_cast<int>(blended[i])-expected[i]) <= 2,"authored alpha preserved");
        renderer->a=255; renderer->r=renderer->b=255;
        animation->enabled=false;
        engine.StepSimulation(); engine.StepSimulation();
        auto disabled = Render(engine);
        Check(animation->GetFrame() == 2 && animation->GetElapsedTicks() == 0,"disabled freezes animation");
        Corners(disabled.get(),1,{Color{255,255,255,255},Color{0,0,255,255},Color{0,255,0,255},Color{255,0,0,255}});
        animation->Stop();
        auto stopped=Render(engine);
        Corners(stopped.get(),1,{Color{220,30,180,255},Color{220,30,180,255},Color{220,30,180,255},Color{220,30,180,255}});
        Check(!renderer->HasAnimationOverride() && renderer->sprite == "base.png" && renderer->sprite_row == 1 && renderer->sprite_column == 1,
              "stop restores unmodified author image and cell");
        animation->enabled=true; animation->Play("walk");
        animation->ClearAsset();
        auto cleared=Render(engine);
        Corners(cleared.get(),1,{Color{220,30,180,255},Color{220,30,180,255},Color{220,30,180,255},Color{220,30,180,255}});
        Check(!renderer->HasAnimationOverride() && !animation->HasAsset() && animation->GetFrame() == 0,
              "ClearAsset clears owned pose and restores author rendering");
        animation->Initialize(SpriteAnimationAsset::LoadResource("test.animation.json"));
        auto reconfigured=Render(engine);
        Corners(reconfigured.get(),1,{Color{40,180,220,255},Color{40,180,220,255},Color{40,180,220,255},Color{40,180,220,255}});
        Check(renderer->HasAnimationOverride() && animation->GetFrame() == 1,
              "resource-cleared component can acquire a new pose");
        animation->OnDestroy();
        auto destroyed=Render(engine);
        Corners(destroyed.get(),1,{Color{220,30,180,255},Color{220,30,180,255},Color{220,30,180,255},Color{220,30,180,255}});
        Check(!renderer->HasAnimationOverride(),"destroy clears only animator override");
        if (argc>2) {
            std::filesystem::path output=argv[2];
            if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path());
            Check(IMG_SavePNG(mirrored.get(),output.string().c_str()),"save animation screenshot");
        }
        Check(!std::filesystem::exists(fixture.root/".pocketengine"),"animation grid must not create editor metadata");
        Check(ComponentManager::GetRuntimeErrorCount()==0,"runtime Lua errors");
        std::cout << "PASS SpriteAnimation rendering " << backend << ": atlas/pivot/mirror/tint/alpha/frozen/stop\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << "FAIL SpriteAnimation render " << backend << ": " << error.what() << '\n'; return 1; }
}
