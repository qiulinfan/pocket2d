#ifndef POCKET2D_TILEMAP_DOCUMENT_H
#define POCKET2D_TILEMAP_DOCUMENT_H

#include "shared/tilemap/TilemapAsset.h"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

// One editor-owned authoring copy. Never reads modified runtime cell state and
// never persists implicitly; the owning overlay decides when to save/discard.
class TilemapDocument {
public:
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
    bool CanUndo() const { return history_position_ > 0; }
    bool CanRedo() const { return history_position_ < history_.size(); }
    bool Undo();
    bool Redo();

private:
    struct Edit { std::string layer; int x, y, before, after; };
    bool Load(const std::filesystem::path &path);
    void Apply(const Edit &edit, bool forward);
    void UpdateDirtyCell(const std::string &layer, std::size_t index);
    std::optional<TilemapAsset::Data> asset_;
    std::optional<TilemapAsset::Data> saved_;
    std::filesystem::path path_;
    std::string error_;
    std::vector<Edit> history_;
    std::size_t history_position_ = 0;
    std::unordered_set<std::size_t> dirty_cells_;
    std::uint64_t revision_ = 0;
};

#endif
