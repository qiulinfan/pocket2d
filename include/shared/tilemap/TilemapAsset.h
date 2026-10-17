#ifndef POCKET2D_SHARED_TILEMAP_ASSET_H
#define POCKET2D_SHARED_TILEMAP_ASSET_H

#include <array>
#include <filesystem>
#include <functional>
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
// Autotiling: a terrain lists, for each of the 16 four-neighbour masks
// (bit 1 up, 2 right, 4 down, 8 left; set when that neighbour connects), the
// tile IDs to choose from. A cell belongs to a terrain when its ID appears in
// that terrain's lists; an ID may belong to at most one terrain.
struct Terrain {
    std::string name;
    bool connect_same = false;  // neighbours connect only within this terrain; else any nonzero tile
    std::array<std::vector<int>, 16> frames;
};
struct Autotile {
    bool frame_on_load = false;
    std::vector<Terrain> terrains;
};
// Extra decorative layers beyond the built-in ground and solid (which alone
// collides): up to six, drawn at their own sorting orders.
struct NamedLayer {
    std::string name;
    Layer layer;
};
constexpr int kMaxExtraLayers = 6;
// World-sized maps (a Terraria small world is 4200x1200): sides and the cell
// count are bounded so a layer stays a few MiB.
constexpr int kMaxSide = 4096;
constexpr int kMaxCells = 1 << 22;
struct Data {
    int version = 1;
    int width = 0;
    int height = 0;
    float cell_size = 1.0f;
    Atlas atlas;
    Layer ground{-100, {}};
    Layer solid{0, {}};
    std::vector<NamedLayer> extra_layers;
    Autotile autotile;
    // Derived at load time; not written into the portable JSON resource.
    std::filesystem::path resolved_image_path;
};

Layer &GetLayer(Data &data, const std::string &layer);
const Layer &GetLayer(const Data &data, const std::string &layer);
// Layers by index: 0 ground, 1 solid, then the extra layers in file order.
int LayerCount(const Data &data);
Layer &LayerAtIndex(Data &data, int index);
const Layer &LayerAtIndex(const Data &data, int index);
std::string LayerName(const Data &data, int index);
int FindLayerIndex(const Data &data, const std::string &layer); // -1 when absent
void ValidateData(const Data &data);
// tile ID -> terrain index, or -1; sized columns*rows+1.
std::vector<int> TerrainIndex(const Data &data);
// The tile a terrain cell shows for its neighbourhood. cell(x, y) returns the
// tile at a position (callers decide what lies outside the map).
int FrameTerrainCell(const Data &data, const std::vector<int> &terrain_of, int terrain, int x, int y,
                     const std::function<int(int, int)> &cell);
// Re-chooses every terrain cell of both layers from its neighbours.
void FrameAll(Data &data);
void ValidateAtlasImage(Data &data, const std::filesystem::path &images_root);
Data LoadFile(const std::filesystem::path &path,
              const std::filesystem::path &images_root);
Data LoadResource(const std::string &map_resource);
void SaveFile(const std::filesystem::path &path, const Data &data);
void SaveResource(const std::string &map_resource, const Data &data);
} // namespace TilemapAsset

#endif
