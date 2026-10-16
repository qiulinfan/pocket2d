#include "RegistrationDetail.h"
#include "input/Input.h"
#include "input/ActionMap.h"
#include "core/Vector2.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include "glm/glm.hpp"
#include <string>

namespace {

using APIRegistrationDetail::LuaState;

bool CppInputGetKey(const std::string &keycode) {
    return Input::GetKey(keycode);
}

bool CppInputGetKeyDown(const std::string &keycode) {
    return Input::GetKeyDown(keycode);
}

bool CppInputGetKeyUp(const std::string &keycode) {
    return Input::GetKeyUp(keycode);
}

bool CppInputGetAction(const std::string &name) { return ActionMap::IsHeld(name); }
bool CppInputGetActionDown(const std::string &name) { return ActionMap::WasPressed(name); }
bool CppInputGetActionUp(const std::string &name) { return ActionMap::WasReleased(name); }
float CppInputGetActionValue(const std::string &name) { return ActionMap::GetValue(name).x; }
Vector2 CppInputGetActionVector(const std::string &name) {
    const auto value = ActionMap::GetValue(name);
    return Vector2(value.x, value.y);
}

glm::vec2 CppInputGetMousePosition() {
    return Input::GetMousePosition();
}

bool CppInputGetMouseButton(int button_num) {
    return Input::GetMouseButton(button_num);
}

bool CppInputGetMouseButtonDown(int button_num) {
    return Input::GetMouseButtonDown(button_num);
}

bool CppInputGetMouseButtonUp(int button_num) {
    return Input::GetMouseButtonUp(button_num);
}

float CppInputGetMouseScrollDelta() {
    return Input::GetMouseScrollDelta();
}

void CppInputHideCursor() {
    Input::HideCursor();
}

void CppInputShowCursor() {
    Input::ShowCursor();
}

void InjectInputAPI() {
    luabridge::getGlobalNamespace(LuaState())
        .beginClass<glm::vec2>("vec2")
        .addPropertyReadWrite("x", &glm::vec2::x)
        .addPropertyReadWrite("y", &glm::vec2::y)
        .endClass();

    luabridge::getGlobalNamespace(LuaState())
        .beginNamespace("Input")
        .addFunction("GetKey", &CppInputGetKey)
        .addFunction("GetKeyDown", &CppInputGetKeyDown)
        .addFunction("GetKeyUp", &CppInputGetKeyUp)
        .addFunction("GetAction", &CppInputGetAction)
        .addFunction("GetActionDown", &CppInputGetActionDown)
        .addFunction("GetActionUp", &CppInputGetActionUp)
        .addFunction("GetActionValue", &CppInputGetActionValue)
        .addFunction("GetActionVector", &CppInputGetActionVector)
        .addFunction("GetMousePosition", &CppInputGetMousePosition)
        .addFunction("GetMouseButton", &CppInputGetMouseButton)
        .addFunction("GetMouseButtonDown", &CppInputGetMouseButtonDown)
        .addFunction("GetMouseButtonUp", &CppInputGetMouseButtonUp)
        .addFunction("GetMouseScrollDelta", &CppInputGetMouseScrollDelta)
        .addFunction("HideCursor", &CppInputHideCursor)
        .addFunction("ShowCursor", &CppInputShowCursor)
        .endNamespace();
}

} // namespace

namespace APIRegistrationDetail {

void RegisterInputAPI() {
    InjectInputAPI();
}

} // namespace APIRegistrationDetail
