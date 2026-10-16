#include "shared/tilemap/TilemapAsset.h"
#include "shared/resources/ResourcePath.h"
#include "SDL3_image/SDL_image.h"
#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace {
using Json = rapidjson::Value;
void Require(bool valid, const std::string &message) {
    if (!valid) throw std::runtime_error("tilemap: " + message);
}
void ExactKeys(const Json &object, std::initializer_list<const char *> keys,
               const std::string &where, std::initializer_list<const char *> optional = {}) {
    Require(object.IsObject(), where + " must be an object");
    std::set<std::string> allowed(optional.begin(), optional.end());
    std::set<std::string> encountered;
    for (const char *key : keys) {
        allowed.emplace(key);
        Require(object.HasMember(key), where + "." + key + " is required");
    }
    for (auto member = object.MemberBegin(); member != object.MemberEnd(); ++member) {
        Require(allowed.count(member->name.GetString()) != 0,
                where + ": unknown field " + member->name.GetString());
        Require(encountered.emplace(member->name.GetString()).second,
                where + ": duplicate field " + member->name.GetString());
    }
}
int Integer(const Json &object, const char *key, const std::string &where) {
    Require(object[key].IsInt(), where + "." + key + " must be an integer");
    return object[key].GetInt();
}
void RequireRelative(const std::filesystem::path &path, const std::string &label) {
    Require(!path.empty() && !path.is_absolute() && !path.has_root_name(),
            label + " must be an explicit relative path");
    Require(path.native().find(std::filesystem::path::value_type{}) == std::filesystem::path::string_type::npos,
            label + " contains a null byte");
    for (const auto &part : path)
        Require(part != "..", label + " must not escape its resource directory");
}
std::filesystem::path ResolveWithin(const std::filesystem::path &root,
                                    const std::filesystem::path &relative,
                                    const std::string &label) {
    RequireRelative(relative, label);
    const auto normalized_root = std::filesystem::weakly_canonical(std::filesystem::absolute(root));
    const auto resolved = std::filesystem::weakly_canonical(normalized_root / relative);
    Require(ResourcePath::IsPathWithinDirectory(resolved, normalized_root),
            label + " resolves outside its resource directory");
    return resolved;
}
void RequireMapExtension(const std::filesystem::path &path) {
    Require(path.filename().string().ends_with(".tilemap.json"),
            "map resource must end in .tilemap.json");
}
void ValidatePixels(const TilemapAsset::Data &data, const std::filesystem::path &path) {
    Require(std::filesystem::is_regular_file(path), "atlas image missing: " + path.string());
    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> image(
        IMG_Load(path.string().c_str()), SDL_DestroySurface);
    Require(image != nullptr, "cannot decode atlas " + path.string() + ": " + SDL_GetError());
    const auto width = static_cast<std::int64_t>(data.atlas.columns) * data.atlas.tile_width_px;
    const auto height = static_cast<std::int64_t>(data.atlas.rows) * data.atlas.tile_height_px;
    Require(image->w == width && image->h == height,
            "atlas pixel dimensions must exactly match columns/rows and tile dimensions: " + path.string());
}
TilemapAsset::Layer ReadLayer(const Json &value, std::size_t cell_count, const char *name) {
    ExactKeys(value, {"sorting_order", "cells"}, name);
    TilemapAsset::Layer layer;
    layer.sorting_order = Integer(value, "sorting_order", name);
    Require(value["cells"].IsArray(), std::string(name) + ".cells must be an array");
    Require(value["cells"].Size() == cell_count, std::string(name) + ".cells length does not match width*height");
    layer.cells.reserve(cell_count);
    for (const auto &cell : value["cells"].GetArray()) {
        Require(cell.IsInt(), std::string(name) + ".cells requires integer tile IDs");
        layer.cells.push_back(cell.GetInt());
    }
    return layer;
}
TilemapAsset::Autotile ReadAutotile(const Json &value) {
    ExactKeys(value, {"terrains"}, "autotile", {"frame_on_load"});
    TilemapAsset::Autotile autotile;
    if (value.HasMember("frame_on_load")) {
        Require(value["frame_on_load"].IsBool(), "autotile.frame_on_load must be a boolean");
        autotile.frame_on_load = value["frame_on_load"].GetBool();
    }
    Require(value["terrains"].IsArray(), "autotile.terrains must be an array");
    for (const auto &entry : value["terrains"].GetArray()) {
        ExactKeys(entry, {"name", "frames"}, "terrain", {"connect"});
        TilemapAsset::Terrain terrain;
        Require(entry["name"].IsString(), "terrain.name must be a string");
        terrain.name.assign(entry["name"].GetString(), entry["name"].GetStringLength());
        if (entry.HasMember("connect")) {
            Require(entry["connect"].IsString(), "terrain.connect must be \"any\" or \"same\"");
            const std::string connect = entry["connect"].GetString();
            Require(connect == "any" || connect == "same", "terrain.connect must be \"any\" or \"same\"");
            terrain.connect_same = connect == "same";
        }
        Require(entry["frames"].IsArray() && entry["frames"].Size() == 16,
                "terrain.frames must list 16 masks");
        for (rapidjson::SizeType mask = 0; mask < 16; ++mask) {
            const auto &ids = entry["frames"][mask];
            Require(ids.IsArray(), "terrain.frames entries must be arrays of tile IDs");
            for (const auto &id : ids.GetArray()) {
                Require(id.IsInt(), "terrain.frames requires integer tile IDs");
                terrain.frames[mask].push_back(id.GetInt());
            }
        }
        autotile.terrains.push_back(std::move(terrain));
    }
    return autotile;
}
} // namespace

