#ifndef EDITOR_APP_H
#define EDITOR_APP_H

#include "editor/core/EditorConfig.h"
#include "editor/core/EditorOverlay.h"
#include "editor/core/EditorSceneSession.h"
#include "engine/core/Engine.h"
#include <filesystem>
#include <memory>

struct SDL_WindowEvent;

class EditorApp {
public:
    void Run();

private:
    // Persist the currently visible editor overlay/window settings as the new
    // confirmed baseline on disk.
    void PersistConfirmedEditorConfig();
    // Revert any unconfirmed overlay/window changes back to the last
    // confirmed baseline.
    void RevertUnconfirmedEditorConfigChanges();
    // Apply the cached editor host config to the live SDL window.
    void ApplyEditorWindowSettings();
    // Toggle the shared SDL window between windowed and fullscreen desktop.
    void SetEditorFullscreen(bool enabled);
    // Return whether the shared SDL window is currently fullscreen.
    bool IsEditorWindowFullscreen() const;
    // Return whether the shared SDL window is currently maximized.
    bool IsEditorWindowMaximized() const;
    // Pull the live SDL window geometry back into editor config.
    // Only windowed geometry is persisted; fullscreen/maximized are treated as
    // transient presentation states over the same stored windowed layout.
    void SyncWindowStateFromLiveWindow();
    // React to SDL window lifecycle changes that can affect layout persistence.
    void HandleWindowEvent(const SDL_WindowEvent &window_event);
    bool BootstrapEngineForCurrentProject();
    bool SwitchProject(const std::filesystem::path &project_root);
    // Play runs a snapshot of the authoring scene in its own World and Engine,
    // embedded in the editor window. Stop discards them; the edit World is
    // never reloaded for play.
    bool StartPlaySession();
    void StopPlaySession();
    // The Engine whose World is simulated, rendered and inspected this frame.
    Engine &ActiveEngine() { return play_engine_ ? *play_engine_ : *engine_; }
    EditorConfigData confirmed_editor_config_;
    bool confirmed_editor_config_persist_dirty_ = false;
    EditorConfigData editor_config_;
    bool editor_config_dirty_ = false;
    std::unique_ptr<Engine> engine_;
    // Declared before play_engine_ so the Engine is destroyed first.
    std::unique_ptr<World> play_world_;
    std::unique_ptr<Engine> play_engine_;
    EditorOverlay overlay_;
    EditorSceneSession scene_session_;
    bool runtime_view_visible_last_frame_ = true;
};

#endif
