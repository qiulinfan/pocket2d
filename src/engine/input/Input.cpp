#include "input/Input.h"
#include "core/Log.h"
#include "core/FrameClock.h"
#include "core/World.h"
#include "input/ActionMap.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_map>


namespace {

std::string ToLowerASCII(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool IsValidScancode(SDL_Scancode key) {
    return key > SDL_SCANCODE_UNKNOWN && key < SDL_SCANCODE_COUNT;
}

bool IsValidKeycode(SDL_Keycode key) {
    return key != SDLK_UNKNOWN;
}

SDL_Scancode ResolveKeyboardEventScancode(const SDL_KeyboardEvent &event) {
    if (IsValidScancode(event.scancode)) {
        return event.scancode;
    }

    const SDL_Scancode fallback = SDL_GetScancodeFromKey(event.key, nullptr);
    return IsValidScancode(fallback) ? fallback : SDL_SCANCODE_UNKNOWN;
}

std::string ToSDLNameCandidate(const std::string &value) {
    if (value.empty()) return value;
    if (value.size() == 1) {
        std::string candidate = value;
        candidate[0] = static_cast<char>(
            std::toupper(static_cast<unsigned char>(candidate[0])));
        return candidate;
    }
    return value;
}

bool ShouldLogInputDiagnostics() {
    return SDL_getenv("ENGINE_LOG_INPUT") != nullptr;
}

void LogKeyboardEvent(const SDL_KeyboardEvent &event, SDL_Scancode resolved_key) {
    if (!ShouldLogInputDiagnostics()) return;

    static std::atomic<int> logged_events = 0;
    if (logged_events.fetch_add(1) >= 80) return;

    const char *type = event.type == SDL_EVENT_KEY_DOWN ? "down" : "up";
    Log::Info("input", std::string("key ") + type +
                           " scancode=" + std::to_string(static_cast<int>(event.scancode)) +
                           " resolved=" + std::to_string(static_cast<int>(resolved_key)) +
                           " scancode_name=" + SDL_GetScancodeName(resolved_key) +
                           " keycode=" + std::to_string(event.key) +
                           " key_name=" + SDL_GetKeyName(event.key) +
                           " repeat=" + std::to_string(static_cast<int>(event.repeat)));
}

void LogWindowFocusEvent(const SDL_WindowEvent &event) {
    if (!ShouldLogInputDiagnostics()) return;
    if (event.type != SDL_EVENT_WINDOW_FOCUS_GAINED &&
        event.type != SDL_EVENT_WINDOW_FOCUS_LOST) {
        return;
    }

    const char *focus_state =
        event.type == SDL_EVENT_WINDOW_FOCUS_GAINED ? "gained" : "lost";
    Log::Info("input", std::string("window focus ") + focus_state);
}
} // namespace

InputState &Input::State() {
    return World::Current().State<InputState>();
}

// initialize input state caches
void Input::Init() {
    State().keyboard_states_.clear();
    State().keyboard_states_.reserve(SDL_SCANCODE_COUNT);
    for (int code = SDL_SCANCODE_UNKNOWN; code < SDL_SCANCODE_COUNT; ++code) {
        State().keyboard_states_[static_cast<SDL_Scancode>(code)] = INPUT_STATE_UP;
    }
    State().keycode_states_.clear();

    State().just_became_down_scancodes_.clear();
    State().just_became_up_scancodes_.clear();
    State().just_became_down_keycodes_.clear();
    State().just_became_up_keycodes_.clear();
    State().keyboard_held_snapshot_.fill(false);
    State().keyboard_held_snapshot_valid_ = false;
    State().keyboard_held_snapshot_frame_ = -1;
    State().mouse_button_held_.fill(false);
    State().mouse_button_down_.fill(false);
    State().mouse_button_up_.fill(false);
    State().mouse_position_ = glm::vec2(0.0f, 0.0f);
    State().mouse_scroll_delta_ = 0.0f;
}

void Input::SetExternalInputEnabled(bool enabled) {
    if (State().external_input_enabled_ == enabled) return;
    // Switching owners must not carry a physically held key into an agent turn.
    ResetKeyboardStates();
    State().mouse_button_held_.fill(false);
    State().mouse_button_down_.fill(false);
    State().mouse_button_up_.fill(false);
    State().mouse_scroll_delta_ = 0.0f;
    State().external_input_enabled_ = enabled;
}

void Input::ResetForSceneChange() {
    if (State().external_input_enabled_) {
        LateUpdate();
    } else {
        Init();
    }
}

void Input::ResetKeyboardStates() {
    for (auto &entry : State().keyboard_states_) {
        entry.second = INPUT_STATE_UP;
    }
    State().keycode_states_.clear();
    State().just_became_down_scancodes_.clear();
    State().just_became_up_scancodes_.clear();
    State().just_became_down_keycodes_.clear();
    State().just_became_up_keycodes_.clear();
    State().keyboard_held_snapshot_.fill(false);
    State().keyboard_held_snapshot_valid_ = false;
    State().keyboard_held_snapshot_frame_ = -1;
}

void Input::BeginFrame() {
    if (State().external_input_enabled_) {
        State().keyboard_held_snapshot_.fill(false);
        State().keyboard_held_snapshot_valid_ = true;
        State().keyboard_held_snapshot_frame_ = FrameClock::GetFrameNumber();
        return;
    }
    SDL_PumpEvents();

    State().keyboard_held_snapshot_.fill(false);
    int key_count = 0;
    const bool *keyboard_state = SDL_GetKeyboardState(&key_count);
    if (keyboard_state != nullptr) {
        const int snapshot_count =
            std::min(key_count, static_cast<int>(SDL_SCANCODE_COUNT));
        for (int code = 0; code < snapshot_count; ++code) {
            State().keyboard_held_snapshot_[code] = keyboard_state[code] != 0;
        }
    }

    State().keyboard_held_snapshot_valid_ = true;
    State().keyboard_held_snapshot_frame_ = FrameClock::GetFrameNumber();
}

void Input::EnsureKeyboardSnapshotForCurrentFrame() {
    if (State().keyboard_held_snapshot_valid_ &&
        State().keyboard_held_snapshot_frame_ == FrameClock::GetFrameNumber()) {
        return;
    }
    BeginFrame();
}

bool Input::IsScancodeHeldInSnapshot(SDL_Scancode key) {
    if (!IsValidScancode(key)) return false;
    EnsureKeyboardSnapshotForCurrentFrame();
    return State().keyboard_held_snapshot_[static_cast<int>(key)];
}

bool Input::IsKeycodeHeldInSnapshot(SDL_Keycode keycode) {
    if (!IsValidKeycode(keycode)) return false;
    EnsureKeyboardSnapshotForCurrentFrame();

    for (int code = 0; code < SDL_SCANCODE_COUNT; ++code) {
        if (!State().keyboard_held_snapshot_[code]) continue;
        const SDL_Scancode scancode = static_cast<SDL_Scancode>(code);
        if (SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, false) == keycode) {
            return true;
        }
    }
    return false;
}

