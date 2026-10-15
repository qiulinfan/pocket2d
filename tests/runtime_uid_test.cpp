#include "engine/core/Engine.h"
#include "shared/resources/ResourcePath.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("pocket-uid-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(root / "scenes");
        std::ofstream(root / "game.config") << R"({"initial_scene":"limits"})";
        std::ofstream(root / "scenes/limits.scene") << R"({"actors":[
            {"uid":18446744073709551615,"name":"max_uid","components":{}},
            {"uid":10000,"name":"reserved","components":{}}]})";
        ResourcePath::SetResourcesRootPath(root);
        {
            Engine engine;
            engine.InitializeHeadless();
            const auto max_uid = std::numeric_limits<Actor::UID>::max();
            std::set<Actor::UID> ids = {max_uid, 10000};
            for (int i = 0; i < 10; ++i) {
                Actor::UID created = 0;
                if (!engine.DuplicateRuntimeActorByUID(max_uid, &created) ||
                    created == 0 || !ids.insert(created).second)
                    throw std::runtime_error("UID wrap overwrote or reused a live actor identity");
                if (engine.GetRuntimeActorByUID(10000)->actor_name != "reserved" ||
                    engine.GetRuntimeActorByUID(max_uid)->actor_name != "max_uid")
                    throw std::runtime_error("authored actor changed during allocation");
            }
            engine.StepSimulation();
            if (engine.GetActorCount() != ids.size())
                throw std::runtime_error("actor count lost across UID wrap");
        }
        std::filesystem::remove_all(root);
        std::cout << "runtime UID boundary regression passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::filesystem::remove_all(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
