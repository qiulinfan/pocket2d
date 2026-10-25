#include "input/Input.h"
#include "input/ActionMap.h"
#include "core/FrameClock.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        Check(SDL_Init(0), "SDL initialization failed");
        Input::Init();
        Input::SetExternalInputEnabled(true);
        FrameClock::Reset();
        SDL_Event key{};
        key.type = SDL_EVENT_KEY_DOWN;
        key.key.scancode = SDL_SCANCODE_A;
        key.key.key = SDLK_A;
        key.key.down = true;
        Input::ProcessEvent(key);
        Input::BeginFrame();
        Check(Input::GetKey("a") && Input::GetKeyDown("a"), "key press must expose held/down");
        Check(Input::GetKeyDownName() == "a", "a pressed key must be named as GetKey takes it");
        Input::LateUpdate();
        FrameClock::AdvanceFrame();
        key.key.repeat = true;
        Input::ProcessEvent(key);
        Input::BeginFrame();
        Check(Input::GetKey("a") && !Input::GetKeyDown("a"), "repeat must not retrigger key down");
        Check(Input::GetKeyDownName().empty(), "a held key is not newly pressed");

        // Rebinding: an action reads the keys set at run time.
        const auto project = std::filesystem::temp_directory_path() / "pocket2d_input_state_test";
        std::filesystem::create_directories(project);
        std::ofstream(project / "actions.json")
            << R"({"version":1,"actions":{"jump":{"type":"button","keys":["k"]},"walk":{"type":"axis","negative":["left"],"positive":["right"]}}})";
        ActionMap::Load(project);
        Check(ActionMap::GetKeys("jump", "keys") == std::vector<std::string>{"k"}, "bindings come from actions.json");
        Check(!ActionMap::IsHeld("jump"), "k is not held");
        ActionMap::SetKeys("jump", "keys", {"a", "keypad 1"});
        Check(ActionMap::IsHeld("jump"), "a rebound action reads its new keys");
        bool rejected = false;
        try { ActionMap::SetKeys("walk", "keys", {"a"}); } catch (const std::invalid_argument &) { rejected = true; }
        Check(rejected, "an axis has no keys field");
        rejected = false;
        try { ActionMap::SetKeys("walk", "positive", {"no such key"}); } catch (const std::invalid_argument &) { rejected = true; }
        Check(rejected, "unknown key names are rejected");
        ActionMap::Load(project);
        std::filesystem::remove_all(project);

        SDL_Event mouse{};
        mouse.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        mouse.button.button = SDL_BUTTON_LEFT;
        mouse.button.x = 12.5f;
        mouse.button.y = 34.25f;
        Input::ProcessEvent(mouse);
        SDL_Event wheel{};
        wheel.type = SDL_EVENT_MOUSE_WHEEL;
        wheel.wheel.y = 2.5f;
        Input::ProcessEvent(wheel);
        Check(Input::GetMouseButton(1) && Input::GetMouseButtonDown(1), "mouse press must be held");
        Check(Input::GetMousePosition().x == 12.5f && Input::GetMousePosition().y == 34.25f,
              "SDL3 floating mouse coordinates must be preserved");
        Check(Input::GetMouseScrollDelta() == 2.5f, "wheel amount must be visible this tick");

        Input::ResetForSceneChange();
        Check(Input::GetKey("a") && Input::GetMouseButton(1),
              "scene replacement must preserve controls held by external host");
        Check(!Input::GetKeyDown("a") && !Input::GetMouseButtonDown(1) &&
              Input::GetMouseScrollDelta() == 0.0f,
              "scene replacement must clear transient edges and wheel");
        Input::ProcessEvent(wheel);

        SDL_Event focus{};
        focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
        Input::ProcessEvent(focus);
        Input::BeginFrame();
        Check(!Input::GetKey("a") && !Input::GetKeyDown("a") && !Input::GetKeyUp("a"),
              "focus loss must release keyboard state");
        Check(!Input::GetMouseButton(1) && !Input::GetMouseButtonDown(1) &&
              !Input::GetMouseButtonUp(1) && Input::GetMouseScrollDelta() == 0.0f,
              "focus loss must clear held mouse buttons and pending edges/wheel");
        focus.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
        Input::ProcessEvent(focus);
        Check(!Input::GetMouseButton(1), "focus regain must not resume stale mouse attack");

        Input::ProcessEvent(mouse);
        Input::LateUpdate();
        Check(Input::GetMouseButton(1) && !Input::GetMouseButtonDown(1),
              "mouse held must survive a normal tick boundary");
        mouse.type = SDL_EVENT_MOUSE_BUTTON_UP;
        Input::ProcessEvent(mouse);
        Check(!Input::GetMouseButton(1) && Input::GetMouseButtonUp(1), "mouse release must expose up");
        Input::LateUpdate();
        Check(!Input::GetMouseButtonUp(1), "mouse up edge must last only one tick");
        Input::SetExternalInputEnabled(false);
        SDL_Quit();
        std::cout << "PASS input state: key repeat, fractional mouse, focus loss, edges\n";
        return 0;
    } catch (const std::exception &error) {
        SDL_Quit();
        std::cerr << "FAIL input state: " << error.what() << '\n';
        return 1;
    }
}
