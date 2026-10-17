#ifndef EDITOR_CORE_EDITOR_OVERLAY_H
#define EDITOR_CORE_EDITOR_OVERLAY_H

#include "editor/core/EditorConfig.h"
#include "editor/documents/TilemapDocument.h"
#include "scene/Actor.h"
#include "scripting/ComponentManager.h"
#include "shared/config/GameConfig.h"
#include "shared/scene_format/SceneMutation.h"
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

union SDL_Event;
struct SDL_Window;
struct SDL_Renderer;

class Engine;
class SceneDocument;
class Tilemap;
struct EditorConfigData;

struct EditorOverlayResult {
    bool save_scene_requested = false;
    bool scene_changed = false;
    std::vector<SceneFormat::SceneEditCommand> scene_edit_commands;
    bool editor_config_changed = false;
    // Theme, font or Project icon size changed: saved at once, no Apply needed.
    bool editor_preferences_changed = false;
    bool editor_window_settings_changed = false;
    bool editor_config_apply_requested = false;
    bool editor_config_revert_requested = false;
    bool open_scene_requested = false;
    std::filesystem::path requested_scene_path;
    bool open_external_editor_requested = false;
    EditorExternalFileType requested_external_file_type = EditorExternalFileType::Generic;
    std::filesystem::path requested_external_file_path;
    std::filesystem::path requested_external_project_root;
    bool open_project_requested = false;
    std::filesystem::path requested_project_root;
    bool play_mode_start_requested = false;
    bool play_mode_pause_toggle_requested = false;
    bool play_mode_stop_requested = false;
    bool edit_mode_live_preview_toggle_requested = false;
    bool toggle_fullscreen_requested = false;
    bool runtime_view_visible = false;
    bool retry_scene_mirror_requested = false;
    bool dismiss_scene_edit_error_requested = false;
};

class EditorOverlay {
public:
    ~EditorOverlay();

    // Create the ImGui context and bind it to the shared SDL window/renderer.
    // 在共享的 SDL 窗口/渲染器上创建 ImGui 上下文并绑定.
    // `font` is editor.config's "font": "platform" (the OS UI font) or "inter".
    bool Initialize(SDL_Window *window, SDL_Renderer *renderer, std::string_view font = "platform");
    // Apply cached editor settings to the live ImGui style and font scale.
    void ApplyConfig(const EditorConfigData &config, int window_width,
                     int window_height);
    // Queue a short-lived top-right notice for host-level actions.
    void ShowNotice(const std::string &text, double duration_seconds = 4.0);
    void SetSceneEditStatus(const std::string &error, bool pending) { scene_edit_error_ = error; scene_mirror_pending_ = pending; }
    // Update routing context used to decide whether input goes to editor or
    // runtime while the viewport is embedded.
    void SetRuntimeInputRoutingState(bool play_mode_active,
                                     bool play_mode_paused);
    // Reset project-specific UI caches after the logical resources root changes.
    void NotifyProjectChanged();
    // Only after a scene open commits; retain independent editor/map draft state.
    void NotifySceneOpened();
    // Forward one SDL event to ImGui and report whether the editor captured it.
    // 将一个 SDL 事件转发给 ImGui, 并报告编辑器是否捕获了它.
    bool ProcessEvent(const SDL_Event &event);
    // Convert editor-window mouse coordinates into runtime viewport coordinates.
    SDL_Event BuildRuntimeEvent(const SDL_Event &event,
                                const Engine &engine) const;
    // Draw the editor UI for the current scene cache and runtime frame.
    // 绘制当前场景缓存和运行时帧对应的编辑器 UI.
    EditorOverlayResult Render(Engine &engine, SceneDocument &scene_document,
                               EditorConfigData &editor_config,
                               bool editor_config_confirmation_pending,
                               bool play_mode_active, bool play_mode_paused,
                               bool edit_mode_live_preview_enabled,
                               bool scene_editing_enabled,
                               bool scene_save_enabled);
    // Release ImGui backend resources and destroy the ImGui context.
    // 释放 ImGui 后端资源并销毁 ImGui 上下文.
    void Shutdown();
    bool HasUnsavedMapChanges() const { return tilemap_document_.HasUnsavedChanges(); }
    TilemapDocument &GetTilemapDocument() { return tilemap_document_; }
    TilemapBrush &GetTilemapBrush() { return tilemap_brush_; }
    // Explicit owner-thread action shared by the tool button and integration tests.
    const ScriptReloadResult &ExecuteScriptReload(const std::vector<std::string> &types);
    const ScriptReloadResult &GetLastScriptReloadResult() const { return script_reload_result_; }
    bool HasScriptReloadResult() const { return has_script_reload_result_; }
    void ShowScriptReloadWindow(bool visible = true) { show_script_reload_window_ = visible; }
    void SetScriptReloadSelection(const std::vector<std::string> &types) { script_reload_selection_ = types; }


private:
    void RenderScriptReloadWindow();
    bool show_script_reload_window_ = false;
    bool has_script_reload_result_ = false;
    std::vector<std::string> script_reload_selection_;
    ScriptReloadResult script_reload_result_;
    void SyncTilemapPreview(Engine &engine);
    void RenderMapDraftTools(bool play_mode_active);
    TilemapDocument tilemap_document_;
    TilemapBrush tilemap_brush_;
    struct AppliedMapPreview {
        std::uint64_t actor_generation = 0;
        std::uint64_t document_revision = 0;
        Tilemap *instance = nullptr;
        // The instance's revision right after this preview last wrote it.
        std::uint64_t map_revision = 0;
    };
    static constexpr std::uint64_t kRefreshMapRevision = ~std::uint64_t{0};
    bool map_draft_orphaned_ = false;
    std::unordered_map<std::string, AppliedMapPreview> applied_map_previews_;
    // Lazily load the project-visible game/rendering config cache from disk.
    void EnsureProjectConfigLoaded();
    // Queue a short-lived top-right notice for editor actions and failures.
    void ShowTransientNotice(const std::string &text,
                             double duration_seconds = 4.0);
    // Queue a short-lived popup when a file type has no external tool command.
    void ShowExternalEditorMissingConfigNotice(EditorExternalFileType type);
    // Render the project-level config editors that write directly to disk.
    void RenderProjectConfigWindows();
    void RenderNewProjectWindow(EditorConfigData &editor_config,
                                EditorOverlayResult &result);
    void RenderOpenProjectWindow(EditorOverlayResult &result);
    // Render the editor-local settings window that edits overlay/window
    // parameters instead of asking users to edit editor.config by hand.
    void RenderEditorSettingsWindow(Engine &engine, EditorConfigData &editor_config,
                                    EditorOverlayResult &result);
    // Render the centered non-modal confirmation window for pending overlay
    // layout/settings changes.
    void RenderEditorConfigConfirmationWindow( bool editor_config_confirmation_pending, EditorOverlayResult &result);
    // Render non-blocking top-right notifications for editor actions.
    void RenderTransientNotifications();

