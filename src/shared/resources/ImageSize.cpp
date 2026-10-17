#include "shared/resources/ImageSize.h"
#include <SDL3_image/SDL_image.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ImageSize {
Result Decoded(const std::filesystem::path &path) {
    struct Known { std::uintmax_t bytes; std::filesystem::file_time_type written; int width, height; };
    static std::mutex mutex;
    static std::unordered_map<std::string, Known> known;
    const auto text = path.generic_u8string();
    const std::string key(text.begin(), text.end());
    const std::uintmax_t bytes = std::filesystem::file_size(path);
    const auto written = std::filesystem::last_write_time(path);
    {
        std::lock_guard lock(mutex);
        const auto found = known.find(key);
        if (found != known.end() && found->second.bytes == bytes && found->second.written == written)
            return {found->second.width, found->second.height, {}};
    }
    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(IMG_Load(key.c_str()), SDL_DestroySurface);
    if (surface == nullptr) return {0, 0, SDL_GetError()};
    std::lock_guard lock(mutex);
    known[key] = Known{bytes, written, surface->w, surface->h};
    return {surface->w, surface->h, {}};
}
} // namespace ImageSize