namespace TilemapAsset {
std::vector<int> TerrainIndex(const Data &data) {
    const auto tile_count = static_cast<std::size_t>(data.atlas.columns) * data.atlas.rows;
    std::vector<int> terrain_of(tile_count + 1, -1);
    for (std::size_t index = 0; index < data.autotile.terrains.size(); ++index)
        for (const auto &ids : data.autotile.terrains[index].frames)
            for (int id : ids)
                if (id >= 1 && static_cast<std::size_t>(id) <= tile_count) terrain_of[id] = static_cast<int>(index);
    return terrain_of;
}
int FrameTerrainCell(const Data &data, const std::vector<int> &terrain_of, int terrain, int x, int y,
                     const std::function<int(int, int)> &cell) {
    const auto &rules = data.autotile.terrains.at(static_cast<std::size_t>(terrain));
    const auto connects = [&](int nx, int ny) {
        const int id = cell(nx, ny);
        if (id <= 0) return false;
        if (!rules.connect_same) return true;
        return static_cast<std::size_t>(id) < terrain_of.size() && terrain_of[id] == terrain;
    };
    const unsigned mask = (connects(x, y - 1) ? 1u : 0u) | (connects(x + 1, y) ? 2u : 0u) |
                          (connects(x, y + 1) ? 4u : 0u) | (connects(x - 1, y) ? 8u : 0u);
    const auto &choices = rules.frames[mask];
    // A stable per-position variant, so re-framing a cell keeps its look.
    const std::uint32_t hash = static_cast<std::uint32_t>(x) * 73856093u ^ static_cast<std::uint32_t>(y) * 19349663u;
    return choices[hash % choices.size()];
}
void FrameAll(Data &data) {
    if (data.autotile.terrains.empty()) return;
    const auto terrain_of = TerrainIndex(data);
    for (const char *name : {"ground", "solid"}) {
        auto &layer = GetLayer(data, name);
        const auto original = layer.cells;
        // Outside the map counts as connected, so map edges show no border.
        const auto cell = [&](int x, int y) {
            if (x < 0 || y < 0 || x >= data.width || y >= data.height) return 1;
            return original[static_cast<std::size_t>(y) * data.width + x];
        };
        for (int y = 0; y < data.height; ++y)
            for (int x = 0; x < data.width; ++x) {
                auto &id = layer.cells[static_cast<std::size_t>(y) * data.width + x];
                if (id > 0 && terrain_of[id] >= 0) id = FrameTerrainCell(data, terrain_of, terrain_of[id], x, y, cell);
            }
    }
}
Layer &GetLayer(Data &data, const std::string &layer) {
    if (layer == "ground") return data.ground;
    if (layer == "solid") return data.solid;
    throw std::runtime_error("tilemap: unknown layer '" + layer + "'");
}
const Layer &GetLayer(const Data &data, const std::string &layer) {
    if (layer == "ground") return data.ground;
    if (layer == "solid") return data.solid;
    throw std::runtime_error("tilemap: unknown layer '" + layer + "'");
}
void ValidateData(const Data &data) {
    Require(data.version == 1, "unsupported version (expected 1)");
    Require(data.width >= 1 && data.width <= 512 && data.height >= 1 && data.height <= 512,
            "width and height must be integers in [1,512]");
    Require(std::isfinite(data.cell_size) && data.cell_size > 0, "cell_size must be finite and positive");
    RequireRelative(data.atlas.image, "atlas.image");
    Require(data.atlas.columns > 0 && data.atlas.rows > 0 &&
            data.atlas.tile_width_px > 0 && data.atlas.tile_height_px > 0,
            "atlas grid and tile pixel dimensions must be positive integers");
    const auto tile_count = static_cast<std::int64_t>(data.atlas.columns) * data.atlas.rows;
    const auto pixel_width = static_cast<std::int64_t>(data.atlas.columns) * data.atlas.tile_width_px;
    const auto pixel_height = static_cast<std::int64_t>(data.atlas.rows) * data.atlas.tile_height_px;
    Require(tile_count <= std::numeric_limits<int>::max() &&
            pixel_width <= std::numeric_limits<int>::max() && pixel_height <= std::numeric_limits<int>::max(),
            "atlas dimensions overflow supported integer range");
    std::set<std::string> names;
    std::vector<int> owner(static_cast<std::size_t>(tile_count) + 1, -1);
    for (std::size_t index = 0; index < data.autotile.terrains.size(); ++index) {
        const auto &terrain = data.autotile.terrains[index];
        Require(!terrain.name.empty() && names.insert(terrain.name).second, "terrain names must be unique and nonempty");
        for (const auto &ids : terrain.frames) {
            Require(!ids.empty(), "terrain " + terrain.name + ": every mask needs at least one tile ID");
            for (int id : ids) {
                Require(id >= 1 && id <= tile_count, "terrain " + terrain.name + ": tile ID outside atlas range");
                Require(owner[id] == -1 || owner[id] == static_cast<int>(index),
                        "terrain " + terrain.name + ": tile ID " + std::to_string(id) + " belongs to another terrain");
                owner[id] = static_cast<int>(index);
            }
        }
    }
    const auto count = static_cast<std::size_t>(data.width) * data.height;
    for (const char *name : {"ground", "solid"}) {
        const auto &layer = GetLayer(data, name);
        Require(layer.cells.size() == count, std::string(name) + ".cells length does not match width*height");
        for (int tile : layer.cells)
            Require(tile >= 0 && tile <= tile_count, std::string(name) + ": tile ID outside atlas range");
    }
}
void ValidateAtlasImage(Data &data, const std::filesystem::path &images_root) {
    ValidateData(data);
    const auto path = ResolveWithin(images_root, data.atlas.image, "atlas.image");
    ValidatePixels(data, path);
    data.resolved_image_path = path;
}
Data LoadFile(const std::filesystem::path &path, const std::filesystem::path &images_root) {
    RequireMapExtension(path);
    std::ifstream input(path, std::ios::binary);
    Require(input.is_open(), "cannot open map " + path.string());
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    Require(!input.bad(), "cannot read map " + path.string());
    rapidjson::Document json;
    json.Parse(text.data(), text.size());
    Require(!json.HasParseError(), "invalid JSON in " + path.string() + ": " +
            rapidjson::GetParseError_En(json.GetParseError()));
    ExactKeys(json, {"version", "width", "height", "cell_size", "atlas", "layers"}, "map", {"autotile"});
    Data data;
    data.version = Integer(json, "version", "map");
    Require(data.version == 1, "unsupported version (expected 1)");
    data.width = Integer(json, "width", "map");
    data.height = Integer(json, "height", "map");
    Require(data.width >= 1 && data.width <= 512 && data.height >= 1 && data.height <= 512,
            "width and height must be in [1,512]");
    Require(json["cell_size"].IsNumber(), "cell_size must be a number");
    data.cell_size = static_cast<float>(json["cell_size"].GetDouble());
    const auto &atlas = json["atlas"];
    ExactKeys(atlas, {"image", "columns", "rows", "tile_width_px", "tile_height_px"}, "atlas");
    Require(atlas["image"].IsString(), "atlas.image must be a string");
    data.atlas.image.assign(atlas["image"].GetString(), atlas["image"].GetStringLength());
    Require(data.atlas.image.find('\0') == std::string::npos, "atlas.image contains a null byte");
    data.atlas.columns = Integer(atlas, "columns", "atlas");
    data.atlas.rows = Integer(atlas, "rows", "atlas");
    data.atlas.tile_width_px = Integer(atlas, "tile_width_px", "atlas");
    data.atlas.tile_height_px = Integer(atlas, "tile_height_px", "atlas");
    ExactKeys(json["layers"], {"ground", "solid"}, "layers");
    const auto count = static_cast<std::size_t>(data.width) * data.height;
    data.ground = ReadLayer(json["layers"]["ground"], count, "ground");
    data.solid = ReadLayer(json["layers"]["solid"], count, "solid");
    if (json.HasMember("autotile")) data.autotile = ReadAutotile(json["autotile"]);
    ValidateAtlasImage(data, images_root);
    if (data.autotile.frame_on_load) FrameAll(data);
    return data;
}
Data LoadResource(const std::string &map_resource) {
    Require(map_resource.find('\0') == std::string::npos, "map_resource contains a null byte");
    const auto path = ResolveWithin(ResourcePath::ResourceSubdirectory("maps"), map_resource, "map_resource");
    return LoadFile(path, ResourcePath::ResourceSubdirectory("images"));
}
void SaveFile(const std::filesystem::path &path, const Data &data) {
    RequireMapExtension(path);
    ValidateData(data);
    if (!data.resolved_image_path.empty()) ValidatePixels(data, data.resolved_image_path);
    else {
        auto checked = data;
        ValidateAtlasImage(checked, ResourcePath::ResourceSubdirectory("images"));
    }
    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("version"); writer.Int(data.version);
    writer.Key("width"); writer.Int(data.width);
    writer.Key("height"); writer.Int(data.height);
    writer.Key("cell_size"); writer.Double(data.cell_size);
    writer.Key("atlas"); writer.StartObject();
    writer.Key("image"); writer.String(data.atlas.image.c_str());
    writer.Key("columns"); writer.Int(data.atlas.columns);
    writer.Key("rows"); writer.Int(data.atlas.rows);
    writer.Key("tile_width_px"); writer.Int(data.atlas.tile_width_px);
    writer.Key("tile_height_px"); writer.Int(data.atlas.tile_height_px);
    writer.EndObject();
    writer.Key("layers"); writer.StartObject();
    for (const char *name : {"ground", "solid"}) {
        const auto &layer = GetLayer(data, name);
        writer.Key(name); writer.StartObject();
        writer.Key("sorting_order"); writer.Int(layer.sorting_order);
        writer.Key("cells"); writer.StartArray();
        for (int tile : layer.cells) writer.Int(tile);
        writer.EndArray(); writer.EndObject();
    }
    writer.EndObject();
    if (!data.autotile.terrains.empty() || data.autotile.frame_on_load) {
        writer.Key("autotile"); writer.StartObject();
        writer.Key("frame_on_load"); writer.Bool(data.autotile.frame_on_load);
        writer.Key("terrains"); writer.StartArray();
        for (const auto &terrain : data.autotile.terrains) {
            writer.StartObject();
            writer.Key("name"); writer.String(terrain.name.c_str());
            writer.Key("connect"); writer.String(terrain.connect_same ? "same" : "any");
            writer.Key("frames"); writer.StartArray();
            for (const auto &ids : terrain.frames) {
                writer.StartArray();
                for (int id : ids) writer.Int(id);
                writer.EndArray();
            }
            writer.EndArray(); writer.EndObject();
        }
        writer.EndArray(); writer.EndObject();
    }
    writer.EndObject();
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    static std::atomic<std::uint64_t> counter{0};
    const auto temporary = path.string() + ".tmp-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(counter++);
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        Require(output.is_open(), "cannot create temporary map file");
        output.write(buffer.GetString(), static_cast<std::streamsize>(buffer.GetSize()));
        output.flush();
        Require(output.good(), "cannot write temporary map file");
        output.close();
        Require(!output.fail(), "cannot close temporary map file");
#if defined(_WIN32)
        Require(MoveFileExW(std::filesystem::path(temporary).c_str(), path.c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), "cannot replace map file");
#else
        std::filesystem::rename(temporary, path);
#endif
    } catch (...) {
        std::error_code error;
        std::filesystem::remove(temporary, error);
        throw;
    }
}
void SaveResource(const std::string &map_resource, const Data &data) {
    const auto path = ResolveWithin(ResourcePath::ResourceSubdirectory("maps"), map_resource, "map_resource");
    SaveFile(path, data);
}
} // namespace TilemapAsset
