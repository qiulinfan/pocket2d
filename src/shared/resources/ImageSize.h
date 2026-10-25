#pragma once

#include <filesystem>
#include <string>

namespace ImageSize {
struct Result {
    int width = 0;
    int height = 0;
    std::string error;   // the decoder's message when the file cannot be decoded
};
// Pixel width and height of an image file. Asset validation asks for the same
// atlases on every load, so sizes are kept per file and the image is decoded
// again only when its size or write time changes.
Result Decoded(const std::filesystem::path &path);
} // namespace ImageSize
