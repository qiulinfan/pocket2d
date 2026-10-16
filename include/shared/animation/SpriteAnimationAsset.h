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
    // Optional own sheet (same tile pixel size as the resource); empty image
    // means the resource's atlas. Lets one-sheet-per-action art (Unity
    // Resources folders, Aseprite tags exported separately) animate unpacked.
    std::string image;
    int columns = 0;
    int rows = 0;
    // Derived on validation; never a portable JSON field.
    std::filesystem::path resolved_image_path;
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
// The sheet a clip draws from: its own when set, else the resource atlas.
int ClipColumns(const Data &data, const Clip &clip);
int ClipRows(const Data &data, const Clip &clip);
std::string ClipImagePath(const Data &data, const Clip &clip);
} // namespace SpriteAnimationAsset

#endif