// feed one SDL event into the input state machine
void Input::ProcessEvent(const SDL_Event &event) {
    if (event.type == SDL_EVENT_KEY_DOWN) {
        const SDL_Scancode key = ResolveKeyboardEventScancode(event.key);
        LogKeyboardEvent(event.key, key);
        const SDL_Keycode keycode = event.key.key;

        if (IsValidScancode(key)) {
            INPUT_STATE &state = State().keyboard_states_[key];
            if (state != INPUT_STATE_DOWN && state != INPUT_STATE_JUST_BECAME_DOWN) {
                state = INPUT_STATE_JUST_BECAME_DOWN;
                State().just_became_down_scancodes_.push_back(key);
            }
        }
        if (IsValidKeycode(keycode)) {
            INPUT_STATE &state = State().keycode_states_[keycode];
            if (state != INPUT_STATE_DOWN && state != INPUT_STATE_JUST_BECAME_DOWN) {
                state = INPUT_STATE_JUST_BECAME_DOWN;
                State().just_became_down_keycodes_.push_back(keycode);
            }
        }
    } 
    else if (event.type == SDL_EVENT_KEY_UP) {
        const SDL_Scancode key = ResolveKeyboardEventScancode(event.key);
        LogKeyboardEvent(event.key, key);
        const SDL_Keycode keycode = event.key.key;

        if (IsValidScancode(key)) {
            INPUT_STATE &state = State().keyboard_states_[key];
            state = INPUT_STATE_JUST_BECAME_UP;
            State().just_became_up_scancodes_.push_back(key);
        }
        if (IsValidKeycode(keycode)) {
            INPUT_STATE &state = State().keycode_states_[keycode];
            state = INPUT_STATE_JUST_BECAME_UP;
            State().just_became_up_keycodes_.push_back(keycode);
        }
    } 
    else if (event.type == SDL_EVENT_MOUSE_MOTION) {
        State().mouse_position_.x = static_cast<float>(event.motion.x);
        State().mouse_position_.y = static_cast<float>(event.motion.y);
    } 
    else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        const int button = static_cast<int>(event.button.button);
        State().mouse_position_.x = static_cast<float>(event.button.x);
        State().mouse_position_.y = static_cast<float>(event.button.y);
        if (IsSupportedMouseButton(button)) {
            if (!State().mouse_button_held_[button]) {
                State().mouse_button_down_[button] = true;
                State().mouse_button_up_[button] = false;
            }
            State().mouse_button_held_[button] = true;
        }
    } 
    else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        const int button = static_cast<int>(event.button.button);
        State().mouse_position_.x = static_cast<float>(event.button.x);
        State().mouse_position_.y = static_cast<float>(event.button.y);
        if (IsSupportedMouseButton(button)) {
            if (State().mouse_button_held_[button]) {
                State().mouse_button_up_[button] = true;
                State().mouse_button_down_[button] = false;
            }
            State().mouse_button_held_[button] = false;
        }
    } 
    else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
        State().mouse_scroll_delta_ += event.wheel.y;
    }
    else if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED ||
             event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        LogWindowFocusEvent(event.window);
        if (event.window.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            ResetKeyboardStates();
            State().mouse_button_held_.fill(false);
            State().mouse_button_down_.fill(false);
            State().mouse_button_up_.fill(false);
            State().mouse_scroll_delta_ = 0.0f;
        }
    }
}

