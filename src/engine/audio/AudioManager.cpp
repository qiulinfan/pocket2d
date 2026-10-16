#include "audio/AudioManager.h"
#include "core/World.h"
#include "shared/resources/ResourcePath.h"
#include "scene/Scene.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <unordered_set>

namespace {

// The audio device and its caches are process-wide. Only Worlds that use audio
// output touch them; headless Worlds never do, so they cannot stop or race the
// presenting World's playback.
bool UsesAudioOutput() {
    return World::Current().audio_output_enabled;
}

std::unordered_set<std::string> g_audio_warning_keys;
int g_audio_resource_warning_count = 0;
std::string g_audio_last_resource_warning;

bool StartTrack(MIX_Track *track, MIX_Audio *audio, int loops) {
    const SDL_PropertiesID options = SDL_CreateProperties();
    if (options == 0) return false;
    const bool configured = SDL_SetNumberProperty(
        options, MIX_PROP_PLAY_LOOPS_NUMBER, loops < 0 ? -1 : loops);
    const bool started = configured && MIX_SetTrackAudio(track, audio) &&
                         MIX_PlayTrack(track, options);
    SDL_DestroyProperties(options);
    return started;
}

bool ShouldLogAudioDiagnostics() {
    return std::getenv("ENGINE_LOG_AUDIO") != nullptr;
}

void LogPlaybackFailure(const char *kind, const std::string &audio_name,
                        int failure_count) {
    if (!ShouldLogAudioDiagnostics()) return;
    if (failure_count > 5 && (failure_count % 25) != 0) return;
    std::cout << "warning: " << kind << " playback failed for ["
              << audio_name << "]: " << SDL_GetError() << std::endl;
}

void LogAudioWarningOnce(const std::string &key, const std::string &message) {
    if (g_audio_warning_keys.find(key) != g_audio_warning_keys.end()) {
        return;
    }
    g_audio_warning_keys.insert(key);
    g_audio_last_resource_warning = message;
    ++g_audio_resource_warning_count;
    std::cout << "warning: " << g_audio_last_resource_warning << std::endl;
}

} // namespace

// initialize SDL_mixer once and reuse it after that
bool AudioManager::Init() {
    if (!UsesAudioOutput()) return false;

    if (initialized) return true;
    if (init_attempted) return false;

    // Try audio backend once. In CI/headless machines ALSA/CoreAudio may be
    // missing; repeated retries would spam stderr every frame.
    init_attempted = true;
    mixer_library_initialized = MIX_Init();
    if (mixer_library_initialized) {
        const SDL_AudioSpec requested{SDL_AUDIO_F32, 2, 48000};
        mixer = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &requested);
    }
    if (mixer != nullptr) {
        for (MIX_Track *&channel : channels) {
            channel = MIX_CreateTrack(mixer);
            if (channel == nullptr) break;
        }
        music_track = MIX_CreateTrack(mixer);
    }
    const bool tracks_ready = mixer != nullptr && music_track != nullptr &&
        std::all_of(channels.begin(), channels.end(),
                    [](MIX_Track *track) { return track != nullptr; });
    if (!tracks_ready) {
        if (!init_failure_logged) {
            std::cout << "warning: audio disabled (failed to initialize mixer): "
                      << SDL_GetError() << std::endl;
            init_failure_logged = true;
        }
        // DestroyMixer owns its tracks, including partial initialization.
        MIX_DestroyMixer(mixer);
        mixer = nullptr;
        channels.fill(nullptr);
        music_track = nullptr;
        if (mixer_library_initialized) MIX_Quit();
        mixer_library_initialized = false;
        return false;
    }
    SDL_AudioSpec actual{};
    if (MIX_GetMixerFormat(mixer, &actual)) {
        actual_output_frequency = actual.freq;
        actual_output_format = actual.format;
        actual_output_channels = actual.channels;
    }
    if (ShouldLogAudioDiagnostics()) {
        std::cout << "audio: opened device at " << actual_output_frequency
                  << " Hz, format " << actual_output_format << ", channels "
                  << actual_output_channels << std::endl;
    }
    
    initialized = true;
    return true;
}

void AudioManager::Shutdown() {
    if (!UsesAudioOutput()) return;

    if (initialized) {
        StopAllPlayback();
    }

    for (auto &entry : audio_cache) {
        if (entry.second != nullptr) {
            MIX_DestroyAudio(entry.second);
        }
    }
    audio_cache.clear();

    for (auto &entry : music_cache) {
        if (entry.second != nullptr) {
            MIX_DestroyAudio(entry.second);
        }
    }
    music_cache.clear();

    MIX_DestroyMixer(mixer);
    mixer = nullptr;
    channels.fill(nullptr);
    music_track = nullptr;
    if (mixer_library_initialized) MIX_Quit();
    mixer_library_initialized = false;

    initialized = false;
    init_attempted = false;
    init_failure_logged = false;
    playback_enabled = true;
    current_music_name.clear();
    current_music_loops = 0;
    audio_play_failure_count = 0;
    music_play_failure_count = 0;
    g_audio_warning_keys.clear();
    g_audio_resource_warning_count = 0;
    g_audio_last_resource_warning.clear();
    actual_output_frequency = 0;
    actual_output_format = SDL_AUDIO_UNKNOWN;
    actual_output_channels = 0;
}

