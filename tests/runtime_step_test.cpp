#include "core/Engine.h"
#include "core/FrameClock.h"
#include "input/Input.h"
#include "physics/Rigidbody.h"
#include "scripting/ComponentManager.h"
#include "shared/resources/ResourcePath.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
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

void Step(Engine &engine, int count) {
    const auto before = FrameClock::GetFrameNumber();
    for (int i = 0; i < count; ++i) engine.StepSimulation();
    Check(ComponentManager::GetRuntimeErrorCount() == 0,
          "Lua runtime error: " + ComponentManager::GetLastRuntimeError());
    Check(FrameClock::GetFrameNumber() == before + count,
          "each simulation step must advance exactly one frame");
}

int Number(Actor::UID uid, const char *field) {
    return ComponentManager::GetComponentByKey(uid, "logic")[field].cast<int>().value();
}

std::string State() {
    return ComponentManager::GetComponentByKey(1, "logic")["state"].cast<std::string>().value();
}

void Start(Engine &engine) {
    Check(engine.GetLastSceneLoadError().empty(), engine.GetLastSceneLoadError());
    engine.InitializeHeadless();
    Input::SetExternalInputEnabled(true);
    Check(FrameClock::GetFrameNumber() == 0, "new runtime must reset frame clock");
    Check(engine.GetWindow() == nullptr && engine.GetRenderer() == nullptr,
          "headless simulation must not create a window or renderer");
    Step(engine, 1);
}

void TestInputGatherAndTreeVictory() {
    Engine engine;
    Start(engine);
    const Actor::UID player_uid = engine.GetRuntimeActorByUID(2)->GetUID();
    Key(SDL_SCANCODE_RIGHT, true);
    Step(engine, 40);
    Check(Number(2, "presses") == 1, "held key must have one down edge");
    Check(Number(2, "held_ticks") == 40, "external held input must survive multiple ticks");
    Check(Number(1, "gathered") == 1, "real Box2D trigger must collect supply exactly once");
    Check(engine.GetRuntimeActorByUID(4) == nullptr, "collected supply must be removed");
    Check(engine.GetRuntimeActorByUID(2)->GetUID() == player_uid, "player UID must remain stable");
    Key(SDL_SCANCODE_RIGHT, false);
    Step(engine, 1);
    Check(Number(2, "releases") == 1, "release edge must be visible to Lua once");
    const float stopped_x = ComponentManager::GetComponentByKey(2, "body").cast<Rigidbody *>().value()->GetPosition().x;
    Step(engine, 10);
    const float after_x = ComponentManager::GetComponentByKey(2, "body").cast<Rigidbody *>().value()->GetPosition().x;
    Check(std::abs(stopped_x - after_x) < 0.001f, "released input must stop motion");
    Check(Number(2, "releases") == 1, "release must not repeat");
    Key(SDL_SCANCODE_SPACE, true);
    Step(engine, 36);
    Check(State() == "won", "destroying devil tree must win before invasion");
    Check(Number(1, "tree_health") == 0, "three attacks must destroy the tree");
    Step(engine, 180);
    Check(Number(1, "spawned") == 0, "destroyed tree must cancel pending waves");
    Key(SDL_SCANCODE_SPACE, false);
    Key(SDL_SCANCODE_R, true);
    Step(engine, 2);
    Check(State() == "countdown" && Number(1, "tree_health") == 3 &&
              Number(1, "health") == 3 && Number(1, "gathered") == 0,
          "graphical restart control must reload the same clean gameplay scene");
    Key(SDL_SCANCODE_R, false);
}

void TestThreeWaveDefense() {
    Engine engine;
    Start(engine);
    Key(SDL_SCANCODE_DOWN, true);
    Step(engine, 25);
    Key(SDL_SCANCODE_DOWN, false);
    Key(SDL_SCANCODE_SPACE, true);
    Step(engine, 1300);
    Check(State() == "won", "defending altar must complete all three waves");
    Check(Number(1, "wave") == 3, "three waves must run");
    Check(Number(1, "spawned") == 6 && Number(1, "defeated") == 6,
          "all six real spawned invaders must be defeated");
    Check(Number(1, "health") == 3, "successful defense must preserve altar health");
    Check(Number(1, "active") == 0, "no live invaders may remain after victory");
}

void TestUndefendedAltarFails() {
    Engine engine;
    Start(engine);
    Step(engine, 1000);
    Check(State() == "lost", "real enemy collisions must destroy undefended altar");
    Check(Number(1, "health") <= 0, "failure must result from depleted altar health");
    const int spawned = Number(1, "spawned");
    Step(engine, 300);
    Check(Number(1, "spawned") == spawned, "failure must stop future wave spawning");
}
}

int main(int argc, char **argv) {
    try {
        Check(argc == 2, "usage: runtime_step_test FIXTURE_PROJECT");
        ResourcePath::SetResourcesRootPath(argv[1]);
        TestInputGatherAndTreeVictory();
        TestThreeWaveDefense();
        TestUndefendedAltarFails();
        std::cout << "runtime_step_test: input, physics pickup, three waves, tree and failure passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "runtime_step_test: " << error.what() << '\n';
        return 1;
    }
}
