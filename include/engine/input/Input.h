#ifndef INPUT_H
#define INPUT_H

#include "SDL3/SDL.h"
#include "glm/glm.hpp"
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

struct InputState;

class Input {
public:
    // initialize input state caches
    // 初始化输入状态缓存
    static void Init();

    // External sessions receive only explicitly injected events, never hardware.
    static void SetExternalInputEnabled(bool enabled);
    // Scene replacement preserves externally held controls until the host
    // changes them, while discarding one-tick edges and wheel impulses.
    static void ResetForSceneChange();

    // feed one SDL event into the input state machine
    // 将一个 SDL 事件写入输入状态机
    static void ProcessEvent(const SDL_Event &event);

    // sample held keyboard state once for this gameplay frame
    // 每个 gameplay frame 统一采样一次键盘按住状态
    static void BeginFrame();

    // advance transient just-down / just-up states at frame end
    // 在帧末推进 just-down / just-up 这类瞬时状态
    static void LateUpdate();

    static bool GetKey(SDL_Scancode key);
    static bool GetKeyDown(SDL_Scancode key);
    static bool GetKeyUp(SDL_Scancode key);

    // string keycode overloads
    // 字符串 keycode 版本
    static bool GetKey(const std::string &keycode);
    static bool GetKeyDown(const std::string &keycode);
    static bool GetKeyUp(const std::string &keycode);

    // Whether GetKey and friends recognize this key name.
    static bool IsKnownKeyName(const std::string &keycode);

    static glm::vec2 GetMousePosition();
    static bool GetMouseButton(int button_num);
    static bool GetMouseButtonDown(int button_num);
    // Any key or mouse button pressed this frame (Unity's Input.anyKeyDown).
    static bool GetAnyKeyDown();
    static bool GetMouseButtonUp(int button_num);
    static float GetMouseScrollDelta();

    static void HideCursor();
    static void ShowCursor();

private:
    // internal input state machine
    // 内部输入状态机
    enum INPUT_STATE {
        INPUT_STATE_UP,
        INPUT_STATE_JUST_BECAME_DOWN,
        INPUT_STATE_DOWN,
        INPUT_STATE_JUST_BECAME_UP
    };

    struct SDLScancodeHash {
        std::size_t operator()(SDL_Scancode keycode) const noexcept {
            return static_cast<std::size_t>(keycode);
        }
    };

    struct SDLKeycodeHash {
        std::size_t operator()(SDL_Keycode keycode) const noexcept {
            return static_cast<std::size_t>(keycode);
        }
    };

    struct KeyBinding {
        SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
        SDL_Keycode keycode = SDLK_UNKNOWN;
    };

    // parse a string key name into SDL scancode
    // 把字符串按键名解析成 SDL scancode
    static SDL_Scancode ParseKeycode(const std::string &keycode);
    static KeyBinding ParseKeyBinding(const std::string &keycode);

    // current implementation only supports left, middle, right
    // 当前只支持左键, 中键, 右键
    static bool IsSupportedMouseButton(int button_num);
    static void EnsureKeyboardSnapshotForCurrentFrame();
    static bool IsScancodeHeldInSnapshot(SDL_Scancode key);
    static bool IsKeycodeHeldInSnapshot(SDL_Keycode keycode);
    static void ResetKeyboardStates();

    // Per-World state; see InputState below.
    static InputState &State();
    friend struct InputState;
};

// Keyboard/mouse state for one World. Input's static API reads and writes the
// state of World::Current().
struct InputState {
    // keyboard state cache and per-frame transition lists
    // 键盘状态缓存和本帧状态变更列表
    std::unordered_map<SDL_Scancode, Input::INPUT_STATE, Input::SDLScancodeHash> keyboard_states_;
    std::vector<SDL_Scancode> just_became_down_scancodes_;
    std::vector<SDL_Scancode> just_became_up_scancodes_;
    std::unordered_map<SDL_Keycode, Input::INPUT_STATE, Input::SDLKeycodeHash> keycode_states_;
    std::vector<SDL_Keycode> just_became_down_keycodes_;
    std::vector<SDL_Keycode> just_became_up_keycodes_;
    std::array<bool, SDL_SCANCODE_COUNT> keyboard_held_snapshot_{};
    bool keyboard_held_snapshot_valid_ = false;
    int keyboard_held_snapshot_frame_ = -1;
    bool external_input_enabled_ = false;

    // mouse button held / down / up caches
    // 鼠标按住 / 本帧按下 / 本帧松开缓存
    std::array<bool, 4> mouse_button_held_{};
    std::array<bool, 4> mouse_button_down_{};
    std::array<bool, 4> mouse_button_up_{};

    // latest mouse position and wheel delta for this frame
    // 当前帧鼠标位置和滚轮增量
    glm::vec2 mouse_position_ = glm::vec2(0.0f, 0.0f);
    float mouse_scroll_delta_ = 0.0f;
};

#endif
