#include "RegistrationDetail.h"
#include "audio/AudioManager.h"
#include "audio/SoundLedger.h"
#include "net/Netplay.h"
#include "scripting/EventBus.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>
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

// Session sounds (below) also name their tick, their place among the
// tick's requests of the clip and how late a correction started them.
void TracePlay(bool music, int channel, const std::string &clip_name, bool does_loop,
               const SoundLedger::Decision *session = nullptr) {
    if (!EventBus::IsPublishTracing()) return;
    const bool found = music ? AudioManager::HasMusicTrack(clip_name) : AudioManager::HasAudioClip(clip_name);
    std::string payload = "{";
    if (!music) payload += "\"channel\":" + std::to_string(channel) + ",";
    payload += "\"clip\":" + JsonString(clip_name) + ",\"loop\":" + (does_loop ? "true" : "false") +
               ",\"found\":" + (found ? "true" : "false");
    if (session)
        payload += ",\"tick\":" + std::to_string(session->frame) + ",\"nth\":" + std::to_string(session->nth) +
                   ",\"late\":" + std::to_string(session->late);
    EventBus::TraceEngineEvent(music ? "music.play" : "audio.play", payload + "}");
}

// Netplay session ticks (rollback's corrections) play sound effects through a
// SoundLedger, as GGPO games do: a tick simulated again does not repeat the
// sounds it already played; a sound that first appears in a correction (the
// prediction missed a hit) plays then, from its start while it is at most
// kLateTicks late and further in after that, so long sounds stay in time; a
// corrected tick's sound that the correction did not ask for again was
// mispredicted and fades out. Effects play as voices on a free channel (the
// asked one when free), so a late sound cannot cut off one already playing.
// The game's channel numbers keep their gain (SetVolume) and name their
// latest voice for Halt, IsPlaying and later gain changes. Music is level
// triggered: a correction's last music command is applied when it ends.
constexpr std::uint32_t kLateTicks = 6;
constexpr int kTickMs = 1000 / 60;
constexpr int kCancelFadeMs = 40;

struct SessionAudio {
    SoundLedger ledger;
    bool in_tick = false, resimulating = false;
    std::array<float, 64> gain;
    std::array<std::int64_t, 64> voice;
    std::optional<std::pair<std::string, bool>> correction_music; // (clip, loop); empty clip: halt
    SessionAudio() { gain.fill(1.0f); voice.fill(-1); }
};

SessionAudio &Session() {
    static SessionAudio session;
    return session;
}

bool ChannelIndex(int channel) { return channel >= 0 && channel < 64; }

void ApplyMusic(const std::string &clip_name, bool does_loop) {
    if (clip_name.empty()) EventBus::TraceEngineEvent("music.halt", "{}");
    else TracePlay(true, 0, clip_name, does_loop);
    if (!EnsureAudioInitialized()) return;
    if (clip_name.empty()) AudioManager::HaltMusic();
    else AudioManager::PlayMusicTrack(clip_name, does_loop ? -1 : 0);
}

void InstallSessionAudio() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    Netplay::SetTickObserver({
        [](std::uint32_t frame, bool resimulating) {
            auto &a = Session();
            a.in_tick = true;
            a.resimulating = resimulating;
            a.ledger.BeginTick(frame, resimulating);
        },
        [] { Session().in_tick = Session().resimulating = false; },
        [](std::uint32_t from, std::uint32_t to) {
            Session().ledger.BeginCorrection(from, to);
            Session().correction_music.reset();
        },
        [] {
            auto &a = Session();
            for (const auto &c : a.ledger.EndCorrection()) {
                if (EventBus::IsPublishTracing())
                    EventBus::TraceEngineEvent("audio.cancel", "{\"clip\":" + JsonString(c.clip) + ",\"tick\":" +
                                                               std::to_string(c.frame) + ",\"nth\":" + std::to_string(c.nth) + "}");
                if (c.voice >= 0) AudioManager::StopVoice(c.voice, kCancelFadeMs);
            }
            if (a.correction_music) ApplyMusic(a.correction_music->first, a.correction_music->second);
            a.correction_music.reset();
        },
    });
}

void SessionPlay(int channel, const std::string &clip_name, bool does_loop) {
    auto &a = Session();
    const SoundLedger::Decision d = a.ledger.Request(clip_name);
    if (!d.play) return;
    TracePlay(false, channel, clip_name, does_loop, &d);
    if (!EnsureAudioInitialized()) return;
    const int start_ms = d.late > kLateTicks ? static_cast<int>(d.late) * kTickMs : 0;
    const float gain = ChannelIndex(channel) ? a.gain[static_cast<std::size_t>(channel)] : 1.0f;
    const std::int64_t voice = AudioManager::PlayVoice(clip_name, channel, does_loop ? -1 : 0, gain, start_ms);
    a.ledger.SetVoice(voice);
    if (ChannelIndex(channel)) a.voice[static_cast<std::size_t>(channel)] = voice;
}

void CppAudioPlay(int channel, const std::string &clip_name, bool does_loop) {
    if (Session().in_tick) { SessionPlay(channel, clip_name, does_loop); return; }
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
    auto &a = Session();
    if (a.in_tick && a.resimulating) return; // a halt cannot be told from the first run's
    if (!EnsureAudioInitialized()) return;
    if (a.in_tick && ChannelIndex(channel)) AudioManager::StopVoice(a.voice[static_cast<std::size_t>(channel)], 0);
    else AudioManager::HaltChannel(channel);
}

void CppAudioSetVolume(int channel, float volume) {
    int volume_i = static_cast<int>(volume);
    volume_i = std::clamp(volume_i, 0, 128);
    auto &a = Session();
    if (a.in_tick) {
        // Kept for the channel's next sound; the first run already set the playing one's.
        const float gain = static_cast<float>(volume_i) / 128.0f;
        if (ChannelIndex(channel)) a.gain[static_cast<std::size_t>(channel)] = gain;
        else if (channel == -1) a.gain.fill(gain);
        if (a.resimulating || !EnsureAudioInitialized()) return;
        if (ChannelIndex(channel)) AudioManager::SetVoiceGain(a.voice[static_cast<std::size_t>(channel)], gain);
        else AudioManager::SetVolume(channel, volume_i);
        return;
    }
    if (!EnsureAudioInitialized()) return;
    AudioManager::SetVolume(channel, volume_i);
}

bool CppAudioIsPlaying(int channel) {
    if (Session().in_tick && ChannelIndex(channel))
        return AudioManager::IsVoicePlaying(Session().voice[static_cast<std::size_t>(channel)]);
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
    if (Session().resimulating) { Session().correction_music.emplace(clip_name, does_loop); return; }
    TracePlay(true, 0, clip_name, does_loop);
    if (!EnsureAudioInitialized()) return;
    const int loops = does_loop ? -1 : 0;
    AudioManager::PlayMusicTrack(clip_name, loops);
}

void CppMusicHalt() {
    if (Session().resimulating) { Session().correction_music.emplace(std::string(), false); return; }
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
    InstallSessionAudio();
    InjectAudioAPI();
    InjectMusicAPI();
}

} // namespace APIRegistrationDetail
