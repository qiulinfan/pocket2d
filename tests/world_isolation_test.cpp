// Several Worlds in one process: interleaved and parallel stepping must match a
// sequential reference frame by frame, and Worlds must not observe each other.
#include "core/Engine.h"
#include "core/FrameClock.h"
#include "core/World.h"
#include "input/Input.h"
#include "scripting/ComponentManager.h"
#include "shared/resources/ResourcePath.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr std::uint32_t kSeed = 42;
constexpr int kFrames = 420;
constexpr int kDigestInterval = 10;

void Check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void Key(SDL_Scancode scancode, bool down) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.scancode = scancode;
    event.key.key = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, false);
    event.key.down = down;
    Input::ProcessEvent(event);
}

std::string Number(double value) {
    char text[64];
    std::snprintf(text, sizeof(text), "%a", value);
    return text;
}

// Text form of what an agent observes: frame, actors, world poses and every
// supported component property, with exact float bits.
std::string Digest(const Engine &engine) {
    std::ostringstream out;
    out << "frame=" << FrameClock::GetFrameNumber() << '\n';
    std::vector<const Actor *> actors;
    for (const auto &actor : engine.GetRuntimeActors())
        if (!actor.runtime_destroyed) actors.push_back(&actor);
    std::sort(actors.begin(), actors.end(),
              [](const Actor *a, const Actor *b) { return a->uid < b->uid; });
    for (const Actor *actor : actors) {
        out << actor->uid << ' ' << actor->actor_name << " parent=" << actor->parent_uid;
        float x = 0, y = 0, rotation = 0;
        if (ComponentManager::TryGetRuntimeTransformWorld(actor->uid, x, y, rotation))
            out << " pose=" << Number(x) << ',' << Number(y) << ',' << Number(rotation);
        out << '\n';
        auto specs = ComponentManager::GetRuntimeComponentSpecs(actor->uid);
        std::sort(specs.begin(), specs.end(),
                  [](const auto &a, const auto &b) { return a.key < b.key; });
        for (const auto &spec : specs) {
            auto properties = ComponentManager::GetRuntimeComponentProperties(actor->uid, spec.key);
            std::sort(properties.begin(), properties.end(),
                      [](const auto &a, const auto &b) { return a.name < b.name; });
            for (const auto &property : properties) {
                out << "  " << spec.key << '.' << property.name << '=';
                std::visit([&out](const auto &value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, double>) out << Number(value);
                    else if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, int> ||
                                       std::is_same_v<T, std::string>) out << value;
                    else for (const auto &item : value) {
                        if constexpr (std::is_same_v<std::decay_t<decltype(item)>, double>) out << Number(item) << ';';
                        else out << item << ';';
                    }
                }, property.value);
                out << '\n';
            }
        }
    }
    return out.str();
}

struct KeyEvent {
    int frame;
    SDL_Scancode key;
    bool down;
};
using Script = std::vector<KeyEvent>;

// One World running a fixture under a fixed input script. Every call binds the
// World, so several runs can interleave on one thread.
class Run {
public:
    Run(World &world, const Script &script) : world_(world), script_(script) {
        WorldScope scope(world_);
        ComponentManager::SetRandomSeed(kSeed);
        engine_ = std::make_unique<Engine>(world_);
        Check(engine_->GetLastSceneLoadError().empty(), engine_->GetLastSceneLoadError());
        engine_->InitializeHeadless();
    }

    bool Done() const { return frame_ >= kFrames; }

    void Step() {
        WorldScope scope(world_);
        for (const KeyEvent &event : script_)
            if (event.frame == frame_) Key(event.key, event.down);
        engine_->StepSimulation();
        ++frame_;
        Check(ComponentManager::GetRuntimeErrorCount() == 0,
              "Lua runtime error: " + ComponentManager::GetLastRuntimeError());
        Check(FrameClock::GetFrameNumber() == frame_, "each World advances only its own frame");
        if (frame_ % kDigestInterval == 0) digests_.push_back(Digest(*engine_));
    }

    luabridge::LuaRef Component(Actor::UID uid, const std::string &key) {
        WorldScope scope(world_);
        return ComponentManager::GetComponentByKey(uid, key);
    }

    const std::vector<std::string> &Digests() const { return digests_; }

private:
    World &world_;
    const Script &script_;
    std::unique_ptr<Engine> engine_;
    int frame_ = 0;
    std::vector<std::string> digests_;
};

