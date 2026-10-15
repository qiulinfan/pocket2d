#include "RegistrationDetail.h"
#include "audio/AudioManager.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <algorithm>
#include <string>

namespace {

using APIRegistrationDetail::g_lua_state;

bool CanRouteAudioCommands() {
    // Top-level component scripts run before an Engine is bound. Respect the
    // headless host's disabled flag there too, before opening any audio device.
    return AudioManager::IsPlaybackEnabled();
}

bool EnsureAudioInitialized() {
    if (!CanRouteAudioCommands()) return false;
    return AudioManager::Init();
}

void CppAudioPlay(int channel, const std::string &clip_name, bool does_loop) {
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
    luabridge::getGlobalNamespace(g_lua_state)
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
    if (!EnsureAudioInitialized()) return;
    const int loops = does_loop ? -1 : 0;
    AudioManager::PlayMusicTrack(clip_name, loops);
}

void CppMusicHalt() {
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
    luabridge::getGlobalNamespace(g_lua_state)
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