    // Overlay keeps only editor-session UI state here. Authoritative scene
    // content lives in SceneDocument, and authoritative runtime state lives in
    // Engine / the runtime copy of the scene.
    bool initialized_ = false;
    bool show_metrics_window_ = false;
    bool show_editor_settings_window_ = false;
    bool show_game_config_window_ = false;
    bool show_rendering_config_window_ = false;
    bool show_external_editors_window_ = false;
    bool show_new_project_window_ = false;
    bool show_open_project_window_ = false;
    // Docking layout: checked once per session, rebuilt on View -> Reset Layout.
    bool dock_layout_checked_ = false;
    bool reset_dock_layout_requested_ = false;
    int select_default_tabs_in_ = 0;  // frames left to select the Project tab
    std::string imgui_ini_path_;
    float applied_ui_scale_ = 1.0f;
    std::string applied_theme_;
    float applied_font_size_ = 0.0f;
    bool project_config_loaded_ = false;
    GameConfigData project_config_cache_;
    int selected_actor_index_ = -1;
    Actor::UID selected_runtime_actor_uid_ = Actor::kInvalidUID;
    bool play_mode_active_for_input_ = false;
    bool play_mode_paused_for_input_ = false;
    bool runtime_input_focus_ = false;
    bool viewport_runtime_image_valid_ = false;
    float viewport_runtime_image_min_x_ = 0.0f;
    float viewport_runtime_image_min_y_ = 0.0f;
    float viewport_runtime_image_max_x_ = 0.0f;
    float viewport_runtime_image_max_y_ = 0.0f;
    bool viewport_transport_bar_valid_ = false;
    float viewport_transport_bar_min_x_ = 0.0f;
    float viewport_transport_bar_min_y_ = 0.0f;
    float viewport_transport_bar_max_x_ = 0.0f;
    float viewport_transport_bar_max_y_ = 0.0f;
    std::string scene_edit_error_;
    bool scene_mirror_pending_ = false;
    bool show_transient_notice_ = false;
    double transient_notice_expire_time_ = 0.0;
    std::string transient_notice_text_;
    std::string new_project_name_;
    std::string new_project_location_;
    int new_project_template_index_ = 0;
    std::string new_project_error_;
    std::string open_project_path_;
    std::string open_project_error_;
};

#endif