std::vector<std::string> Sequential(const Script &script) {
    World world;
    Run run(world, script);
    while (!run.Done()) run.Step();
    return run.Digests();
}

void CheckSame(const std::vector<std::string> &expected,
               const std::vector<std::string> &actual, const std::string &label) {
    Check(actual.size() == expected.size(), label + ": digest count differs");
    for (std::size_t i = 0; i < expected.size(); ++i)
        Check(actual[i] == expected[i], label + ": diverged at frame " +
                                            std::to_string((i + 1) * kDigestInterval) + "\nexpected:\n" +
                                            expected[i] + "actual:\n" + actual[i]);
}

// Two Worlds driven by different scripts must each match their own sequential
// reference, whether they interleave on one thread or run on two threads.
void TestWorlds(const Script &first_script, const Script &second_script, bool defense) {
    const auto first_reference = Sequential(first_script);
    const auto second_reference = Sequential(second_script);
    Check(first_reference != second_reference, "the two input scripts must lead to different worlds");
    CheckSame(first_reference, Sequential(first_script), "sequential replay");

    {
        World a, b;
        Run first(a, first_script), second(b, second_script);
        while (!first.Done()) {
            first.Step();
            second.Step();
        }
        CheckSame(first_reference, first.Digests(), "interleaved world A");
        CheckSame(second_reference, second.Digests(), "interleaved world B");
        if (defense)
            Check(first.Component(1, "logic")["gathered"].cast<int>().value() == 1 &&
                      second.Component(1, "logic")["gathered"].cast<int>().value() == 0,
                  "one World's pickup must not appear in another World");
    }

    std::vector<std::string> results[2];
    std::string errors[2];
    const Script *scripts[2] = {&first_script, &second_script};
    auto worker = [&](int index) {
        try {
            World world;
            Run run(world, *scripts[index]);
            while (!run.Done()) run.Step();
            results[index] = run.Digests();
        } catch (const std::exception &error) {
            errors[index] = error.what();
        }
    };
    std::thread first(worker, 0), second(worker, 1);
    first.join();
    second.join();
    Check(errors[0].empty() && errors[1].empty(), "parallel World failed: " + errors[0] + errors[1]);
    CheckSame(first_reference, results[0], "parallel world A");
    CheckSame(second_reference, results[1], "parallel world B");
}
} // namespace

int main(int argc, char **argv) {
    try {
        Check(argc == 3, "usage: world_isolation_test FIXTURE_PROJECT defense|village");
        ResourcePath::SetResourcesRootPath(argv[1]);
        const std::string scripts = argv[2];
        if (scripts == "defense") {
            // Collect the supply to the right, or walk down; then attack.
            TestWorlds({{0, SDL_SCANCODE_RIGHT, true}, {40, SDL_SCANCODE_RIGHT, false}, {40, SDL_SCANCODE_SPACE, true}},
                       {{0, SDL_SCANCODE_DOWN, true}, {40, SDL_SCANCODE_DOWN, false}, {40, SDL_SCANCODE_SPACE, true}},
                       true);
        } else if (scripts == "village") {
            // Farm interaction, melee, eating and movement in different orders.
            TestWorlds({{0, SDL_SCANCODE_RIGHT, true}, {50, SDL_SCANCODE_RIGHT, false},
                        {50, SDL_SCANCODE_E, true}, {52, SDL_SCANCODE_E, false},
                        {60, SDL_SCANCODE_SPACE, true}, {180, SDL_SCANCODE_SPACE, false},
                        {180, SDL_SCANCODE_DOWN, true}, {260, SDL_SCANCODE_DOWN, false},
                        {260, SDL_SCANCODE_Q, true}, {262, SDL_SCANCODE_Q, false}},
                       {{0, SDL_SCANCODE_LEFT, true}, {40, SDL_SCANCODE_LEFT, false},
                        {40, SDL_SCANCODE_UP, true}, {120, SDL_SCANCODE_UP, false},
                        {120, SDL_SCANCODE_SPACE, true}, {300, SDL_SCANCODE_SPACE, false}},
                       false);
        } else {
            throw std::runtime_error("unknown script set: " + scripts);
        }
        std::cout << "world_isolation_test: sequential, interleaved and parallel Worlds match\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "world_isolation_test: " << error.what() << '\n';
        return 1;
    }
}
