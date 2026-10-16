#include "editor/core/EditorSceneSession.h"
#include "engine/core/Engine.h"
#include <utility>
#include <atomic>
#include <limits>

namespace {
std::atomic<std::uint64_t> next_coordinator_epoch{1};
}

void EditorSceneSession::BindDocumentHooks(Engine &engine, bool new_epoch) {
    if (!new_epoch && bound_engine_ == &engine && coordinator_epoch_ != 0) return;
    coordinator_epoch_ = next_coordinator_epoch.fetch_add(1);
    bound_engine_ = &engine;
    authoring_scene_document_.SetPrecommitHook([this](const SceneDocument &candidate, const SceneFormat::SceneEditCommand &command) {
        return CommitDocumentCandidate(true, candidate, command);
    });
    play_scene_document_.SetPrecommitHook([this](const SceneDocument &candidate, const SceneFormat::SceneEditCommand &command) {
        return CommitDocumentCandidate(false, candidate, command);
    });
}

bool EditorSceneSession::CommitDocumentCandidate(bool authoring, const SceneDocument &, const SceneFormat::SceneEditCommand &command) {
    if (authoring && play_mode_active_) {
        // Explicit edits of the inactive authoring draft never touch play state.
        authoring_edited_during_play_ = true;
        command.coordinator_receipt = coordinator_epoch_;
        return true;
    }
    if (!bound_engine_) { last_edit_error_ = "No runtime is bound to this document."; return false; }
    if (runtime_scene_dirty_ && !play_mode_active_) {
        // Existing unmirrored drafts may need more than one structural repair.
        ++mirror_revision_;
        command.coordinator_receipt = coordinator_epoch_;
        return true;
    }
    if (bound_engine_->RequiresSceneReset()) {
        last_edit_error_ = "Runtime requires restart before further synchronized edits.";
        return false;
    }
    if (!bound_engine_->ApplySceneEditCommand(command)) {
        last_edit_error_ = bound_engine_->GetLastSceneEditError();
        if (last_edit_error_.empty()) last_edit_error_ = "Runtime rejected the scene edit.";
        return false;
    }
    last_edit_error_.clear();
    command.coordinator_receipt = coordinator_epoch_;
    return true;
}


// EditorSceneSession public API: scene loading / access
void EditorSceneSession::LoadInitialScene(Engine &engine) {
    BindDocumentHooks(engine, true);
    mirror_revision_ = 0;
    attempted_revision_ = std::numeric_limits<std::uint64_t>::max();
    last_edit_error_.clear();
    authoring_scene_document_ = SceneDocument();
    play_scene_document_ = SceneDocument();
    authoring_scene_document_loaded_ = false;
    play_scene_document_loaded_ = false;
    runtime_scene_dirty_ = false;
    play_mode_active_ = false;
    play_mode_paused_ = false;
    edit_mode_live_preview_enabled_ = false;

    const std::string initial_scene_name = engine.GetConfig().initial_scene_name;
    if (initial_scene_name.empty()) {
        return;
    }

    authoring_scene_document_loaded_ = authoring_scene_document_.LoadFromSceneName(initial_scene_name);
    runtime_scene_dirty_ = authoring_scene_document_loaded_;
    if (runtime_scene_dirty_) ++mirror_revision_;
}

bool EditorSceneSession::OpenSceneFromPath(const std::filesystem::path &scene_path, Engine &engine) {
    if (scene_path.empty()) return false;
    BindDocumentHooks(engine);
    SceneDocument next_document;
    if (!next_document.LoadFromScenePath(scene_path)) return false;
    // Stage both the document and runtime before changing the editor's active
    // authoring/play state. A rejected target must leave existing drafts intact.
    if (!engine.LoadSceneAsset(next_document.GetSceneAsset(), [this] {
            // Engine invokes this only after all candidate validation passes,
            // before any old-world/input/audio mutation. Preserve the existing
            // successful-open autosave policy without saving on a bad target.
            return !authoring_scene_document_loaded_ || !authoring_scene_document_.IsDirty() ||
                   authoring_scene_document_.Save();
        })) { last_edit_error_ = engine.GetLastSceneLoadError(); return false; }
    authoring_scene_document_ = std::move(next_document);
    authoring_scene_document_loaded_ = true;
    play_scene_document_ = SceneDocument();
    play_scene_document_loaded_ = false;
    runtime_scene_dirty_ = false;
    play_mode_active_ = false;
    play_mode_paused_ = false;
    last_edit_error_.clear();
    BindDocumentHooks(engine, true);
    return true;
}

