#include "editor/documents/TilemapDocument.h"
#include "shared/resources/ResourcePath.h"
#include <algorithm>
#include <stdexcept>

void TilemapDocument::Clear() {
    asset_.reset();
    saved_.reset();
    path_.clear();
    error_.clear();
    ++revision_;
    ResetHistory();
}

// After the whole asset was replaced: no undo across it, and previews must
// copy it whole rather than patch cells.
void TilemapDocument::ResetHistory() {
    history_.clear();
    history_position_ = 0;
    stroke_open_ = false;
    dirty_cells_.clear();
    changes_.clear();
    changes_known_after_ = revision_;
}

const TilemapAsset::Data &TilemapDocument::GetAsset() const {
    if (!asset_) throw std::logic_error("no tilemap document is open");
    return *asset_;
}

bool TilemapDocument::Load(const std::filesystem::path &path) {
    try {
        auto candidate = TilemapAsset::LoadFile(
            path, ResourcePath::ResourceSubdirectory("images"));
        // Parse before changing the current authoring copy: failed opens keep it.
        auto saved_candidate = candidate;
        asset_ = std::move(candidate);
        saved_ = std::move(saved_candidate);
        path_ = std::filesystem::absolute(path).lexically_normal();
        error_.clear();
        ++revision_;
        ResetHistory();
        return true;
    } catch (const std::exception &error) {
        error_ = error.what();
        return false;
    }
}

bool TilemapDocument::Open(const std::filesystem::path &path) {
    const auto requested = std::filesystem::absolute(path).lexically_normal();
    if (asset_ && path_ == requested) return true;
    if (HasUnsavedChanges()) {
        error_ = "Save or discard the current map edits before opening another map.";
        return false;
    }
    return Load(requested);
}

bool TilemapDocument::Revert() {
    if (!asset_) return false;
    return Load(path_);
}

bool TilemapDocument::DiscardChanges() {
    if (!asset_ || !saved_) return false;
    *asset_ = *saved_;
    error_.clear();
    ++revision_;
    ResetHistory();
    return true;
}

bool TilemapDocument::Save() {
    if (!asset_) return false;
    try {
        TilemapAsset::SaveFile(path_, *asset_);
        saved_ = *asset_;
        dirty_cells_.clear();
        error_.clear();
        return true;
    } catch (const std::exception &error) {
        error_ = error.what();
        return false;
    }
}

void TilemapDocument::Apply(const Edit &edit, bool forward) {
    TilemapAsset::LayerAtIndex(*asset_, edit.layer).cells[edit.index] = forward ? edit.after : edit.before;
    const auto key = static_cast<std::size_t>(edit.layer) * asset_->width * asset_->height + edit.index;
    if (TilemapAsset::LayerAtIndex(*asset_, edit.layer).cells[edit.index] ==
        TilemapAsset::LayerAtIndex(*saved_, edit.layer).cells[edit.index])
        dirty_cells_.erase(key);
    else
        dirty_cells_.insert(key);
    ++revision_;
    changes_.push_back({revision_, {edit.layer, edit.index}});
}

// Writes one cell into the open stroke, or as its own undo step.
bool TilemapDocument::Record(int layer, std::size_t index, int tile) {
    const int before = TilemapAsset::LayerAtIndex(*asset_, layer).cells[index];
    if (before == tile) return false;
    if (!stroke_open_) {
        history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(history_position_), history_.end());
        history_.emplace_back();
        history_position_ = history_.size();
    }
    const Edit edit{layer, index, before, tile};
    history_.back().push_back(edit);
    Apply(edit, true);
    return true;
}

bool TilemapDocument::CheckCell(int layer, int x, int y) {
    if (!asset_) return false;
    if (layer < 0 || layer >= TilemapAsset::LayerCount(*asset_)) {
        error_ = "layer is not in this map";
        return false;
    }
    if (x < 0 || y < 0 || x >= asset_->width || y >= asset_->height) {
        error_ = "cell is outside this map";
        return false;
    }
    return true;
}

bool TilemapDocument::SetCell(const std::string &layer, int x, int y, int tile) {
    if (!asset_) return false;
    const int index = TilemapAsset::FindLayerIndex(*asset_, layer);
    if (index < 0) { error_ = "tilemap: unknown layer '" + layer + "'"; return false; }
    return SetCell(index, x, y, tile);
}

bool TilemapDocument::SetCell(int layer, int x, int y, int tile) {
    if (!CheckCell(layer, x, y)) return false;
    if (tile < 0 || tile > asset_->atlas.columns * asset_->atlas.rows) {
        error_ = "tile ID is outside this atlas";
        return false;
    }
    error_.clear();
    return Record(layer, static_cast<std::size_t>(y) * asset_->width + x, tile);
}