// supports explicit extension and default wav / ogg lookup
std::string AudioManager::FindAudioFile(const std::string &base_name) {
    return ResourcePath::ResolveResourcePath(
        ResourcePath::ResourceSubdirectory("audio"), base_name,
        {".wav", ".ogg", ".mp3"},
        Scene::GetActiveSceneSubdirectory());
}

// remove the extension from a clip name, mainly for error output
std::string AudioManager::ClipNameSansExtension(const std::string &audio_name) {
    size_t dot_pos = audio_name.find_last_of('.');
    if (dot_pos == std::string::npos) return audio_name;
    return audio_name.substr(0, dot_pos);
}

// load audio by name and cache it
MIX_Audio *AudioManager::LoadAudioClip(const std::string &audio_name) {
    if (!UsesAudioOutput()) return nullptr;

    auto it = audio_cache.find(audio_name);
    if (it != audio_cache.end()) return it->second;

    std::string file_path = FindAudioFile(audio_name);
    if (file_path.empty()) {
        LogAudioWarningOnce("clip:missing:" + audio_name,
                            "audio clip [" +
                                ClipNameSansExtension(audio_name) +
                                "] missing; skipping playback");
        return nullptr;
    }

    MIX_Audio *chunk = MIX_LoadAudio(mixer, file_path.c_str(), true);
    if (chunk == nullptr) {
        LogAudioWarningOnce("clip:load:" + audio_name,
                            "audio clip [" +
                                ClipNameSansExtension(audio_name) +
                                "] failed to load; skipping playback");
        return nullptr;
    }

    audio_cache[audio_name] = chunk;
    return chunk;
}

MIX_Audio *AudioManager::LoadMusicTrack(const std::string &audio_name) {
    if (!UsesAudioOutput()) return nullptr;

    auto it = music_cache.find(audio_name);
    if (it != music_cache.end()) return it->second;

    const std::string file_path = FindAudioFile(audio_name);
    if (file_path.empty()) {
        LogAudioWarningOnce("music:missing:" + audio_name,
                            "music track [" +
                                ClipNameSansExtension(audio_name) +
                                "] missing; skipping playback");
        return nullptr;
    }

    MIX_Audio *music = MIX_LoadAudio(mixer, file_path.c_str(), false);
    if (music == nullptr) {
        LogAudioWarningOnce("music:load:" + audio_name,
                            "music track [" +
                                ClipNameSansExtension(audio_name) +
                                "] failed to load; skipping playback");
        return nullptr;
    }

    music_cache[audio_name] = music;
    return music;
}

bool AudioManager::HasAudioClip(const std::string &audio_name) {
    if (!UsesAudioOutput()) return !FindAudioFile(audio_name).empty();

    if (audio_cache.find(audio_name) != audio_cache.end()) return true;
    return !FindAudioFile(audio_name).empty();
}

bool AudioManager::HasMusicTrack(const std::string &audio_name) {
    if (!UsesAudioOutput()) return !FindAudioFile(audio_name).empty();

    if (music_cache.find(audio_name) != music_cache.end()) return true;
    return !FindAudioFile(audio_name).empty();
}

bool AudioManager::PreloadAudioClip(const std::string &audio_name) {
    if (!UsesAudioOutput()) return false;

    if (!Init()) return false;
    if (audio_cache.find(audio_name) != audio_cache.end()) return true;

    const std::string file_path = FindAudioFile(audio_name);
    if (file_path.empty()) return false;

    MIX_Audio *chunk = MIX_LoadAudio(mixer, file_path.c_str(), true);
    if (chunk == nullptr) return false;

    audio_cache[audio_name] = chunk;
    return true;
}

bool AudioManager::PreloadMusicTrack(const std::string &audio_name) {
    if (!UsesAudioOutput()) return false;

    if (!Init()) return false;
    if (music_cache.find(audio_name) != music_cache.end()) return true;

    const std::string file_path = FindAudioFile(audio_name);
    if (file_path.empty()) return false;

    MIX_Audio *music = MIX_LoadAudio(mixer, file_path.c_str(), false);
    if (music == nullptr) return false;

    music_cache[audio_name] = music;
    return true;
}

