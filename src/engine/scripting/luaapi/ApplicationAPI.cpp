#include "RegistrationDetail.h"
#include "core/Log.h"
#include "core/FrameClock.h"
#include "core/Engine.h"
#include "shared/resources/ResourcePath.h"
#include "SDL3/SDL.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

using APIRegistrationDetail::LuaState;
using APIRegistrationDetail::BoundEngine;

void CppLog(const std::string &message) {
    Log::Info("script", message);
}

void InjectDebugAPI() {
    luabridge::getGlobalNamespace(LuaState())
        .beginNamespace("Debug")
        .addFunction("Log", &CppLog)
        .endNamespace();
}

void CppApplicationQuit() {
    if (BoundEngine() != nullptr) BoundEngine()->RequestQuit();
}

void CppApplicationSleep(int milliseconds) {
    if (milliseconds < 0) milliseconds = 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

int CppApplicationGetFrame() {
    return FrameClock::GetFrameNumber();
}

float CppApplicationGetDeltaTime() {
    return FrameClock::GetDeltaTime();
}

int CppApplicationGetWindowWidth() {
    if (BoundEngine() == nullptr) return 640;
    const int runtime_width = BoundEngine()->GetRuntimeRenderTargetWidth();
    if (runtime_width > 0) return runtime_width;
    return BoundEngine()->GetWindowWidth();
}

int CppApplicationGetWindowHeight() {
    if (BoundEngine() == nullptr) return 360;
    const int runtime_height = BoundEngine()->GetRuntimeRenderTargetHeight();
    if (runtime_height > 0) return runtime_height;
    return BoundEngine()->GetWindowHeight();
}

// Absolute path of the loaded project's resource root, for games that read
// their own data files on demand (e.g. generated content too large to load at startup).
// The system clipboard (empty and false without a video subsystem, e.g. headless).
std::string CppApplicationGetClipboardText() {
    if (!SDL_WasInit(SDL_INIT_VIDEO) || !SDL_HasClipboardText()) return "";
    char *text = SDL_GetClipboardText();
    std::string out = text ? text : "";
    SDL_free(text);
    return out;
}

bool CppApplicationSetClipboardText(const std::string &text) {
    return SDL_WasInit(SDL_INIT_VIDEO) && SDL_SetClipboardText(text.c_str());
}

std::string CppApplicationGetProjectDirectory() {
    return ResourcePath::ResourcesRootPath().lexically_normal().string();
}

void CppApplicationOpenURL(const std::string &url) {
    std::string command;
#ifdef _WIN32
    command = "start \"\" \"" + url + "\"";
#elif __APPLE__
    command = "open \"" + url + "\"";
#else
    command = "xdg-open \"" + url + "\"";
#endif
    [[maybe_unused]] const int result = std::system(command.c_str());
}

void InjectApplicationAPI() {
    luabridge::getGlobalNamespace(LuaState())
        .beginNamespace("Application")
        .addFunction("Quit", &CppApplicationQuit)
        .addFunction("Sleep", &CppApplicationSleep)
        .addFunction("GetFrame", &CppApplicationGetFrame)
        .addFunction("GetDeltaTime", &CppApplicationGetDeltaTime)
        .addFunction("GetWindowWidth", &CppApplicationGetWindowWidth)
        .addFunction("GetWindowHeight", &CppApplicationGetWindowHeight)
        .addFunction("OpenURL", &CppApplicationOpenURL)
        .addFunction("GetProjectDirectory", &CppApplicationGetProjectDirectory)
        .addFunction("GetClipboardText", &CppApplicationGetClipboardText)
        .addFunction("SetClipboardText", &CppApplicationSetClipboardText)
        .endNamespace();
}

} // namespace

namespace APIRegistrationDetail {

void RegisterApplicationAndDebugAPI() {
    InjectDebugAPI();
    InjectApplicationAPI();
}

} // namespace APIRegistrationDetail