// advance transient just-down / just-up states at frame end
void Input::LateUpdate() {
    ActionMap::EndFrame();
    for (SDL_Scancode code : State().just_became_down_scancodes_) {
        if (State().keyboard_states_[code] == INPUT_STATE_JUST_BECAME_DOWN) {
            State().keyboard_states_[code] = INPUT_STATE_DOWN;
        }
    }

    for (SDL_Scancode code : State().just_became_up_scancodes_) {
        if (State().keyboard_states_[code] == INPUT_STATE_JUST_BECAME_UP) {
            State().keyboard_states_[code] = INPUT_STATE_UP;
        }
    }
    for (SDL_Keycode code : State().just_became_down_keycodes_) {
        if (State().keycode_states_[code] == INPUT_STATE_JUST_BECAME_DOWN) {
            State().keycode_states_[code] = INPUT_STATE_DOWN;
        }
    }

    for (SDL_Keycode code : State().just_became_up_keycodes_) {
        if (State().keycode_states_[code] == INPUT_STATE_JUST_BECAME_UP) {
            State().keycode_states_[code] = INPUT_STATE_UP;
        }
    }

    State().just_became_down_scancodes_.clear();
    State().just_became_up_scancodes_.clear();
    State().just_became_down_keycodes_.clear();
    State().just_became_up_keycodes_.clear();
    State().mouse_button_down_.fill(false);
    State().mouse_button_up_.fill(false);
    State().mouse_scroll_delta_ = 0.0f;
}

