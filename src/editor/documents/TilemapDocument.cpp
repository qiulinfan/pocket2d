#include "editor/documents/TilemapDocument.h"
#include "shared/resources/ResourcePath.h"
#include <stdexcept>

void TilemapDocument::Clear() {
    asset_.reset();
    saved_.reset();
    path_.clear();
    error_.clear();
    history_.clear();
    history_position_ = 0;
    dirty_cells_.clear();
    ++revision_;
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
        history_.clear();
        history_position_ = 0;
        dirty_cells_.clear();
        error_.clear();
        ++revision_;
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
    dirty_cells_.clear();
    history_.clear();
    history_position_ = 0;
    error_.clear();
    ++revision_;
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

void TilemapDocument::UpdateDirtyCell(const std::string &layer, std::size_t index) {
    const auto key = index + (layer == "solid" ? asset_->ground.cells.size() : 0);
    if (TilemapAsset::GetLayer(*asset_, layer).cells[index] ==
        TilemapAsset::GetLayer(*saved_, layer).cells[index])
        dirty_cells_.erase(key);
    else
        dirty_cells_.insert(key);
}

void TilemapDocument::Apply(const Edit &edit, bool forward) {
    const auto index = static_cast<std::size_t>(edit.y * asset_->width + edit.x);
    TilemapAsset::GetLayer(*asset_, edit.layer).cells[index] = forward ? edit.after : edit.before;
    UpdateDirtyCell(edit.layer, index);
    ++revision_;
}

bool TilemapDocument::SetCell(const std::string &layer, int x, int y, int tile) {
    if (!asset_) return false;
    try {
        auto &cells = TilemapAsset::GetLayer(*asset_, layer).cells;
        if (x < 0 || y < 0 || x >= asset_->width || y >= asset_->height)
            throw std::out_of_range("cell is outside this map");
        if (tile < 0 || tile > asset_->atlas.columns * asset_->atlas.rows)
            throw std::out_of_range("tile ID is outside this atlas");
        const auto index = static_cast<std::size_t>(y * asset_->width + x);
        if (cells[index] == tile) { error_.clear(); return false; }
        Edit edit{layer, x, y, cells[index], tile};
        history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(history_position_), history_.end());
        history_.push_back(edit);
        Apply(edit, true);
        history_position_ = history_.size();
        error_.clear();
        return true;
    } catch (const std::exception &error) {
        error_ = error.what();
        return false;
    }
}

bool TilemapDocument::Undo() {
    if (!CanUndo()) return false;
    Apply(history_[--history_position_], false);
    error_.clear();
    return true;
}

bool TilemapDocument::Redo() {
    if (!CanRedo()) return false;
    Apply(history_[history_position_++], true);
    error_.clear();
    return true;
}
