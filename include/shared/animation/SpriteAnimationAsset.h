#ifndef POCKET2D_SHARED_SPRITE_ANIMATION_ASSET_H
#define POCKET2D_SHARED_SPRITE_ANIMATION_ASSET_H

#include <filesystem>
#include <string>
#include <vector>

namespace SpriteAnimationAsset {
struct Frame {
    int row = 1;
    int column = 1;
    int ticks = 1;
    std::string event;
};

struct Clip {
    std::string name;
    bool loop = false;
    std::vector<Frame> frames;
};

struct Data {
    int version = 1;
    std::string image;
    int columns = 0;
    int rows = 0;
    int tile_width_px = 0;
    int tile_height_px = 0;
    std::vector<Clip> clips;
    // Derived on successful atlas validation; never a portable JSON field.
    std::filesystem::path resolved_image_path;
};

void ValidateData(const Data &data);
void ValidateAtlasImage(Data &data, const std::filesystem::path &images_root);
Data LoadFile(const std::filesystem::path &path, const std::filesystem::path &images_root);
Data LoadResource(const std::string &relative);
const Clip *FindClip(const Data &data, const std::string &name);
} // namespace SpriteAnimationAsset

#endif
