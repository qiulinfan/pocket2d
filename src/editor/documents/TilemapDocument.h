#ifndef POCKET2D_TILEMAP_DOCUMENT_H
#define POCKET2D_TILEMAP_DOCUMENT_H

#include "shared/tilemap/TilemapAsset.h"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

// What the Scene view paints with, chosen in the Inspector. Layers are indices
// as in TilemapAsset (0 ground, 1 solid, then the extra layers).
struct TilemapBrush {
    enum class Mode { Tile, Terrain };
    bool paint = false;   // Scene-view left drags paint instead of selecting
    int layer = 0;
    Mode mode = Mode::Tile;
    int tile = 1;         // 0 erases
    int terrain = 0;      // index into autotile.terrains; -1 clears a terrain
    int size = 1;         // square side in cells
};

// One editor-owned authoring copy. Never reads modified runtime cell state and
// never persists implicitly; the owning overlay decides when to save/discard.
class TilemapDocument {
public:
    struct CellRef { int layer; std::size_t index; };
    void Clear();
    bool Open(const std::filesystem::path &path);
    bool Save();
    bool Revert();
    // Explicitly discard to the last saved/opened in-memory snapshot. Unlike
    // Revert this needs no readable source file and performs no filesystem IO.
    bool DiscardChanges();
    bool HasAsset() const { return asset_.has_value(); }
    bool HasUnsavedChanges() const { return !dirty_cells_.empty(); }
    const std::filesystem::path &GetPath() const { return path_; }
    const TilemapAsset::Data &GetAsset() const;
    std::uint64_t GetRevision() const { return revision_; }
    const std::string &GetLastError() const { return error_; }
    bool SetCell(const std::string &layer, int x, int y, int tile);
    bool SetCell(int layer, int x, int y, int tile);
    // Places a terrain (-1 clears it) and re-chooses the cell and its four
    // neighbours from their neighbourhoods, as the runtime Tilemap:SetTerrain.
    bool SetTerrain(int layer, int x, int y, int terrain);
    // Applies the brush to its square around (x, y); cells off the map are
    // skipped. Returns whether any cell changed.
    bool Paint(const TilemapBrush &brush, int x, int y);
    // Edits between BeginStroke and EndStroke undo as one step (a brush drag);
    // any other edit is its own step.
    void BeginStroke();
    void EndStroke();
    bool InStroke() const { return stroke_open_; }
    bool CanUndo() const { return history_position_ > 0; }
    bool CanRedo() const { return history_position_ < history_.size(); }
    bool Undo();
    bool Redo();
    // Cells changed after `revision`, for previews that patch only those
    // cells; nullopt when the whole asset was replaced since (open, revert,
    // discard) or that history was forgotten.
    std::optional<std::vector<CellRef>> ChangesSince(std::uint64_t revision) const;
    void ForgetChangesThrough(std::uint64_t revision);

private:
    struct Edit { int layer; std::size_t index; int before, after; };
    struct Change { std::uint64_t revision; CellRef cell; };
    bool Load(const std::filesystem::path &path);
    void ResetHistory();
    bool Record(int layer, std::size_t index, int tile);
    void Apply(const Edit &edit, bool forward);
    void ReframeTerrainCell(int layer, int x, int y, const std::vector<int> &terrain_of);
    bool CheckCell(int layer, int x, int y);
    std::optional<TilemapAsset::Data> asset_;
    std::optional<TilemapAsset::Data> saved_;
    std::filesystem::path path_;
    std::string error_;
    std::vector<std::vector<Edit>> history_;
    std::size_t history_position_ = 0;
    bool stroke_open_ = false;
    // Keyed by layer * width * height + cell.
    std::unordered_set<std::size_t> dirty_cells_;
    std::vector<Change> changes_;
    std::uint64_t changes_known_after_ = 0;
    std::uint64_t revision_ = 0;
};

#endif
