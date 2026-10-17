// Text rendering cost: a Lua HUD draws static and per-frame-changing strings
// through Text.Draw on a real renderer. Reports wall and main-thread CPU time
// per frame; optionally writes the last frame as PNG for pixel comparison.
//   text_benchmark BACKEND [SAMPLES] [CAPTURE.png]
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
#include <stdexcept>
#include <string>
#include <vector>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
double Ms(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
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
        ("pocket-text-benchmark-" + std::to_string(Clock::now().time_since_epoch().count()));
    explicit Fixture(const std::filesystem::path &font) {
        for (const char *directory : {"fonts", "scenes", "component_types"})
            std::filesystem::create_directories(root / directory);
        std::filesystem::copy_file(font, root / "fonts/hud.ttf");
        std::ofstream(root / "game.config") << R"({"initial_scene":"hud"})";
        std::ofstream(root / "rendering.config") << R"({"x_resolution":960,"y_resolution":540})";
        std::ofstream(root / "scenes/hud.scene") << R"({"actors":[{"uid":1,"name":"hud","components":{"hud":{"type":"Hud"}}}]})";
        // 40 static labels and 240 values that change every frame.
        std::ofstream(root / "component_types/Hud.lua") << R"(
Hud = {
    frame = 0,
    OnUpdate = function(self)
        self.frame = self.frame + 1
        for i = 0, 39 do
            Text.Draw("Label " .. i, 8 + (i % 4) * 236, 8 + math.floor(i / 4) * 18, "hud", 14, 230, 230, 230, 255)
        end
        for i = 0, 239 do
            local value = (self.frame * (i + 7)) % 100000
            Text.Draw("HP " .. value .. "/" .. (i * 13), 8 + (i % 6) * 158, 200 + math.floor(i / 6) * 8, "hud", 12,
                      120 + i % 120, 200, 90, 255)
        end
    end
}
)";
        ResourcePath::SetResourcesRootPath(root);
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
};
} // namespace

int main(int argc, char **argv) {
    try {
        Check(argc >= 2, "usage: text_benchmark BACKEND [SAMPLES] [CAPTURE.png]");
        const std::string backend = argv[1];
        const int samples = argc > 2 ? std::stoi(argv[2]) : 120;
        Check(samples > 0 && samples <= 5000, "samples must be in 1..5000");
        SDL_setenv_unsafe("POCKET_RENDER_DRIVER", backend.c_str(), 1);
        SDL_setenv_unsafe("POCKET_RENDER_VSYNC", "0", 1);
        const auto font = std::filesystem::path(POCKET_SOURCE_DIR) / ".engine/templates/ballgame/fonts/NotoSans-Regular.ttf";
        Fixture fixture(font);
        Engine engine;
        Check(engine.GetLastSceneLoadError().empty(), engine.GetLastSceneLoadError());
        engine.InitializeRuntime();
        engine.SetRenderRuntimeToTexture(true);
        engine.SetRuntimeRenderTargetSize(960, 540);
        std::vector<double> wall, cpu;
        auto frame = [&] {
            const double cpu_start = ThreadCpuMs();
            const auto start = Clock::now();
            engine.RunSingleFrame();
            Check(SDL_RenderTexture(engine.GetRenderer(), engine.GetRuntimeRenderTarget(), nullptr, nullptr), "present");
            engine.PresentFrame();
            wall.push_back(Ms(start));
            cpu.push_back(ThreadCpuMs() - cpu_start);
        };
        for (int i = 0; i < 10; ++i) frame();
        wall.clear(); cpu.clear();
        for (int i = 0; i < samples; ++i) frame();
        auto percentile = [](std::vector<double> values, double p) {
            std::sort(values.begin(), values.end());
            return values[std::min(values.size() - 1, static_cast<std::size_t>(std::ceil(p * values.size()) - 1))];
        };
        std::cout << "# 280 Text.Draw calls per frame, 240 changing every frame; warmup 10\n"
                  << "backend,samples,p50_ms,p95_ms,cpu_p50_ms,cpu_p95_ms\n"
                  << backend << ',' << samples << ',' << percentile(wall, 0.5) << ',' << percentile(wall, 0.95) << ','
                  << percentile(cpu, 0.5) << ',' << percentile(cpu, 0.95) << '\n';
        if (argc > 3) {
            SDL_SetRenderTarget(engine.GetRenderer(), engine.GetRuntimeRenderTarget());
            SDL_Surface *surface = SDL_RenderReadPixels(engine.GetRenderer(), nullptr);
            Check(surface != nullptr, SDL_GetError());
            const bool saved = IMG_SavePNG(surface, argv[3]);
            SDL_DestroySurface(surface);
            Check(saved, SDL_GetError());
        }
        engine.ShutdownRuntime();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "text benchmark: " << error.what() << '\n';
        return 1;
    }
}