bool Input::GetKey(SDL_Scancode key) {
    if (!IsValidScancode(key)) return false;
    if (IsScancodeHeldInSnapshot(key)) return true;
    const INPUT_STATE state = State().keyboard_states_[key];
    return state == INPUT_STATE_DOWN || state == INPUT_STATE_JUST_BECAME_DOWN;
}

bool Input::GetKeyDown(SDL_Scancode key) {
    if (!IsValidScancode(key)) return false;
    if (State().keyboard_states_[key] == INPUT_STATE_JUST_BECAME_DOWN) return true;
    // A tap pressed and released within one frame still counts as pressed.
    const auto &down = State().just_became_down_scancodes_;
    return std::find(down.begin(), down.end(), key) != down.end();
}

bool Input::GetKeyUp(SDL_Scancode key) {
    if (!IsValidScancode(key)) return false;
    return State().keyboard_states_[key] == INPUT_STATE_JUST_BECAME_UP;
}

// parse a string key name into SDL scancode
SDL_Scancode Input::ParseKeycode(const std::string &keycode) {
    return ParseKeyBinding(keycode).scancode;
}

Input::KeyBinding Input::ParseKeyBinding(const std::string &keycode) {
    static const std::unordered_map<std::string, KeyBinding> kKeyMap = {
        {"up", {SDL_SCANCODE_UP, SDLK_UP}},
        {"down", {SDL_SCANCODE_DOWN, SDLK_DOWN}},
        {"right", {SDL_SCANCODE_RIGHT, SDLK_RIGHT}},
        {"left", {SDL_SCANCODE_LEFT, SDLK_LEFT}},
        {"escape", {SDL_SCANCODE_ESCAPE, SDLK_ESCAPE}},
        {"lshift", {SDL_SCANCODE_LSHIFT, SDLK_LSHIFT}},
        {"left shift", {SDL_SCANCODE_LSHIFT, SDLK_LSHIFT}},
        {"rshift", {SDL_SCANCODE_RSHIFT, SDLK_RSHIFT}},
        {"right shift", {SDL_SCANCODE_RSHIFT, SDLK_RSHIFT}},
        {"shift", {SDL_SCANCODE_LSHIFT, SDLK_LSHIFT}},
        {"lctrl", {SDL_SCANCODE_LCTRL, SDLK_LCTRL}},
        {"rctrl", {SDL_SCANCODE_RCTRL, SDLK_RCTRL}},
        {"left ctrl", {SDL_SCANCODE_LCTRL, SDLK_LCTRL}},
        {"right ctrl", {SDL_SCANCODE_RCTRL, SDLK_RCTRL}},
        {"lalt", {SDL_SCANCODE_LALT, SDLK_LALT}},
        {"ralt", {SDL_SCANCODE_RALT, SDLK_RALT}},
        {"left alt", {SDL_SCANCODE_LALT, SDLK_LALT}},
        {"right alt", {SDL_SCANCODE_RALT, SDLK_RALT}},
        {"left option", {SDL_SCANCODE_LALT, SDLK_LALT}},
        {"right option", {SDL_SCANCODE_RALT, SDLK_RALT}},
        // GUI keys (Command on macOS, Windows key elsewhere). Not through SDL's
        // names: on macOS SDL renames them "Left Command" once video starts.
        {"lgui", {SDL_SCANCODE_LGUI, SDLK_LGUI}},
        {"rgui", {SDL_SCANCODE_RGUI, SDLK_RGUI}},
        {"left gui", {SDL_SCANCODE_LGUI, SDLK_LGUI}},
        {"right gui", {SDL_SCANCODE_RGUI, SDLK_RGUI}},
        {"lcmd", {SDL_SCANCODE_LGUI, SDLK_LGUI}},
        {"rcmd", {SDL_SCANCODE_RGUI, SDLK_RGUI}},
        {"left command", {SDL_SCANCODE_LGUI, SDLK_LGUI}},
        {"right command", {SDL_SCANCODE_RGUI, SDLK_RGUI}},
        {"tab", {SDL_SCANCODE_TAB, SDLK_TAB}},
        {"return", {SDL_SCANCODE_RETURN, SDLK_RETURN}},
        {"enter", {SDL_SCANCODE_RETURN, SDLK_RETURN}},
        {"backspace", {SDL_SCANCODE_BACKSPACE, SDLK_BACKSPACE}},
        {"delete", {SDL_SCANCODE_DELETE, SDLK_DELETE}},
        {"insert", {SDL_SCANCODE_INSERT, SDLK_INSERT}},
        {"space", {SDL_SCANCODE_SPACE, SDLK_SPACE}},
        {"a", {SDL_SCANCODE_A, SDLK_A}},
        {"b", {SDL_SCANCODE_B, SDLK_B}},
        {"c", {SDL_SCANCODE_C, SDLK_C}},
        {"d", {SDL_SCANCODE_D, SDLK_D}},
        {"e", {SDL_SCANCODE_E, SDLK_E}},
        {"f", {SDL_SCANCODE_F, SDLK_F}},
        {"g", {SDL_SCANCODE_G, SDLK_G}},
        {"h", {SDL_SCANCODE_H, SDLK_H}},
        {"i", {SDL_SCANCODE_I, SDLK_I}},
        {"j", {SDL_SCANCODE_J, SDLK_J}},
        {"k", {SDL_SCANCODE_K, SDLK_K}},
        {"l", {SDL_SCANCODE_L, SDLK_L}},
        {"m", {SDL_SCANCODE_M, SDLK_M}},
        {"n", {SDL_SCANCODE_N, SDLK_N}},
        {"o", {SDL_SCANCODE_O, SDLK_O}},
        {"p", {SDL_SCANCODE_P, SDLK_P}},
        {"q", {SDL_SCANCODE_Q, SDLK_Q}},
        {"r", {SDL_SCANCODE_R, SDLK_R}},
        {"s", {SDL_SCANCODE_S, SDLK_S}},
        {"t", {SDL_SCANCODE_T, SDLK_T}},
        {"u", {SDL_SCANCODE_U, SDLK_U}},
        {"v", {SDL_SCANCODE_V, SDLK_V}},
        {"w", {SDL_SCANCODE_W, SDLK_W}},
        {"x", {SDL_SCANCODE_X, SDLK_X}},
        {"y", {SDL_SCANCODE_Y, SDLK_Y}},
        {"z", {SDL_SCANCODE_Z, SDLK_Z}},
        {"0", {SDL_SCANCODE_0, SDLK_0}},
        {"1", {SDL_SCANCODE_1, SDLK_1}},
        {"2", {SDL_SCANCODE_2, SDLK_2}},
        {"3", {SDL_SCANCODE_3, SDLK_3}},
        {"4", {SDL_SCANCODE_4, SDLK_4}},
        {"5", {SDL_SCANCODE_5, SDLK_5}},
        {"6", {SDL_SCANCODE_6, SDLK_6}},
        {"7", {SDL_SCANCODE_7, SDLK_7}},
        {"8", {SDL_SCANCODE_8, SDLK_8}},
        {"9", {SDL_SCANCODE_9, SDLK_9}},
        {"/", {SDL_SCANCODE_SLASH, SDLK_SLASH}},
        {";", {SDL_SCANCODE_SEMICOLON, SDLK_SEMICOLON}},
        {"=", {SDL_SCANCODE_EQUALS, SDLK_EQUALS}},
        {"-", {SDL_SCANCODE_MINUS, SDLK_MINUS}},
        {".", {SDL_SCANCODE_PERIOD, SDLK_PERIOD}},
        {",", {SDL_SCANCODE_COMMA, SDLK_COMMA}},
        {"[", {SDL_SCANCODE_LEFTBRACKET, SDLK_LEFTBRACKET}},
        {"]", {SDL_SCANCODE_RIGHTBRACKET, SDLK_RIGHTBRACKET}},
        {"\\", {SDL_SCANCODE_BACKSLASH, SDLK_BACKSLASH}},
        {"'", {SDL_SCANCODE_APOSTROPHE, SDLK_APOSTROPHE}}
    };

    const std::string normalized = ToLowerASCII(keycode);
    auto it = kKeyMap.find(normalized);
    if (it != kKeyMap.end()) return it->second;

    KeyBinding binding;
    const std::string sdl_name = ToSDLNameCandidate(normalized);
    binding.scancode = SDL_GetScancodeFromName(sdl_name.c_str());
    binding.keycode = SDL_GetKeyFromName(sdl_name.c_str());

    /*
    SDL names cover most special keys. Single-character names also have stable
    ASCII keycodes, which is useful on Windows keyboard layouts where the
    keycode path can differ from the physical scancode path.
    */
    if (!IsValidKeycode(binding.keycode) && normalized.size() == 1) {
        binding.keycode = static_cast<SDL_Keycode>(normalized[0]);
    }
    if (!IsValidScancode(binding.scancode) &&
        IsValidKeycode(binding.keycode)) {
        binding.scancode = SDL_GetScancodeFromKey(binding.keycode, nullptr);
    }
    return binding;
}

