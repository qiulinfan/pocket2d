#include "scripting/ComponentManager.h"
#include "scene/Transform.h"
#include "shared/resources/ResourcePath.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <list>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
volatile double observed = 0.0;

std::list<Actor> MakeActors(int count, bool chain) {
    std::list<Actor> actors;
    for (int i = 0; i < count; ++i) {
        Actor actor;
        actor.uid = static_cast<Actor::UID>(i + 1);
        actor.parent_uid = chain && i > 0 ? actor.uid - 1 : Actor::kInvalidUID;
        actor.actor_name = "benchmark";
        actors.push_back(std::move(actor));
        ComponentManager::InstantiateComponentForActor(
            actors.back().uid, {"transform", "Transform", {{"x", 1.0}}});
    }
    return actors;
}

template <typename Fn> double MedianMilliseconds(Fn work) {
    work();
    std::vector<double> samples;
    for (int i = 0; i < 5; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        work();
        samples.push_back(std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - begin).count());
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

void Benchmark(int count) {
    {
        auto actors = MakeActors(count, false);
        ComponentManager::BindActorsForScene(actors);
        const double elapsed = MedianMilliseconds([&] {
            for (const Actor &actor : actors) {
                float x = 0.0f, y = 0.0f, rotation = 0.0f;
                if (!ComponentManager::TryGetRuntimeTransformWorld(
                        actor.uid, x, y, rotation) || x != 1.0f) {
                    throw std::runtime_error("world query produced an invalid result");
                }
                observed += x;
            }
        });
        std::cout << "root_world_queries," << count << ',' << elapsed << '\n';
        ComponentManager::ClearActorComponents();
    }
    {
        auto actors = MakeActors(count, true);
        ComponentManager::BindActorsForScene(actors);
        const double elapsed = MedianMilliseconds([&] {
            auto result = ComponentManager::GetComponentsInChildrenByType(1, "Transform");
            if (result.length() != count) {
                throw std::runtime_error("subtree query produced an invalid result");
            }
            observed += result.length();
        });
        std::cout << "chain_subtree_query," << count << ',' << elapsed << '\n';
        ComponentManager::ClearActorComponents();
    }
}
} // namespace

int main(int argc, char **argv) {
    // Avoid project scripts, SDL windows and user configuration entirely.
    ResourcePath::SetResourcesRootPath("__hierarchy_benchmark_no_project__");
    ComponentManager::Initialize();
    try {
        std::cout << "case,actors,median_ms\n";
        if (argc > 1) {
            const int count = std::atoi(argv[1]);
            if (count <= 0) throw std::runtime_error("actor count must be positive");
            Benchmark(count);
        } else {
            for (int count : {100, 1000, 5000}) Benchmark(count);
        }
        ComponentManager::Shutdown();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        ComponentManager::Shutdown();
        return 1;
    }
}
