#ifndef POCKET2D_SHARED_TILEMAP_ASSET_H
#define POCKET2D_SHARED_TILEMAP_ASSET_H

#include <filesystem>
#include <string>
#include <vector>

namespace TilemapAsset {
struct Atlas {
    std::string image;
    int columns = 1;
    int rows = 1;
    int tile_width_px = 16;
    int tile_height_px = 16;
};
struct Layer {
    int sorting_order = 0;
    std::vector<int> cells;
};
struct Data {
    int version = 1;
    int width = 0;
    int height = 0;
    float cell_size = 1.0f;
    Atlas atlas;
    Layer ground{-100, {}};
    Layer solid{0, {}};
    // Derived at load time; not written into the portable JSON resource.
    std::filesystem::path resolved_image_path;
};

Layer &GetLayer(Data &data, const std::string &layer);
const Layer &GetLayer(const Data &data, const std::string &layer);
void ValidateData(const Data &data);
void ValidateAtlasImage(Data &data, const std::filesystem::path &images_root);
Data LoadFile(const std::filesystem::path &path,
              const std::filesystem::path &images_root);
Data LoadResource(const std::string &map_resource);
void SaveFile(const std::filesystem::path &path, const Data &data);
void SaveResource(const std::string &map_resource, const Data &data);
} // namespace TilemapAsset

#endif