bool Input::IsKnownKeyName(const std::string &keycode) {
    const KeyBinding binding = ParseKeyBinding(keycode);
    return IsValidScancode(binding.scancode) || IsValidKeycode(binding.keycode);
}

// string keycode overloads
bool Input::GetKey(const std::string &keycode) {
    const KeyBinding binding = ParseKeyBinding(keycode);
    if (GetKey(binding.scancode)) return true;
    if (!IsValidKeycode(binding.keycode)) return false;
    const auto found = State().keycode_states_.find(binding.keycode);
    if (found != State().keycode_states_.end() &&
        (found->second == INPUT_STATE_DOWN ||
         found->second == INPUT_STATE_JUST_BECAME_DOWN)) {
        return true;
    }
    return IsKeycodeHeldInSnapshot(binding.keycode);
}

// string keycode overloads
bool Input::GetKeyDown(const std::string &keycode) {
    const KeyBinding binding = ParseKeyBinding(keycode);
    if (GetKeyDown(binding.scancode)) return true;
    if (!IsValidKeycode(binding.keycode)) return false;
    const auto found = State().keycode_states_.find(binding.keycode);
    if (found != State().keycode_states_.end() && found->second == INPUT_STATE_JUST_BECAME_DOWN) return true;
    const auto &down = State().just_became_down_keycodes_;
    return std::find(down.begin(), down.end(), binding.keycode) != down.end();
}

