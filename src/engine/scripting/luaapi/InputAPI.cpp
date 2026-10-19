#include "RegistrationDetail.h"
#include "input/Input.h"
#include "input/ActionMap.h"
#include "core/Vector2.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include "glm/glm.hpp"
#include <string>
#include <vector>

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

std::string CppInputGetKeyDownName() { return Input::GetKeyDownName(); }

// Input.GetBindings(action, field) -> {key names}
int CppInputGetBindings(lua_State *L) {
    try {
        const auto keys = ActionMap::GetKeys(luaL_checkstring(L, 1), luaL_checkstring(L, 2));
        lua_createtable(L, static_cast<int>(keys.size()), 0);
        for (std::size_t i = 0; i < keys.size(); ++i) {
            lua_pushlstring(L, keys[i].data(), keys[i].size());
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        return 1;
    } catch (const std::exception &failure) {
        return luaL_error(L, "%s", failure.what());
    }
}

// Input.SetBindings(action, field, {key names})
int CppInputSetBindings(lua_State *L) {
    const std::string name = luaL_checkstring(L, 1), field = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    std::vector<std::string> keys;
    const lua_Integer n = luaL_len(L, 3);
    for (lua_Integer i = 1; i <= n && i <= 17; ++i) {
        lua_rawgeti(L, 3, i);
        if (!lua_isstring(L, -1)) return luaL_error(L, "Input.SetBindings: key names must be strings");
        keys.emplace_back(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    std::string error;
    try {
        ActionMap::SetKeys(name, field, keys);
    } catch (const std::exception &failure) {
        error = failure.what();
    }
    if (!error.empty()) return luaL_error(L, "Input.SetBindings: %s", error.c_str());
    return 0;
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
        .addFunction("GetAnyKeyDown", &Input::GetAnyKeyDown)
        .addFunction("GetKeyUp", &CppInputGetKeyUp)
        .addFunction("GetKeyDownName", &CppInputGetKeyDownName)
        .addFunction("GetBindings", &CppInputGetBindings)
        .addFunction("SetBindings", &CppInputSetBindings)
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