// Play one named clip on the requested channel using the cached chunk.
void AudioManager::PlayAudioClip(const std::string &audio_name, int channel,
                                 int loops) {
    if (!UsesAudioOutput()) return;
    if (!initialized || !playback_enabled) return;
    MIX_Audio *chunk = LoadAudioClip(audio_name);
    if (chunk == nullptr) {
        ++audio_play_failure_count;
        return;
    }
    MIX_Track *track = nullptr;
    if (channel == -1) {
        const auto free_channel = std::find_if(channels.begin(), channels.end(),
            [](MIX_Track *candidate) { return !MIX_TrackPlaying(candidate); });
        if (free_channel != channels.end()) track = *free_channel;
    } else if (channel >= 0 && channel < static_cast<int>(channels.size())) {
        track = channels[static_cast<std::size_t>(channel)];
    }
    if (track != nullptr && StartTrack(track, chunk, loops)) return;
    if (track == nullptr) SDL_SetError("No available audio channel");
    ++audio_play_failure_count;
    LogPlaybackFailure("audio", audio_name, audio_play_failure_count);
}

void AudioManager::PlayMusicTrack(const std::string &audio_name, int loops) {
    if (!UsesAudioOutput()) return;
    if (!initialized || !playback_enabled) return;
    if (IsMusicPlaying() && current_music_name == audio_name &&
        current_music_loops == loops) {
        return;
    }
    MIX_Audio *music = LoadMusicTrack(audio_name);
    if (music == nullptr) {
        ++music_play_failure_count;
        return;
    }
    if (!StartTrack(music_track, music, loops)) {
        ++music_play_failure_count;
        LogPlaybackFailure("music", audio_name, music_play_failure_count);
        return;
    }
    current_music_name = audio_name;
    current_music_loops = loops;
}

// Stop playback on one mixer channel, or all channels when channel == -1.
void AudioManager::HaltChannel(int channel) {
    if (!UsesAudioOutput()) return;

    if (!initialized) return;
    if (channel == -1) {
        for (MIX_Track *track : channels) MIX_StopTrack(track, 0);
    } else if (channel >= 0 && channel < static_cast<int>(channels.size())) {
        MIX_StopTrack(channels[static_cast<std::size_t>(channel)], 0);
    }
}

// Update mixer volume for one channel without touching the cached clip data.
void AudioManager::SetVolume(int channel, int volume) {
    if (!UsesAudioOutput()) return;

    if (!initialized) return;
    if (volume < 0) return; // The legacy negative-volume query never changed gain.
    const float gain = static_cast<float>(std::clamp(volume, 0, 128)) / 128.0f;
    if (channel == -1) {
        for (MIX_Track *track : channels) MIX_SetTrackGain(track, gain);
    } else if (channel >= 0 && channel < static_cast<int>(channels.size())) {
        MIX_SetTrackGain(channels[static_cast<std::size_t>(channel)], gain);
    }
}

bool AudioManager::IsChannelPlaying(int channel) {
    if (!UsesAudioOutput()) return false;

    if (!initialized) return false;
    if (channel == -1) {
        return std::any_of(channels.begin(), channels.end(),
                          [](MIX_Track *track) { return MIX_TrackPlaying(track); });
    }
    return channel >= 0 && channel < static_cast<int>(channels.size()) &&
        MIX_TrackPlaying(channels[static_cast<std::size_t>(channel)]);
}

void AudioManager::HaltMusic() {
    if (!UsesAudioOutput()) return;

    if (!initialized) return;
    MIX_StopTrack(music_track, 0);
    current_music_name.clear();
    current_music_loops = 0;
}

void AudioManager::SetMusicVolume(int volume) {
    if (!UsesAudioOutput()) return;

    if (!initialized) return;
    if (volume < 0) return;
    MIX_SetTrackGain(music_track,
                    static_cast<float>(std::clamp(volume, 0, 128)) / 128.0f);
}

bool AudioManager::IsMusicPlaying() {
    if (!UsesAudioOutput()) return false;

    if (!initialized) return false;
    return MIX_TrackPlaying(music_track);
}

void AudioManager::StopAllPlayback() {
    if (!UsesAudioOutput()) return;

    if (!initialized) return;
    HaltChannel(-1);
    MIX_StopTrack(music_track, 0);
    current_music_name.clear();
    current_music_loops = 0;
}

void AudioManager::SetPlaybackEnabled(bool enabled) {
    if (!UsesAudioOutput()) return;

    playback_enabled = enabled;
    if (!playback_enabled) {
        StopAllPlayback();
    }
}

bool AudioManager::IsPlaybackEnabled() {
    if (!UsesAudioOutput()) return false;

    return playback_enabled;
}

int AudioManager::GetAudioPlayFailureCount() {
    if (!UsesAudioOutput()) return 0;

    return audio_play_failure_count;
}

int AudioManager::GetMusicPlayFailureCount() {
    if (!UsesAudioOutput()) return 0;

    return music_play_failure_count;
}

int AudioManager::GetResourceWarningCount() {
    return g_audio_resource_warning_count;
}

const std::string &AudioManager::GetLastResourceWarning() {
    return g_audio_last_resource_warning;
}
