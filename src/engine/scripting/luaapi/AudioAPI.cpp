#include "RegistrationDetail.h"
#include "audio/AudioManager.h"
#include "scripting/EventBus.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <algorithm>
#include <cstdio>
#include <string>

namespace {

using APIRegistrationDetail::LuaState;

bool CanRouteAudioCommands() {
    // Top-level component scripts run before an Engine is bound. Respect the
    // headless host's disabled flag there too, before opening any audio device.
    return AudioManager::IsPlaybackEnabled();
}

bool EnsureAudioInitialized() {
    if (!CanRouteAudioCommands()) return false;
    return AudioManager::Init();
}

// Agents see every play request, with or without an audio device, and whether
// the clip resolves (a missing asset is otherwise silent).
std::string JsonString(const std::string &text) {
    std::string out = "\"";
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20) { char escaped[8]; std::snprintf(escaped, sizeof escaped, "\\u%04x", c); out += escaped; }
        else out += static_cast<char>(c);
    }
    return out + "\"";
}

void TracePlay(bool music, int channel, const std::string &clip_name, bool does_loop) {
    if (!EventBus::IsPublishTracing()) return;
    const bool found = music ? AudioManager::HasMusicTrack(clip_name) : AudioManager::HasAudioClip(clip_name);
    std::string payload = "{";
    if (!music) payload += "\"channel\":" + std::to_string(channel) + ",";
    payload += "\"clip\":" + JsonString(clip_name) + ",\"loop\":" + (does_loop ? "true" : "false") +
               ",\"found\":" + (found ? "true" : "false") + "}";
    EventBus::TraceEngineEvent(music ? "music.play" : "audio.play", std::move(payload));
}

void CppAudioPlay(int channel, const std::string &clip_name, bool does_loop) {
    TracePlay(false, channel, clip_name, does_loop);
    if (!EnsureAudioInitialized()) return;
    const int loops = does_loop ? -1 : 0;
    AudioManager::PlayAudioClip(clip_name, channel, loops);
}

bool CppAudioHasClip(const std::string &clip_name) {
    return AudioManager::HasAudioClip(clip_name);
}

bool CppAudioPreload(const std::string &clip_name, bool as_music) {
    if (!CanRouteAudioCommands()) return false;
    if (as_music) return AudioManager::PreloadMusicTrack(clip_name);
    return AudioManager::PreloadAudioClip(clip_name);
}

void CppAudioHalt(int channel) {
    if (!EnsureAudioInitialized()) return;
    AudioManager::HaltChannel(channel);
}

void CppAudioSetVolume(int channel, float volume) {
    if (!EnsureAudioInitialized()) return;
    int volume_i = static_cast<int>(volume);
    volume_i = std::clamp(volume_i, 0, 128);
    AudioManager::SetVolume(channel, volume_i);
}

bool CppAudioIsPlaying(int channel) {
    return AudioManager::IsChannelPlaying(channel);
}

bool CppAudioIsPlaybackEnabled() {
    return AudioManager::IsPlaybackEnabled();
}

void InjectAudioAPI() {
    luabridge::getGlobalNamespace(LuaState())
        .beginNamespace("Audio")
        .addFunction("Play", &CppAudioPlay)
        .addFunction("HasClip", &CppAudioHasClip)
        .addFunction("Preload", &CppAudioPreload)
        .addFunction("Halt", &CppAudioHalt)
        .addFunction("SetVolume", &CppAudioSetVolume)
        .addFunction("IsPlaying", &CppAudioIsPlaying)
        .addFunction("IsPlaybackEnabled", &CppAudioIsPlaybackEnabled)
        .endNamespace();
}

void CppMusicPlay(const std::string &clip_name, bool does_loop) {
    TracePlay(true, 0, clip_name, does_loop);
    if (!EnsureAudioInitialized()) return;
    const int loops = does_loop ? -1 : 0;
    AudioManager::PlayMusicTrack(clip_name, loops);
}

void CppMusicHalt() {
    EventBus::TraceEngineEvent("music.halt", "{}");
    if (!EnsureAudioInitialized()) return;
    AudioManager::HaltMusic();
}

void CppMusicSetVolume(float volume) {
    if (!EnsureAudioInitialized()) return;
    int volume_i = static_cast<int>(volume);
    volume_i = std::clamp(volume_i, 0, 128);
    AudioManager::SetMusicVolume(volume_i);
}

bool CppMusicIsPlaying() {
    if (!EnsureAudioInitialized()) return false;
    return AudioManager::IsMusicPlaying();
}

void InjectMusicAPI() {
    luabridge::getGlobalNamespace(LuaState())
        .beginNamespace("Music")
        .addFunction("Play", &CppMusicPlay)
        .addFunction("Halt", &CppMusicHalt)
        .addFunction("SetVolume", &CppMusicSetVolume)
        .addFunction("IsPlaying", &CppMusicIsPlaying)
        .endNamespace();
}

} // namespace

namespace APIRegistrationDetail {

void RegisterAudioAPI() {
    InjectAudioAPI();
    InjectMusicAPI();
}

} // namespace APIRegistrationDetail