bool EditorSceneSession::HasSceneDocument() const {
    return play_mode_active_ ? play_scene_document_loaded_
                             : authoring_scene_document_loaded_;
}

SceneDocument &EditorSceneSession::GetSceneDocument() {
    return GetActiveSceneDocument();
}

const SceneDocument &EditorSceneSession::GetSceneDocument() const {
    return GetActiveSceneDocument();
}

SceneDocument &EditorSceneSession::GetAuthoringSceneDocument() {
    return authoring_scene_document_;
}

const SceneDocument &EditorSceneSession::GetAuthoringSceneDocument() const {
    return authoring_scene_document_;
}

bool EditorSceneSession::HasPlaySceneDocument() const {
    return play_scene_document_loaded_;
}


// EditorSceneSession public API: save / runtime mirroring
void EditorSceneSession::MarkRuntimeMirrorDirty() {
    if (!authoring_scene_document_loaded_) return;
    if (play_mode_active_) return;
    runtime_scene_dirty_ = true;
    ++mirror_revision_;
}

void EditorSceneSession::RetryRuntimeMirror() {
    if (!authoring_scene_document_loaded_ || play_mode_active_) return;
    runtime_scene_dirty_ = true;
    ++mirror_revision_;
}

void EditorSceneSession::SaveDocumentIfDirty() {
    if (!authoring_scene_document_loaded_) return;
    if (!authoring_scene_document_.IsDirty()) return;
    authoring_scene_document_.Save();
}

/*
Runtime always consumes a copy of the editor cache. This keeps the
viewport live without making editor panels mutate runtime state directly.
*/
void EditorSceneSession::SyncRuntimeMirrorIfDirty(Engine &engine) {
    BindDocumentHooks(engine);
    if (!authoring_scene_document_loaded_ || play_mode_active_ || !runtime_scene_dirty_) return;
    if (attempted_revision_ == mirror_revision_) return;
    attempted_revision_ = mirror_revision_;
    if (!engine.LoadSceneAsset(authoring_scene_document_.GetSceneAsset())) {
        last_edit_error_ = engine.GetLastSceneLoadError();
        if (last_edit_error_.empty()) last_edit_error_ = "Runtime preview is out of date; repair the draft or retry.";
        return;
    }
    runtime_scene_dirty_ = false;
    last_edit_error_.clear();
}

/*
Play mode snapshots the current document once, then runtime is free to
mutate its own world copy until Stop is pressed.
*/
bool EditorSceneSession::EnterPlayMode(Engine &engine) {
    BindDocumentHooks(engine);
    if (!authoring_scene_document_loaded_) return false;
    if (play_mode_active_) return false;

    SceneDocument next_play_document = authoring_scene_document_;
    if (!engine.LoadSceneAsset(next_play_document.GetSceneAsset())) { last_edit_error_ = engine.GetLastSceneLoadError(); return false; }
    play_scene_document_ = std::move(next_play_document);
    play_scene_document_loaded_ = true;
    play_mode_active_ = true;
    play_mode_paused_ = false;
    authoring_edited_during_play_ = false;
    runtime_scene_dirty_ = false;
    last_edit_error_.clear();
    BindDocumentHooks(engine, true);
    return true;
}

// The play World is discarded by its owner; the edit World never left the
// authoring state, so it is reloaded only if the draft changed during play.
bool EditorSceneSession::EndPlaySession(Engine &edit_engine) {
    if (!play_mode_active_) return false;
    play_mode_active_ = false;
    play_mode_paused_ = false;
    play_scene_document_ = SceneDocument();
    play_scene_document_loaded_ = false;
    last_edit_error_.clear();
    if (authoring_edited_during_play_ && authoring_scene_document_loaded_) {
        runtime_scene_dirty_ = true;
        ++mirror_revision_;
    }
    authoring_edited_during_play_ = false;
    BindDocumentHooks(edit_engine, true);
    return true;
}