// string keycode overloads
bool Input::GetKeyUp(const std::string &keycode) {
    const KeyBinding binding = ParseKeyBinding(keycode);
    if (GetKeyUp(binding.scancode)) return true;
    if (!IsValidKeycode(binding.keycode)) return false;
    const auto found = State().keycode_states_.find(binding.keycode);
    return found != State().keycode_states_.end() &&
           found->second == INPUT_STATE_JUST_BECAME_UP;
}

// current implementation only supports left, middle, right
bool Input::IsSupportedMouseButton(int button_num) {
    return button_num >= 1 && button_num <= 3;
}

glm::vec2 Input::GetMousePosition() {
    return State().mouse_position_;
}

bool Input::GetMouseButton(int button_num) {
    if (!IsSupportedMouseButton(button_num)) return false;
    return State().mouse_button_held_[button_num];
}

bool Input::GetMouseButtonDown(int button_num) {
    if (!IsSupportedMouseButton(button_num)) return false;
    return State().mouse_button_down_[button_num];
}


bool Input::GetAnyKeyDown() {
    for (const auto &[key, state] : State().keyboard_states_)
        if (state == INPUT_STATE_JUST_BECAME_DOWN) return true;
    for (const bool down : State().mouse_button_down_)
        if (down) return true;
    return false;
}

bool Input::GetMouseButtonUp(int button_num) {
    if (!IsSupportedMouseButton(button_num)) return false;
    return State().mouse_button_up_[button_num];
}

float Input::GetMouseScrollDelta() {
    return State().mouse_scroll_delta_;
}

void Input::HideCursor() {
    SDL_HideCursor();
}


void Input::ShowCursor() {
    SDL_ShowCursor();
}