// Outside the map counts as connected, as in TilemapAsset::FrameAll.
void TilemapDocument::ReframeTerrainCell(int layer, int x, int y, const std::vector<int> &terrain_of) {
    if (x < 0 || y < 0 || x >= asset_->width || y >= asset_->height) return;
    const auto &cells = TilemapAsset::LayerAtIndex(*asset_, layer).cells;
    const auto index = static_cast<std::size_t>(y) * asset_->width + x;
    const int id = cells[index];
    if (id <= 0 || static_cast<std::size_t>(id) >= terrain_of.size() || terrain_of[id] < 0) return;
    Record(layer, index, TilemapAsset::FrameTerrainCell(*asset_, terrain_of, terrain_of[id], x, y, [&](int nx, int ny) {
        if (nx < 0 || ny < 0 || nx >= asset_->width || ny >= asset_->height) return 1;
        return cells[static_cast<std::size_t>(ny) * asset_->width + nx];
    }));
}

bool TilemapDocument::SetTerrain(int layer, int x, int y, int terrain) {
    if (!CheckCell(layer, x, y)) return false;
    if (terrain < -1 || terrain >= static_cast<int>(asset_->autotile.terrains.size())) {
        error_ = "terrain is not in this map";
        return false;
    }
    error_.clear();
    const bool own_stroke = !stroke_open_;
    if (own_stroke) BeginStroke();
    const auto terrain_of = TilemapAsset::TerrainIndex(*asset_);
    const auto &cells = TilemapAsset::LayerAtIndex(*asset_, layer).cells;
    int tile = 0;
    if (terrain >= 0)
        tile = TilemapAsset::FrameTerrainCell(*asset_, terrain_of, terrain, x, y, [&](int nx, int ny) {
            if (nx < 0 || ny < 0 || nx >= asset_->width || ny >= asset_->height) return 1;
            return cells[static_cast<std::size_t>(ny) * asset_->width + nx];
        });
    const bool changed = Record(layer, static_cast<std::size_t>(y) * asset_->width + x, tile);
    if (changed) {
        ReframeTerrainCell(layer, x, y - 1, terrain_of);
        ReframeTerrainCell(layer, x + 1, y, terrain_of);
        ReframeTerrainCell(layer, x, y + 1, terrain_of);
        ReframeTerrainCell(layer, x - 1, y, terrain_of);
    }
    if (own_stroke) EndStroke();
    return changed;
}

bool TilemapDocument::Paint(const TilemapBrush &brush, int x, int y) {
    if (!asset_) return false;
    const int size = std::clamp(brush.size, 1, 64);
    const bool own_stroke = !stroke_open_;
    if (own_stroke) BeginStroke();
    bool changed = false;
    error_.clear();
    // An invalid brush reports its error once instead of per cell.
    for (int cy = y - (size - 1) / 2; cy <= y + size / 2 && error_.empty(); ++cy)
        for (int cx = x - (size - 1) / 2; cx <= x + size / 2 && error_.empty(); ++cx) {
            if (cx < 0 || cy < 0 || cx >= asset_->width || cy >= asset_->height) continue;
            changed |= brush.mode == TilemapBrush::Mode::Terrain ? SetTerrain(brush.layer, cx, cy, brush.terrain)
                                                                 : SetCell(brush.layer, cx, cy, brush.tile);
        }
    if (own_stroke) EndStroke();
    return changed;
}

void TilemapDocument::BeginStroke() {
    if (!asset_ || stroke_open_) return;
    history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(history_position_), history_.end());
    history_.emplace_back();
    history_position_ = history_.size();
    stroke_open_ = true;
}

void TilemapDocument::EndStroke() {
    if (!stroke_open_) return;
    stroke_open_ = false;
    if (history_.back().empty()) {
        history_.pop_back();
        history_position_ = history_.size();
    }
}

bool TilemapDocument::Undo() {
    EndStroke();
    if (!CanUndo()) return false;
    const auto &step = history_[--history_position_];
    for (auto edit = step.rbegin(); edit != step.rend(); ++edit) Apply(*edit, false);
    error_.clear();
    return true;
}

bool TilemapDocument::Redo() {
    EndStroke();
    if (!CanRedo()) return false;
    for (const auto &edit : history_[history_position_++]) Apply(edit, true);
    error_.clear();
    return true;
}

std::optional<std::vector<TilemapDocument::CellRef>> TilemapDocument::ChangesSince(std::uint64_t revision) const {
    if (revision < changes_known_after_ || revision > revision_) return std::nullopt;
    const auto first = std::upper_bound(changes_.begin(), changes_.end(), revision,
        [](std::uint64_t value, const Change &change) { return value < change.revision; });
    std::vector<CellRef> cells;
    cells.reserve(static_cast<std::size_t>(changes_.end() - first));
    for (auto change = first; change != changes_.end(); ++change) cells.push_back(change->cell);
    return cells;
}

void TilemapDocument::ForgetChangesThrough(std::uint64_t revision) {
    revision = std::min(revision, revision_);
    if (revision <= changes_known_after_) return;
    const auto first = std::upper_bound(changes_.begin(), changes_.end(), revision,
        [](std::uint64_t value, const Change &change) { return value < change.revision; });
    changes_.erase(changes_.begin(), first);
    changes_known_after_ = revision;
}
