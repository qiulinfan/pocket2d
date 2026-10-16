// Bunnymark: N sprites, each moved by its own Lua OnUpdate with gravity and
// bouncing, rendered by a real SDL renderer. The rules are fixed so ports to
// other engines measure the same work (docs/development/benchmark-harness.md).
//   bunnymark_benchmark BACKEND COUNT [WARMUP] [SAMPLES] [per_entity|batch]
// "batch" runs the same rules through one OnUpdateBatch call per frame.
#include "core/Engine.h"
#include "shared/resources/ResourcePath.h"
#include "SDL3_image/SDL_image.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
void Check(bool condition, const std::string &message) { if (!condition) throw std::runtime_error(message); }
double ThreadCpuMs() {
#if defined(__APPLE__)
    thread_basic_info_data_t info{};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    const mach_port_t thread = mach_thread_self();
    thread_info(thread, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count);
    mach_port_deallocate(mach_task_self(), thread);
    return info.user_time.seconds * 1000.0 + info.user_time.microseconds / 1000.0 +
           info.system_time.seconds * 1000.0 + info.system_time.microseconds / 1000.0;
#else
    timespec now{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return now.tv_sec * 1000.0 + now.tv_nsec / 1e6;
#endif
}

struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("pocket-bunnymark-" + std::to_string(Clock::now().time_since_epoch().count()));
    Fixture(int count, bool batch) {
        for (const char *directory : {"images", "scenes", "component_types"})
            std::filesystem::create_directories(root / directory);
        SDL_Surface *bunny = SDL_CreateSurface(26, 37, SDL_PIXELFORMAT_RGBA32);
        Check(bunny != nullptr && SDL_FillSurfaceRect(bunny, nullptr, SDL_MapSurfaceRGBA(bunny, 240, 240, 240, 255)),
              "cannot create the bunny image");
        Check(IMG_SavePNG(bunny, (root / "images/bunny.png").string().c_str()), "cannot save the bunny image");
        SDL_DestroySurface(bunny);
        std::ofstream(root / "game.config") << R"({"initial_scene":"bunnies"})";
        // 960x540 pixels at the engine's 100 px per unit.
        std::ofstream(root / "rendering.config") << R"({"x_resolution":960,"y_resolution":540})";
        std::ostringstream scene;
        scene << R"({"actors":[)";
        for (int i = 0; i < count; ++i) {
            if (i) scene << ',';
            scene << R"({"uid":)" << i + 1 << R"(,"name":"bunny","components":{"transform":{"type":"Transform"},)"
                  << R"("sprite":{"type":"SpriteRenderer","sprite":"bunny.png"},"move":{"type":"Bunny"}}})";
        }
        scene << "]}";
        std::ofstream(root / "scenes/bunnies.scene") << scene.str();
        // Units: pixels per frame, converted to world units for the Transform.
        // A deterministic LCG seeded by actor index chooses the start velocity.
        std::ofstream(root / "component_types/Bunny.lua") << (batch ? R"(
local function Move(self)
    local x, y = self.x + self.vx, self.y + self.vy
    local vx, vy = self.vx, self.vy + 0.75
    if x > 960 then vx = -vx; x = 960 elseif x < 0 then vx = -vx; x = 0 end
    if y > 540 then vy = -0.85 * vy; y = 540 elseif y < 0 then vy = 0; y = 0 end
    self.x, self.y, self.vx, self.vy = x, y, vx, vy
    self.transform:SetLocalPosition(x / 100 - 4.8, y / 100 - 2.7)
end
Bunny = {
    OnStart = function(self)
        local seed = (self.actor:GetUID() * 1103515245 + 12345) % 2147483648
        self.vx = (seed % 1000) / 100
        seed = (seed * 1103515245 + 12345) % 2147483648
        self.vy = (seed % 1000) / 200 - 2.5
        self.x, self.y = 0, 0
        self.transform = self.actor:GetComponent("Transform")
    end,
    OnUpdateBatch = function(bunnies)
        for i = 1, #bunnies do Move(bunnies[i]) end
    end
}
)" : R"(
Bunny = {
    OnStart = function(self)
        local seed = (self.actor:GetUID() * 1103515245 + 12345) % 2147483648
        self.vx = (seed % 1000) / 100            -- 0..10 px/frame
        seed = (seed * 1103515245 + 12345) % 2147483648
        self.vy = (seed % 1000) / 200 - 2.5      -- -2.5..2.5 px/frame
        self.x, self.y = 0, 0
        self.transform = self.actor:GetComponent("Transform")
    end,
    OnUpdate = function(self)
        local x, y = self.x + self.vx, self.y + self.vy
        local vx, vy = self.vx, self.vy + 0.75
        if x > 960 then vx = -vx; x = 960 elseif x < 0 then vx = -vx; x = 0 end
        if y > 540 then vy = -0.85 * vy; y = 540 elseif y < 0 then vy = 0; y = 0 end
        self.x, self.y, self.vx, self.vy = x, y, vx, vy
        self.transform:SetLocalPosition(x / 100 - 4.8, y / 100 - 2.7)
    end
}
)");
        ResourcePath::SetResourcesRootPath(root);
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
};
} // namespace

int main(int argc, char **argv) {
    try {
        Check(argc >= 3, "usage: bunnymark_benchmark BACKEND COUNT [WARMUP] [SAMPLES]");
        const std::string backend = argv[1];
        const int count = std::stoi(argv[2]);
        const int warmup = argc > 3 ? std::stoi(argv[3]) : 30;
        const int samples = argc > 4 ? std::stoi(argv[4]) : 300;
        const std::string mode = argc > 5 ? argv[5] : "per_entity";
        Check(count >= 1 && count <= 200000 && warmup >= 0 && samples >= 1 &&
              (mode == "per_entity" || mode == "batch"), "invalid arguments");
        SDL_setenv_unsafe("POCKET_RENDER_DRIVER", backend.c_str(), 1);
        SDL_setenv_unsafe("POCKET_RENDER_VSYNC", "0", 1);
        Fixture fixture(count, mode == "batch");
        Engine engine;
        Check(engine.GetLastSceneLoadError().empty(), engine.GetLastSceneLoadError());
        engine.InitializeRuntime();
        std::vector<double> wall, cpu;
        auto frame = [&] {
            const double cpu_start = ThreadCpuMs();
            const auto start = Clock::now();
            engine.RunSingleFrame();
            engine.PresentFrame();
            wall.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
            cpu.push_back(ThreadCpuMs() - cpu_start);
        };
        for (int i = 0; i < warmup; ++i) frame();
        wall.clear(); cpu.clear();
        for (int i = 0; i < samples; ++i) frame();
        auto percentile = [](std::vector<double> values, double p) {
            std::sort(values.begin(), values.end());
            return values[std::min(values.size() - 1, static_cast<std::size_t>(std::ceil(p * values.size()) - 1))];
        };
        std::cout << "backend,mode,bunnies,warmup,samples,p50_ms,p95_ms,p99_ms,cpu_p50_ms,cpu_p95_ms\n"
                  << backend << ',' << mode << ',' << count << ',' << warmup << ',' << samples << ',' << percentile(wall, 0.5) << ','
                  << percentile(wall, 0.95) << ',' << percentile(wall, 0.99) << ',' << percentile(cpu, 0.5) << ','
                  << percentile(cpu, 0.95) << '\n';
        engine.ShutdownRuntime();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "bunnymark: " << error.what() << '\n';
        return 1;
    }
}
