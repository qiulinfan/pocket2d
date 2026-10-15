#include "core/Engine.h"
#include "scene/Tilemap.h"
#include "rendering/SpriteRenderer.h"
#include "scripting/ComponentManager.h"
#include "shared/config/SpritesheetConfig.h"
#include "shared/resources/ResourcePath.h"
#include "SDL3_image/SDL_image.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
double Ms(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
void Check(bool condition, const std::string &message) { if (!condition) throw std::runtime_error(message); }
std::uint64_t ResidentBytes() {
#if defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return info.resident_size;
#endif
    return 0; // Unavailable, never interpreted as zero memory use.
}
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("pocket2d-map-bench-" + std::to_string(Clock::now().time_since_epoch().count()));
    TilemapAsset::Data data;
    explicit Fixture(int size) {
        for (const auto *directory : {"maps", "images", "scenes"}) std::filesystem::create_directories(root / directory);
        ResourcePath::SetResourcesRootPath(root);
        auto *atlas = SDL_CreateSurface(64, 32, SDL_PIXELFORMAT_RGBA32);
        Check(atlas != nullptr, "create benchmark atlas");
        for (int id = 0; id < 8; ++id) {
            SDL_Rect rect{(id % 4) * 16, (id / 4) * 16, 16, 16};
            SDL_FillSurfaceRect(atlas, &rect, SDL_MapSurfaceRGBA(atlas, 30 + id * 25, 190 - id * 15, 40 + id * 20, 255));
        }
        Check(IMG_SavePNG(atlas, (root / "images/atlas.png").string().c_str()), "save atlas"); SDL_DestroySurface(atlas);
        Check(SpritesheetConfig::WriteForImagePath(root, root / "images/atlas.png", {2,4}), "prepare original SpriteRenderer grid metadata");
        data.width = size; data.height = size; data.atlas = {"atlas.png",4,2,16,16};
        data.ground.cells.resize(static_cast<std::size_t>(size) * size); data.solid.cells.assign(data.ground.cells.size(), 0);
        for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) data.ground.cells[y * size + x] = 1 + (x + y) % 2;
        TilemapAsset::ValidateAtlasImage(data, root / "images"); TilemapAsset::SaveResource("bench.tilemap.json", data);
        std::ofstream(root / "rendering.config") << R"({"x_resolution":640,"y_resolution":360,"clear_color_r":3,"clear_color_g":7,"clear_color_b":11})";
        std::ofstream(root / "scenes/native.scene") << R"({"actors":[{"uid":1,"name":"map","components":{
            "map":{"type":"Tilemap","map_resource":"bench.tilemap.json"},"transform":{"type":"Transform"}}}]})";
        std::ofstream baseline(root / "scenes/sprites.scene"); baseline << "{\"actors\":[";
        for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
            const int index = y * size + x;
            if (index) baseline << ',';
            baseline << "{\"uid\":" << index + 1 << ",\"name\":\"tile\",\"components\":{"
                     << "\"transform\":{\"type\":\"Transform\",\"x\":" << x << ",\"y\":" << y << "},"
                     << "\"sprite\":{\"type\":\"SpriteRenderer\",\"sprite\":\"atlas.png\",\"pivot_x\":0,\"pivot_y\":0,"
                     << "\"scale_x\":6.25,\"scale_y\":6.25,\"sorting_order\":-100,\"sprite_column\":" << data.ground.cells[index] << "}}}";
        }
        baseline << "]}";
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    void Select(const char *scene) { std::ofstream(root / "game.config") << "{\"game_title\":\"Tilemap benchmark\",\"initial_scene\":\"" << scene << "\"}"; }
};
std::vector<unsigned char> ReadPixels(Engine &engine) {
    auto *renderer = engine.GetRenderer();
    Check(SDL_SetRenderTarget(renderer, engine.GetRuntimeRenderTarget()), "select benchmark target");
    auto *read = SDL_RenderReadPixels(renderer, nullptr); Check(read != nullptr, "read benchmark pixels");
    auto *rgba = SDL_ConvertSurface(read, SDL_PIXELFORMAT_RGBA32); SDL_DestroySurface(read);
    Check(rgba != nullptr, "convert benchmark pixels");
    std::vector<unsigned char> result(static_cast<std::size_t>(rgba->w) * rgba->h * 4);
    for (int y = 0; y < rgba->h; ++y)
        std::memcpy(result.data() + static_cast<std::size_t>(y) * rgba->w * 4,
                    static_cast<unsigned char *>(rgba->pixels) + y * rgba->pitch, static_cast<std::size_t>(rgba->w) * 4);
    SDL_DestroySurface(rgba); SDL_SetRenderTarget(renderer, nullptr); return result;
}
struct Result { std::vector<unsigned char> pixels; int samples = 0; };
std::vector<Result> Measure(Fixture &fixture, bool native, int requested_samples,
                            const std::vector<Result> *reference = nullptr) {
    fixture.Select(native ? "native" : "sprites");
    const auto cold = Clock::now();
    Engine engine; Check(engine.GetLastSceneLoadError().empty(), engine.GetLastSceneLoadError());
    engine.InitializeRuntime(); engine.SetRenderRuntimeToTexture(true); engine.SetRuntimeRenderTargetSize(640,360);
    engine.SetCameraPosition(4,4); engine.RunRenderFrozenFrame(false,false);
    const double cold_ms = Ms(cold);
    if (cold_ms > 30000) throw std::runtime_error("cold initialization exceeded 30s workload budget; no speedup claim");
    Tilemap *map = native ? ComponentManager::GetComponentByKey(1,"map").cast<Tilemap *>().value() : nullptr;
    std::vector<SpriteRenderer *> sprites;
    if (!native)
        for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x)
            sprites.push_back(ComponentManager::GetComponentByKey(y * fixture.data.width + x + 1,"sprite").cast<SpriteRenderer *>().value());
    std::vector<Result> output;
    int camera_tick = 0;
    for (int scenario = 0; scenario < 4; ++scenario) {
        const char *label = scenario == 0 ? "fixed" : scenario == 1 ? "scrolling" : scenario == 2 ? "edit1" : "edit64";
        const int edits = scenario < 2 ? 0 : scenario == 2 ? 1 : 64;
        auto frame = [&] {
            const int id = 3 + camera_tick % 2;
            for (int i = 0; i < edits; ++i) {
                const int index = edits == 1 ? 27 : i; // single-cell case is inside the fixed viewport
                if (native) map->SetCell("ground", index % 8, index / 8, id);
                else sprites[index]->sprite_column = id;
            }
            engine.SetCameraPosition(scenario == 1 ? 4.0f + static_cast<float>(std::sin(camera_tick * 0.1)) * 2 : 4.0f, 4.0f);
            ++camera_tick;
            engine.RunRenderFrozenFrame(false,false);
            Check(SDL_RenderTexture(engine.GetRenderer(), engine.GetRuntimeRenderTarget(), nullptr, nullptr), "present map texture");
            Check(SDL_RenderPresent(engine.GetRenderer()), "present benchmark frame");
        };
        for (int i = 0; i < 3; ++i) frame();
        if (map) map->ResetDiagnostics();
        std::vector<double> timings;
        const int sample_count = reference ? (*reference)[scenario].samples : requested_samples;
        const auto budget_start = Clock::now();
        for (int i = 0; i < sample_count; ++i) {
            const auto start = Clock::now(); frame(); timings.push_back(Ms(start));
            if (Ms(budget_start) > 30000 && !reference) break;
        }
        Result result{ReadPixels(engine), static_cast<int>(timings.size())};
        if (reference && result.pixels != (*reference)[scenario].pixels) {
            const auto &expected = (*reference)[scenario].pixels;
            std::size_t mismatches = 0;
            for (std::size_t pixel = 0; pixel < result.pixels.size() / 4; ++pixel)
                if (std::memcmp(result.pixels.data() + pixel * 4, expected.data() + pixel * 4, 4) != 0) {
                    if (mismatches < 4)
                        std::cerr << "pixel mismatch " << pixel % 640 << ',' << pixel / 640 << " native="
                                  << static_cast<int>(result.pixels[pixel*4]) << ',' << static_cast<int>(result.pixels[pixel*4+1]) << ','
                                  << static_cast<int>(result.pixels[pixel*4+2]) << " sprite=" << static_cast<int>(expected[pixel*4]) << ','
                                  << static_cast<int>(expected[pixel*4+1]) << ',' << static_cast<int>(expected[pixel*4+2]) << '\n';
                    ++mismatches;
                }
            throw std::runtime_error("native/SpriteRenderer final pixels differ for " + std::string(label) +
                                     " (" + std::to_string(mismatches) + " pixels)");
        }
        if (map && edits) Check(map->GetCell("ground", edits == 1 ? 3 : 0, edits == 1 ? 3 : 0) ==
                               3 + (camera_tick - 1) % 2, "committed edit mismatch");
        std::sort(timings.begin(),timings.end());
        auto percentile = [&](double p) { return timings[std::min(timings.size()-1, static_cast<std::size_t>(std::ceil(p*timings.size())-1))]; };
        const auto diagnostics = map ? map->GetDiagnostics() : Tilemap::Diagnostics{};
        std::cout << (native ? "native" : "sprites") << ',' << fixture.data.width << ',' << label << ','
                  << timings.size() << ',' << cold_ms << ',' << percentile(0.50) << ',' << percentile(0.95) << ',' << percentile(0.99) << ','
                  << (native ? diagnostics.visible_tiles : fixture.data.ground.cells.size()) << ','
                  << diagnostics.render_chunk_rebuilds << ',' << diagnostics.collision_chunk_rebuilds << ',' << ResidentBytes() << ','
                  << (timings.size() == static_cast<std::size_t>(requested_samples) ? "complete" : "budget_limited") << '\n';
        output.push_back(std::move(result));
    }
    return output;
}
} // namespace
int main(int argc, char **argv) {
    try {
        const int size = argc > 1 ? std::stoi(argv[1]) : 64;
        const std::string backend = argc > 2 ? argv[2] : "software";
        const int samples = argc > 3 ? std::stoi(argv[3]) : 21;
        Check(size >= 64 && size <= 512 && samples > 0 && samples <= 1000, "usage: tilemap_benchmark SIZE[64..512] BACKEND SAMPLES[1..1000]");
        SDL_setenv_unsafe("POCKET_RENDER_DRIVER", backend.c_str(), 1); SDL_setenv_unsafe("POCKET_RENDER_VSYNC","0",1);
        Fixture fixture(size);
        std::cout << "# actual SDL3 render+present; ground-only/no physics; identical atlas/cells/view/output; exact pixels checked\n"
                  << "# warmup=3; GPU timestamps/allocations not instrumented; rss_bytes=0 means unavailable\n"
                  << "# rss is a same-process snapshot (allocator retention/reference pixels remain), not an isolated memory comparison\n"
                  << "path,size,scenario,samples,cold_ms,p50_ms,p95_ms,p99_ms,queued_requests_last,render_rebuilds,collision_rebuilds,rss_bytes,status\n";
        const auto baseline = Measure(fixture,false,samples); Measure(fixture,true,samples,&baseline);
        return 0;
    } catch (const std::exception &error) { std::cerr << "tilemap benchmark: " << error.what() << '\n'; return 1; }
}
