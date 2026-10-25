#include "audio/AudioManager.h"
#include "shared/resources/ResourcePath.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void WriteLittleEndian(std::ostream &out, std::uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        out.put(static_cast<char>((value >> (8 * i)) & 0xff));
    }
}

void WriteTone(const std::filesystem::path &path) {
    constexpr int sample_rate = 48000;
    constexpr int samples = sample_rate / 10;
    constexpr int data_bytes = samples * 2;
    std::ofstream output(path, std::ios::binary);
    output.write("RIFF", 4);
    WriteLittleEndian(output, 36 + data_bytes, 4);
    output.write("WAVEfmt ", 8);
    WriteLittleEndian(output, 16, 4);
    WriteLittleEndian(output, 1, 2); // PCM
    WriteLittleEndian(output, 1, 2); // mono
    WriteLittleEndian(output, sample_rate, 4);
    WriteLittleEndian(output, sample_rate * 2, 4);
    WriteLittleEndian(output, 2, 2);
    WriteLittleEndian(output, 16, 2);
    output.write("data", 4);
    WriteLittleEndian(output, data_bytes, 4);
    for (int i = 0; i < samples; ++i) {
        const auto sample = static_cast<std::int16_t>(
            1000.0 * std::sin(i * 6.283185307179586 * 440.0 / sample_rate));
        WriteLittleEndian(output, static_cast<std::uint16_t>(sample), 2);
    }
    Check(output.good(), "failed to write temporary WAV fixture");
}

class AudioFixture {
public:
    AudioFixture() : previous_root_(ResourcePath::ResourcesRootPath()) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() /
                ("pocket2d-audio-test-" + std::to_string(stamp));
        // Refuse to reuse an existing directory: this test only removes its own files.
        Check(std::filesystem::create_directory(root_), "temporary directory collision");
        try {
            std::filesystem::create_directory(root_ / "audio");
            WriteTone(root_ / "audio" / "tone.wav");
            ResourcePath::SetResourcesRootPath(root_);
            Check(SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy"), "dummy audio driver unavailable");
        } catch (...) {
            ResourcePath::SetResourcesRootPath(previous_root_);
            std::filesystem::remove_all(root_);
            throw;
        }
    }

    ~AudioFixture() {
        AudioManager::Shutdown();
        ResourcePath::SetResourcesRootPath(previous_root_);
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

private:
    std::filesystem::path previous_root_;
    std::filesystem::path root_;
};

void TestAudio() {
    AudioFixture fixture;
    Check(AudioManager::Init(), "audio initialization failed");
    Check(AudioManager::Init(), "audio initialization must be idempotent");
    Check(AudioManager::PreloadAudioClip("tone"), "clip preload failed");
    Check(AudioManager::PreloadMusicTrack("tone"), "music preload failed");
    Check(!AudioManager::PreloadAudioClip("absent"), "missing clip must fail preload");

    for (int channel = 0; channel < 64; ++channel) {
        AudioManager::PlayAudioClip("tone", channel, -1);
        Check(AudioManager::IsChannelPlaying(channel), "channel did not start");
    }
    const int failures = AudioManager::GetAudioPlayFailureCount();
    AudioManager::PlayAudioClip("tone", -1, 0);
    Check(AudioManager::GetAudioPlayFailureCount() == failures + 1,
          "automatic channel selection must fail when all 64 are occupied");
    AudioManager::PlayAudioClip("tone", 64, 0);
    Check(AudioManager::GetAudioPlayFailureCount() == failures + 2,
          "out-of-range channel must report failure");
    Check(!AudioManager::IsChannelPlaying(64) && !AudioManager::IsChannelPlaying(-2),
          "invalid channel must not report playback");

    AudioManager::PlayMusicTrack("tone", -1);
    Check(AudioManager::IsMusicPlaying(), "music must have an independent track");
    AudioManager::HaltChannel(12);
    Check(!AudioManager::IsChannelPlaying(12), "single-channel halt failed");
    AudioManager::PlayAudioClip("tone", -1, -1);
    Check(AudioManager::IsChannelPlaying(12), "automatic selection must reuse the free channel");
    AudioManager::SetVolume(-1, 0);
    AudioManager::SetVolume(63, 128);
    AudioManager::SetMusicVolume(64);
    AudioManager::HaltChannel(-1);
    Check(!AudioManager::IsChannelPlaying(-1), "halt-all must stop clip tracks");
    Check(AudioManager::IsMusicPlaying(), "halt-all clip tracks must preserve music");
    AudioManager::HaltMusic();
    Check(!AudioManager::IsMusicPlaying(), "music halt failed");

    AudioManager::PlayAudioClip("tone", 0, 0);
    SDL_Delay(400);
    Check(!AudioManager::IsChannelPlaying(0), "non-looping clip must finish");
    AudioManager::PlayAudioClip("tone", 0, 1);
    SDL_Delay(400);
    Check(!AudioManager::IsChannelPlaying(0), "finite-loop clip must finish");
    AudioManager::PlayAudioClip("tone", 0, -1);
    SDL_Delay(400);
    Check(AudioManager::IsChannelPlaying(0), "infinite-loop clip must keep playing");

    AudioManager::PlayMusicTrack("tone", -1);
    AudioManager::SetPlaybackEnabled(false);
    Check(!AudioManager::IsChannelPlaying(-1) && !AudioManager::IsMusicPlaying(),
          "disabling playback must stop clips and music");
    AudioManager::PlayAudioClip("tone", 0, -1);
    Check(!AudioManager::IsChannelPlaying(0), "disabled playback must not restart clips");
    AudioManager::Shutdown();
    AudioManager::Shutdown();
    Check(AudioManager::Init(), "audio must reinitialize after shutdown");
    Check(AudioManager::GetAudioPlayFailureCount() == 0 &&
              AudioManager::GetMusicPlayFailureCount() == 0,
          "shutdown must reset failure counters");
    Check(AudioManager::IsPlaybackEnabled(), "shutdown must reset playback enable state");
}
} // namespace

int main() {
    try {
        TestAudio();
        std::cout << "native SDL3 audio regressions passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
