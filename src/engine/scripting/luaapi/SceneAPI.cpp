#include "RegistrationDetail.h"
#include "core/Vector2.h"
#include "core/Engine.h"
#include "scene/ActorHandle.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <string>

namespace {

using APIRegistrationDetail::BoundEngine;
using APIRegistrationDetail::LuaState;

void CppSceneLoad(const std::string &scene_name) {
    if (BoundEngine() == nullptr) return;
    BoundEngine()->RequestSceneLoad(scene_name);
}

std::string CppSceneGetCurrent() {
    if (BoundEngine() == nullptr) return "";
    return BoundEngine()->GetCurrentSceneName();
}

void CppSceneDontDestroy(const ActorHandle *actor) {
    if (BoundEngine() == nullptr || actor == nullptr) return;
    BoundEngine()->MarkActorDontDestroy(actor->Resolve());
}

void InjectSceneAPI() {
    luabridge::getGlobalNamespace(LuaState())
        .beginNamespace("Scene")
        .addFunction("Load", &CppSceneLoad)
        .addFunction("GetCurrent", &CppSceneGetCurrent)
        .addFunction("DontDestroy", &CppSceneDontDestroy)
        .endNamespace();
}

void CppCameraSetPosition(float x, float y) {
    if (BoundEngine() == nullptr) return;
    BoundEngine()->SetCameraPosition(x, y);
}

float CppCameraGetPositionX() {
    if (BoundEngine() == nullptr) return 0.0f;
    return BoundEngine()->GetCameraPositionX();
}

float CppCameraGetPositionY() {
    if (BoundEngine() == nullptr) return 0.0f;
    return BoundEngine()->GetCameraPositionY();
}

void CppCameraSetZoom(float zoom_factor) {
    if (BoundEngine() == nullptr) return;
    BoundEngine()->SetCameraZoom(zoom_factor);
}

float CppCameraGetZoom() {
    if (BoundEngine() == nullptr) return 1.0f;
    return BoundEngine()->GetCameraZoom();
}

Vector2 CppCameraScreenToWorld(float x, float y) {
    if (BoundEngine() == nullptr) return {x, y};
    return BoundEngine()->ScreenToWorld(x, y);
}

Vector2 CppCameraWorldToScreen(float x, float y) {
    if (BoundEngine() == nullptr) return {x, y};
    return BoundEngine()->WorldToScreen(x, y);
}

Vector2 CppCameraGetScreenSize() {
    if (BoundEngine() == nullptr) return {0.0f, 0.0f};
    return BoundEngine()->GetScreenSize();
}

Vector2 CppCameraGetViewSize() {
    if (BoundEngine() == nullptr) return {0.0f, 0.0f};
    return BoundEngine()->GetViewSize();
}

void InjectCameraAPI() {
    luabridge::getGlobalNamespace(LuaState())
        .beginNamespace("Camera")
        .addFunction("SetPosition", &CppCameraSetPosition)
        .addFunction("GetPositionX", &CppCameraGetPositionX)
        .addFunction("GetPositionY", &CppCameraGetPositionY)
        .addFunction("SetZoom", &CppCameraSetZoom)
        .addFunction("GetZoom", &CppCameraGetZoom)
        .addFunction("ScreenToWorld", &CppCameraScreenToWorld)
        .addFunction("WorldToScreen", &CppCameraWorldToScreen)
        .addFunction("GetScreenSize", &CppCameraGetScreenSize)
        .addFunction("GetViewSize", &CppCameraGetViewSize)
        .endNamespace();
}

} // namespace

namespace APIRegistrationDetail {

void RegisterSceneAndCameraAPI() {
    InjectSceneAPI();
    InjectCameraAPI();
}

} // namespace APIRegistrationDetail
