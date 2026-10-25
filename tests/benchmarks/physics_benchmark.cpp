// Box2D stepping through the engine: a pyramid of dynamic boxes settling on a
// static ground, stepped headlessly. Reports step times and a digest of final
// body positions so runs with different POCKET_PHYSICS_WORKERS can be compared.
//   physics_benchmark [BASE_WIDTH] [WARMUP] [SAMPLES]
#include "core/Engine.h"
#include "physics/Rigidbody.h"
#include "scripting/ComponentManager.h"
#include "scripting/LuaBoundary.h"
#include "shared/resources/ResourcePath.h"
#include "LuaBridge/LuaBridge.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
void Check(bool condition, const std::string &message) { if (!condition) throw std::runtime_error(message); }

struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("pocket-physics-benchmark-" + std::to_string(Clock::now().time_since_epoch().count()));
    int bodies = 0;
    explicit Fixture(int base) {
        std::filesystem::create_directories(root / "scenes");
        std::ofstream(root / "game.config") << R"({"initial_scene":"pyramid"})";
        std::ostringstream scene;
        scene << R"({"actors":[{"uid":1,"name":"ground","components":{"body":{"type":"Rigidbody","body_type":"static","x":0,"y":1,"width":)"
              << base * 1.2 + 10 << R"(,"height":1,"has_trigger":false}}})";
        int uid = 2;
        for (int row = 0; row < base; ++row)
            for (int column = 0; column < base - row; ++column, ++bodies)
                scene << R"(,{"uid":)" << uid++ << R"(,"name":"box","components":{"body":{"type":"Rigidbody","x":)"
                      << (column - (base - row) * 0.5) * 1.05 << R"(,"y":)" << -row * 1.0 << R"(,"width":1,"height":1,"has_trigger":false}}})";
        scene << "]}";
        std::ofstream(root / "scenes/pyramid.scene") << scene.str();
        ResourcePath::SetResourcesRootPath(root);
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
};
} // namespace

int main(int argc, char **argv) {
    try {
        const int base = argc > 1 ? std::stoi(argv[1]) : 60;
        const int warmup = argc > 2 ? std::stoi(argv[2]) : 30;
        const int samples = argc > 3 ? std::stoi(argv[3]) : 300;
        Check(base >= 2 && base <= 200 && warmup >= 0 && samples > 0, "usage: physics_benchmark [BASE 2..200] [WARMUP] [SAMPLES]");
        Fixture fixture(base);
        Engine engine;
        Check(engine.GetLastSceneLoadError().empty(), engine.GetLastSceneLoadError());
        engine.InitializeHeadless();
        for (int i = 0; i < warmup; ++i) engine.StepSimulation();
        std::vector<double> steps;
        for (int i = 0; i < samples; ++i) {
            const auto start = Clock::now();
            engine.StepSimulation();
            steps.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        }
        std::uint64_t digest = 14695981039346656037ull;
        for (int uid = 2; uid < fixture.bodies + 2; ++uid) {
            auto *body = LuaBoundary::Cast<Rigidbody *>(ComponentManager::GetComponentByKey(uid, "body"));
            const Vector2 position = body->GetPosition();
            for (const float value : {position.x, position.y}) {
                std::uint32_t bits;
                std::memcpy(&bits, &value, sizeof(bits));
                digest = (digest ^ bits) * 1099511628211ull;
            }
        }
        std::sort(steps.begin(), steps.end());
        auto percentile = [&](double p) {
            return steps[std::min(steps.size() - 1, static_cast<std::size_t>(std::ceil(p * steps.size()) - 1))];
        };
        const char *workers = std::getenv("POCKET_PHYSICS_WORKERS");
        std::cout << "workers,bodies,warmup,samples,step_p50_ms,step_p95_ms,digest\n"
                  << (workers ? workers : "0") << ',' << fixture.bodies << ',' << warmup << ',' << samples << ','
                  << percentile(0.5) << ',' << percentile(0.95) << ',' << std::hex << digest << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "physics benchmark: " << error.what() << '\n';
        return 1;
    }
}
