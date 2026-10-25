// Full headless Engine::StepSimulation comparison, not an animation-kernel timer.
// Defaults: 1k and 10k actors, 8 warmup ticks, 60 measured ticks, 30s per case.
// Example smoke run: sprite_animation_benchmark --actors 10 --warmup 2 --samples 4
// Each step is indivisible: a single slow initialization/step may exceed the
// cooperative budget. All actor states are checked outside the measured region.
#include "core/Engine.h"
#include "core/FrameClock.h"
#include "rendering/SpriteAnimation.h"
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
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
double Ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
void Check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}
struct Options {
    std::vector<int> actors{1000, 10000};
    int warmup = 8;
    int samples = 60;
    int budget_ms = 30000;
};
int Number(const std::string &text, int minimum, int maximum) {
    std::size_t end = 0;
    const int value = std::stoi(text, &end);
    Check(end == text.size() && value >= minimum && value <= maximum, "invalid numeric argument: " + text);
    return value;
}
Options Parse(int argc, char **argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        Check(i + 1 < argc, "missing value for " + option);
        const std::string value = argv[++i];
        if (option == "--actors") {
            options.actors.clear();
            std::istringstream values(value);
            std::string count;
            while (std::getline(values, count, ',')) options.actors.push_back(Number(count, 1, 100000));
            Check(!options.actors.empty() && value.back() != ',', "--actors needs comma-separated positive counts");
        } else if (option == "--warmup") options.warmup = Number(value, 0, 10000);
        else if (option == "--samples") options.samples = Number(value, 1, 10000);
        else if (option == "--budget-ms") options.budget_ms = Number(value, 1, 300000);
        else throw std::runtime_error("unknown option " + option);
    }
    return options;
}
void Write(const std::filesystem::path &path, const std::string &contents) {
    std::ofstream file(path);
    file << contents;
    file.close();
    Check(file.good(), "cannot write benchmark fixture " + path.string());
}
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("pocket-animation-benchmark-" + std::to_string(Clock::now().time_since_epoch().count()));
    explicit Fixture(int count) {
        for (const auto *directory : {"images", "animations", "component_types", "scenes"})
            std::filesystem::create_directories(root / directory);
        using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>;
        Surface atlas(SDL_CreateSurface(32, 16, SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface);
        Check(atlas != nullptr, "cannot create benchmark atlas");
        SDL_Rect left{0, 0, 16, 16}, right{16, 0, 16, 16};
        Check(SDL_FillSurfaceRect(atlas.get(), &left, SDL_MapSurfaceRGBA(atlas.get(), 220, 40, 20, 255)) &&
              SDL_FillSurfaceRect(atlas.get(), &right, SDL_MapSurfaceRGBA(atlas.get(), 20, 80, 220, 255)), "cannot paint atlas");
        Check(IMG_SavePNG(atlas.get(), (root / "images/atlas.png").string().c_str()), "cannot save benchmark atlas");
        // Both paths select the same 16px cells. Lua uses the existing authored
        // SpriteRenderer grid; native uses the asset's explicit matching grid.
        Check(SpritesheetConfig::WriteForImagePath(root, root / "images/atlas.png", {1, 2}), "cannot write grid metadata");
        Write(root / "animations/loop.animation.json", R"({"version":1,"image":"atlas.png","columns":2,"rows":1,"tile_width_px":16,"tile_height_px":16,"clips":[{"name":"loop","loop":true,"frames":[{"row":1,"column":1,"ticks":2},{"row":1,"column":2,"ticks":2}]}]})");
        // Deliberately a small Lua-only loop. It does not implement native
        // markers, completion, playback IDs, pause/restart, or transient poses.
        // Neither scene subscribes to animation events. Native still performs
        // its production event-generation/dispatch bookkeeping during timing.
        Write(root / "component_types/BenchmarkAnimation.lua", R"(
BenchmarkAnimation = {
    frame = 1, elapsed_ticks = 0,
    OnStart = function(self)
        self.sprite = self.actor:GetComponentByKey("sprite")
    end,
    OnUpdate = function(self)
        self.elapsed_ticks = self.elapsed_ticks + 1
        if self.elapsed_ticks == 2 then
            self.elapsed_ticks = 0
            self.frame = 3 - self.frame
            self.sprite.sprite_column = self.frame
        end
    end
}
)");
        for (const bool native : {true, false}) {
            std::ostringstream scene;
            scene << "{\"actors\":[";
            for (int i = 0; i < count; ++i) {
                if (i != 0) scene << ',';
                scene << "{\"uid\":" << i + 1 << ",\"name\":\"actor\",\"components\":{"
                      << "\"transform\":{\"type\":\"Transform\",\"x\":" << i % 100 << ",\"y\":" << i / 100 << "},"
                      << "\"sprite\":{\"type\":\"SpriteRenderer\",\"sprite\":\"atlas.png\",\"sprite_row\":1,\"sprite_column\":1},"
                      << "\"animation\":{";
                if (native) scene << "\"type\":\"SpriteAnimation\",\"animation_resource\":\"loop.animation.json\",\"renderer_key\":\"sprite\",\"clip\":\"loop\"";
                else scene << "\"type\":\"BenchmarkAnimation\"";
                scene << "}}}";
            }
            scene << "]}";
            Write(root / "scenes" / (native ? "native.scene" : "lua.scene"), scene.str());
        }
        Write(root / "rendering.config", R"({"x_resolution":640,"y_resolution":360})");
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    void Select(bool native) {
        ResourcePath::SetResourcesRootPath(root);
        Write(root / "game.config", std::string("{\"game_title\":\"SpriteAnimation step benchmark\",\"initial_scene\":\"") +
              (native ? "native" : "lua") + "\"}");
    }
};
struct State {
    int row = 1, column = 1, elapsed_ticks = 0;
    bool operator==(const State &) const = default;
};
struct Result {
    bool native = false;
    int actors = 0, warmup = 0, measured = 0, ticks = 0;
    double init_ms = 0, workload_ms = 0;
    bool partial = false;
    std::vector<double> times_ms;
    std::vector<State> final_states;
};
Result Measure(Fixture &fixture, int actor_count, bool native, const Options &options) {
    fixture.Select(native);
    Result result;
    result.native = native; result.actors = actor_count;
    result.times_ms.reserve(options.samples);
    const auto start = Clock::now();
    Engine engine;
    Check(engine.GetLastSceneLoadError().empty(), engine.GetLastSceneLoadError());
    engine.InitializeHeadless();
    result.init_ms = Ms(start);
    for (int tick = 0; tick < options.warmup; ++tick) {
        if (Ms(start) >= options.budget_ms) break;
        engine.StepSimulation();
        ++result.warmup;
    }
    if (result.warmup == options.warmup) {
        for (int tick = 0; tick < options.samples; ++tick) {
            if (Ms(start) >= options.budget_ms) break;
            const auto before = Clock::now();
            engine.StepSimulation();
            result.times_ms.push_back(Ms(before));
            ++result.measured;
        }
    }
    result.workload_ms = Ms(start);
    result.ticks = result.warmup + result.measured;
    result.partial = result.warmup != options.warmup || result.measured != options.samples ||
                     result.workload_ms >= options.budget_ms;
    // No reads/assertions were inserted inside timed StepSimulation calls.
    Check(FrameClock::GetFrameNumber() == result.ticks, "step clock mismatch");
    Check(ComponentManager::GetRuntimeErrorCount() == 0, ComponentManager::GetLastRuntimeError());
    const int expected_column = 1 + (result.ticks / 2) % 2;
    const int expected_elapsed = result.ticks % 2;
    result.final_states.reserve(actor_count);
    for (int i = 0; i < actor_count; ++i) {
        const auto sprite_ref = ComponentManager::GetComponentByKey(i + 1, "sprite");
        Check(sprite_ref.isInstance<SpriteRenderer>(), "missing SpriteRenderer actor " + std::to_string(i + 1));
        const auto *sprite = sprite_ref.cast<SpriteRenderer *>().value();
        const auto animation_ref = ComponentManager::GetComponentByKey(i + 1, "animation");
        State state;
        if (native) {
            Check(animation_ref.isInstance<SpriteAnimation>(), "missing native animation");
            const auto *animation = animation_ref.cast<SpriteAnimation *>().value();
            // In this exact asset frame N selects row 1, column N. The render
            // tests verify override pixel selection separately; this benchmark
            // verifies every logical pose and keeps GPU work out of timing.
            state.column = animation->GetFrame();
            state.elapsed_ticks = animation->GetElapsedTicks();
            Check(animation->GetCurrentClip() == "loop" && animation->IsPlaying() &&
                  sprite->HasAnimationOverride() && sprite->sprite_row == 1 && sprite->sprite_column == 1,
                  "native playback or transient-pose contract changed");
        } else {
            state.row = sprite->sprite_row;
            state.column = sprite->sprite_column;
            state.elapsed_ticks = animation_ref["elapsed_ticks"].cast<int>().value();
            Check(animation_ref["frame"].cast<int>().value() == state.column && !sprite->HasAnimationOverride(),
                  "Lua loop frame differs from authored sprite cell");
        }
        Check(state.row == 1 && state.column == expected_column && state.elapsed_ticks == expected_elapsed,
              std::string(native ? "native" : "lua") + " actor " + std::to_string(i + 1) + " final timeline mismatch");
        result.final_states.push_back(state);
    }
    return result;
}
std::string Percentile(std::vector<double> values, double quantile) {
    if (values.empty()) return "NA";
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(std::ceil(values.size() * quantile)) - 1;
    std::ostringstream output;
    output << std::fixed << std::setprecision(6) << values[index];
    return output.str();
}
void Print(const Result &result, const Options &options, const char *paired) {
#ifdef NDEBUG
    constexpr const char *build = "release";
#else
    constexpr const char *build = "debug";
#endif
    std::cout << build << ',' << (result.native ? "native" : "lua_loop") << ',' << result.actors << ','
              << (result.partial ? "partial" : "complete") << ',' << options.budget_ms << ','
              << options.warmup << ',' << result.warmup << ',' << options.samples << ',' << result.measured << ','
              << result.ticks << ',' << result.init_ms << ',' << result.workload_ms << ','
              << Percentile(result.times_ms, 0.5) << ',' << Percentile(result.times_ms, 0.95) << ','
              << result.final_states.size() << ',' << paired << '\n' << std::flush;
}
}
int main(int argc, char **argv) {
    try {
        const auto options = Parse(argc, argv);
        std::cout << std::fixed << std::setprecision(6)
                  << "build,mode,actors,status,budget_ms,warmup_requested,warmup_completed,samples_requested,samples_completed,"
                     "total_ticks,init_ms,workload_ms,step_p50_ms,step_p95_ms,verified_actors,paired_final_state\n";
        for (const int count : options.actors) {
            Fixture fixture(count);
            const auto native = Measure(fixture, count, true, options);
            const auto lua = Measure(fixture, count, false, options);
            const char *paired = "not_comparable_different_ticks";
            if (native.ticks == lua.ticks) {
                Check(native.final_states == lua.final_states, "native and Lua final actor cells/ticks differ");
                paired = "equal";
            }
            // Publish timings only after every actor in both cases is verified.
            Print(native, options, paired);
            Print(lua, options, paired);
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "sprite_animation_benchmark: " << error.what() << '\n';
        return 1;
    }
}