// Leaving play mode restores runtime from the authoritative editor cache.
bool EditorSceneSession::ExitPlayMode(Engine &engine) {
    if (!authoring_scene_document_loaded_) return false;
    if (!play_mode_active_) return false;

    if (!engine.LoadSceneAsset(authoring_scene_document_.GetSceneAsset())) { last_edit_error_ = engine.GetLastSceneLoadError(); return false; }
    play_mode_active_ = false;
    play_mode_paused_ = false;
    play_scene_document_ = SceneDocument();
    play_scene_document_loaded_ = false;
    runtime_scene_dirty_ = false;
    last_edit_error_.clear();
    BindDocumentHooks(engine, true);
    return true;
}

bool EditorSceneSession::TogglePlayPause() {
    if (!authoring_scene_document_loaded_) return false;
    if (!play_mode_active_) return false;

    play_mode_paused_ = !play_mode_paused_;
    return true;
}

bool EditorSceneSession::HandleSceneEditCommand(const SceneFormat::SceneEditCommand &command, Engine &engine) {
    BindDocumentHooks(engine);
    if (command.empty() || !HasSceneDocument()) return false;
    if (command.coordinator_receipt == coordinator_epoch_) return true;
    if (command.coordinator_receipt != 0) {
        last_edit_error_ = "Command receipt belongs to a previous document or mode.";
        return false;
    }
    // An unmarked command is a new request, not a guess about current effects.
    last_edit_error_.clear();
    if (!GetActiveSceneDocument().ApplyEditCommand(command)) {
        if (last_edit_error_.empty()) last_edit_error_ = "Document rejected the scene edit.";
        return false;
    }
    return true;
}

bool EditorSceneSession::HandleSceneEditCommands(const std::vector<SceneFormat::SceneEditCommand> &commands, Engine &engine) {
    BindDocumentHooks(engine);
    SceneFormat::SceneEditCommand pending;
    for (const auto &command : commands) {
        if (command.coordinator_receipt == coordinator_epoch_) continue;
        if (command.coordinator_receipt != 0) {
            last_edit_error_ = "Command receipt belongs to a previous document or mode.";
            return false;
        }
        pending.mutations.insert(pending.mutations.end(), command.mutations.begin(), command.mutations.end());
    }
    if (pending.empty()) return true;
    if (!HandleSceneEditCommand(pending, engine)) return false;
    for (const auto &command : commands) command.coordinator_receipt = coordinator_epoch_;
    return true;
}

/*
Called when the editor is closing: persist only editor document cache.
Runtime play-state mutations are discarded and never serialized.
*/
void EditorSceneSession::PersistDocumentOnEditorShutdown() {
    if (!authoring_scene_document_loaded_) return;
    play_mode_active_ = false;
    play_mode_paused_ = false;
    play_scene_document_ = SceneDocument();
    play_scene_document_loaded_ = false;
    runtime_scene_dirty_ = false;
    SaveDocumentIfDirty();
}

bool EditorSceneSession::IsPlayModeActive() const {
    return play_mode_active_;
}

bool EditorSceneSession::IsPlayModePaused() const {
    return play_mode_active_ && play_mode_paused_;
}

bool EditorSceneSession::IsEditModeLivePreviewEnabled() const {
    return edit_mode_live_preview_enabled_;
}

bool EditorSceneSession::ToggleEditModeLivePreview() {
    if (play_mode_active_) return false;
    edit_mode_live_preview_enabled_ = !edit_mode_live_preview_enabled_;
    MarkRuntimeMirrorDirty();
    return true;
}

bool EditorSceneSession::IsSceneEditingEnabled() const {
    return HasSceneDocument();
}

bool EditorSceneSession::IsSceneSaveEnabled() const {
    return authoring_scene_document_loaded_ && !play_mode_active_;
}

SceneDocument &EditorSceneSession::GetActiveSceneDocument() {
    if (play_mode_active_ && play_scene_document_loaded_) {
        return play_scene_document_;
    }
    return authoring_scene_document_;
}

const SceneDocument &EditorSceneSession::GetActiveSceneDocument() const {
    if (play_mode_active_ && play_scene_document_loaded_) {
        return play_scene_document_;
    }
    return authoring_scene_document_;
}
