#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "editor/core/EditorOverlay.h"
#include "scene/Tilemap.h"
#include "scripting/ComponentManager.h"
#include "editor/core/EditorConfig.h"
#include "editor/core/EditorTheme.h"
#include "editor/core/ProjectTemplates.h"
#include "editor/documents/SceneDocument.h"
#include "editor/panels/HierarchyPanel.h"
#include "editor/panels/InspectorPanel.h"
#include "editor/panels/ProjectPanel.h"
#include "editor/panels/StatusPanel.h"
#include "editor/panels/ViewportPanel.h"
#include "editor/panels/SceneViewPanel.h"
#include "engine/core/Engine.h"
#include "shared/resources/ResourcePath.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "SDL3/SDL.h"
#if defined(_WIN32)
#include <objbase.h>
#include <shobjidl.h>
#include <windows.h>
#endif
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr float kEditorFontPixelSize = 16.0f;

/*
These helpers decide whether an SDL event should stay in editor land 
or should be forwarded into runtime input processing.
(以下 helpers: 一个 SDL event 是否应该被转发到 runtime)
*/ 
bool ShouldCaptureMouseEvent(Uint32 event_type) {
    switch (event_type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
        return true;
    default:
        return false;
    }
}
bool ShouldCaptureKeyboardEvent(Uint32 event_type) {
    switch (event_type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_TEXT_INPUT:
        return true;
    default:
        return false;
    }
}

bool IsEditorPlaybackHotkeyEvent(const SDL_Event &event) {
    if (event.type != SDL_EVENT_KEY_DOWN && event.type != SDL_EVENT_KEY_UP) {
        return false;
    }
    const SDL_Scancode scancode = event.key.scancode;
    return scancode == SDL_SCANCODE_F5 || scancode == SDL_SCANCODE_F6;
}

bool IsPointInsideRect(float x, float y, float min_x, float min_y, float max_x,
                       float max_y) {
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    return fx >= min_x && fx <= max_x && fy >= min_y && fy <= max_y;
}

bool TryGetMousePositionFromEvent(const SDL_Event &event, float &x, float &y) {
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        x = event.motion.x;
        y = event.motion.y;
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        x = event.button.x;
        y = event.button.y;
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        SDL_GetMouseState(&x, &y);
        return true;
    default:
        return false;
    }
}

bool IsWindowFullscreen(SDL_Window *window) {
    if (window == nullptr) return false;
    const SDL_WindowFlags window_flags = SDL_GetWindowFlags(window);
    return (window_flags & SDL_WINDOW_FULLSCREEN) != 0;
}

#if defined(__APPLE__)
std::string TrimTrailingLineBreaks(std::string value) {
    while (!value.empty() &&
           (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}
#endif

std::string TrimWhitespace(std::string value) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::optional<std::filesystem::path> OpenNativeProjectFolderPicker() {
#if defined(_WIN32)
    HRESULT init_result =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                    COINIT_DISABLE_OLE1DDE);
    const bool should_uninitialize = SUCCEEDED(init_result);
    if (FAILED(init_result) && init_result != RPC_E_CHANGED_MODE) {
        return std::nullopt;
    }

    IFileOpenDialog *dialog = nullptr;
    HRESULT dialog_result = CoCreateInstance(
        CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));
    if (FAILED(dialog_result) || dialog == nullptr) {
        if (should_uninitialize) CoUninitialize();
        return std::nullopt;
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                           FOS_PATHMUSTEXIST);
    }
    dialog->SetTitle(L"Choose Pocket2D Project Folder");

    std::optional<std::filesystem::path> selected_path;
    if (SUCCEEDED(dialog->Show(nullptr))) {
        IShellItem *item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr) {
            PWSTR wide_path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,
                                               &wide_path)) &&
                wide_path != nullptr) {
                selected_path = std::filesystem::path(wide_path);
                CoTaskMemFree(wide_path);
            }
            item->Release();
        }
    }
    dialog->Release();
    if (should_uninitialize) CoUninitialize();
    return selected_path;
#elif defined(__APPLE__)
    FILE *pipe = popen(
        "osascript -e 'POSIX path of (choose folder with prompt "
        "\"Choose Pocket2D project folder\")'",
        "r");
    if (pipe == nullptr) return std::nullopt;

    std::string output;
    char buffer[4096];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    const int close_result = pclose(pipe);
    output = TrimTrailingLineBreaks(output);
    if (close_result != 0 || output.empty()) return std::nullopt;
    return std::filesystem::path(output);
#else
    return std::nullopt;
#endif
}

// Top-level menu owns document-wide commands such as save. Panels below stay
// focused on selection and property editing.
void BuildMainMenuBar(bool &show_metrics_window,
                      bool &show_editor_settings_window,
                      bool &show_game_config_window,
                      bool &show_rendering_config_window,
                      bool &show_external_editors_window,
                      bool &show_script_reload_window,
                      bool &show_new_project_window,
                      bool &show_open_project_window,
                      std::string &new_project_name,
                      std::string &new_project_error,
                      std::string &open_project_path,
                      const EditorConfigData &editor_config,
                      const SceneDocument &scene_document,
                      bool play_mode_active, bool play_mode_paused,
                      bool edit_mode_live_preview_enabled,
                      bool window_fullscreen,
                      bool scene_save_enabled,
                      bool &reset_layout_requested,
                      EditorOverlayResult &result) {
#if defined(_WIN32) || defined(__APPLE__)
    (void)show_open_project_window;
    (void)open_project_path;
#endif
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Project...")) {
            new_project_name.clear();
            new_project_error.clear();
            show_new_project_window = true;
        }
        if (ImGui::MenuItem("Open Project...")) {
            const std::optional<std::filesystem::path> selected_path =
                OpenNativeProjectFolderPicker();
            if (selected_path.has_value()) {
                result.open_project_requested = true;
                result.requested_project_root =
                    selected_path->lexically_normal();
            }
#if !defined(_WIN32) && !defined(__APPLE__)
            else {
                open_project_path = ResourcePath::ResourcesRootPath().string();
                show_open_project_window = true;
            }
#endif
        }
        if (!editor_config.recent_project_roots.empty() &&
            ImGui::BeginMenu("Recent Projects")) {
            for (const std::filesystem::path &project_root :
                 editor_config.recent_project_roots) {
                if (ImGui::MenuItem(project_root.string().c_str())) {
                    result.open_project_requested = true;
                    result.requested_project_root =
                        project_root.lexically_normal();
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save Scene", "Ctrl+S", false,
                            scene_save_enabled)) {
            result.save_scene_requested = true;
        }
        ImGui::Separator();
        ImGui::TextDisabled("Project: %s",
                            ResourcePath::ResourcesRootPath()
                                .string()
                                .c_str());
        ImGui::TextDisabled("%s", scene_document.GetScenePath().string().c_str());
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Editor")) {
        ImGui::MenuItem("Editor Settings", nullptr,
                        &show_editor_settings_window);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Project")) {
        ImGui::MenuItem("Game Config", nullptr, &show_game_config_window);
        ImGui::MenuItem("Rendering Config", nullptr,
                        &show_rendering_config_window);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Runtime")) {
        if (!play_mode_active) {
            if (ImGui::MenuItem("Play", "F5", false, true)) {
                result.play_mode_start_requested = true;
            }
        } else {
            if (ImGui::MenuItem(play_mode_paused ? "Resume" : "Pause",
                                "F6", false, true)) {
                result.play_mode_pause_toggle_requested = true;
            }
            if (ImGui::MenuItem("Stop", "F5", false, true)) {
                result.play_mode_stop_requested = true;
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Live Preview", nullptr,
                            edit_mode_live_preview_enabled,
                            !play_mode_active)) {
            result.edit_mode_live_preview_toggle_requested = true;
        }
        ImGui::EndMenu();
    }

    // imgui demo and metrics
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Fullscreen", "F11", window_fullscreen, true)) {
            result.toggle_fullscreen_requested = true;
        }
        if (ImGui::MenuItem("Reset Layout")) reset_layout_requested = true;
        ImGui::Separator();
        ImGui::MenuItem("ImGui Metrics", nullptr, &show_metrics_window);
        ImGui::EndMenu();
    }

    // Explicit authoring tools do not alter the Project browser Refresh action.
    if (ImGui::BeginMenu("Tools")) {
        ImGui::MenuItem("External Editors", nullptr,
                        &show_external_editors_window);
        ImGui::MenuItem("Reload Lua Scripts", nullptr, &show_script_reload_window);
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

// Unity's default arrangement: Inspector the full height on the right,
// Project and Status along the bottom, Hierarchy left of the Scene and game
// Viewport tabs.
void BuildDefaultDockLayout(ImGuiID dockspace_id, ImVec2 size) {
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, size);
    ImGuiID center = dockspace_id;
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.22f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr, &center);
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.22f, nullptr, &center);
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderDockWindow("Scene", center);
    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Map Asset Draft", right);
    ImGui::DockBuilderDockWindow("Project", bottom);
    ImGui::DockBuilderDockWindow("Status", bottom);
    ImGui::DockBuilderFinish(dockspace_id);
}

float ComputeEffectiveUiScale(const EditorConfigData &config, int window_width,
                              int window_height) {
                                  const float safe_reference_width = static_cast<float>(std::max(1, config.reference_width));
                                  const float safe_reference_height = static_cast<float>(std::max(1, config.reference_height));
                                  const float width_scale = static_cast<float>(std::max(1, window_width)) / safe_reference_width;
                                  const float height_scale = static_cast<float>(std::max(1, window_height)) / safe_reference_height;
    const float auto_scale = std::min(width_scale, height_scale);
    return std::max(0.5f, config.ui_scale * auto_scale);
}

bool IsBlankCommand(const std::string &command) {
    return std::all_of(command.begin(), command.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
}

// The operating system's UI font, when it has one as a single font file.
std::string PlatformUiFontPath() {
#if defined(__APPLE__)
    const std::filesystem::path path = "/System/Library/Fonts/SFNS.ttf";  // SF Pro
#elif defined(_WIN32)
    const char *windows = std::getenv("WINDIR");
    const std::filesystem::path path =
        std::filesystem::path(windows != nullptr ? windows : "C:\\Windows") / "Fonts" / "segoeui.ttf";
#else
    const std::filesystem::path path;
#endif
    std::error_code error;
    return !path.empty() && std::filesystem::is_regular_file(path, error) ? path.string() : std::string();
}

// "platform": the OS UI font, else the bundled Inter (.engine/system/fonts/system.ttf).
void LoadEditorDefaultFont(ImGuiIO &io, std::string_view preference) {
    io.Fonts->Clear();
    if (preference != "inter") {
        const std::string platform_path = PlatformUiFontPath();
        if (!platform_path.empty()) {
            if (ImFont *font = io.Fonts->AddFontFromFileTTF(platform_path.c_str(), kEditorFontPixelSize)) {
                io.FontDefault = font;
                return;
            }
        }
    }
    std::string font_path = ResourcePath::ResolveResourcePath(
        ResourcePath::EngineSystemFontsRoot(), "system", {".ttf"});
    if (font_path.empty()) {
        // Backward compatibility for projects that have not moved local editor
        // assets out of resources yet.
        font_path = ResourcePath::ResolveResourcePath(
            ResourcePath::ResourceSubdirectory("fonts"), "system", {".ttf"});
    }
    if (!font_path.empty()) {
        if (ImFont *font = io.Fonts->AddFontFromFileTTF(font_path.c_str(), kEditorFontPixelSize)) {
            io.FontDefault = font;
            return;
        }
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "EditorOverlay: editor system font not found, fallback to Dear ImGui default font.");
    }

    io.FontDefault = io.Fonts->AddFontDefault();
}

/*
helper: ImGui doesn't have a built-in InputText variant that works with std::string, 
so we gotta  provide that functionality.
std::string <-> ImGui char buffer
*/
bool InputTextString(const char *label, std::string &value, const char *hint) {
    std::vector<char> buffer(std::max<std::size_t>(1024, value.size() + 256), '\0');
    std::memcpy(buffer.data(), value.c_str(), value.size());

    bool changed = false;
    if (hint != nullptr && hint[0] != '\0') {
        changed = ImGui::InputTextWithHint(label, hint, buffer.data(),
                                           buffer.size());
    } else {
        changed = ImGui::InputText(label, buffer.data(), buffer.size());
    }
    if (!changed) {
        return false;
    }

    value = buffer.data();
    return true;
}

const char *ExternalEditorCommandHint(EditorExternalFileType type) {
    if (type == EditorExternalFileType::Lua) return "e.g. code {file} (supports {file}, {resources})";
    return "Default appends clicked file path. Supports {file}, {resources}";
}

std::vector<std::string> CollectAvailableSceneNames() {
    const std::filesystem::path scene_root =
        ResourcePath::ResourceSubdirectory("scenes");
    std::vector<std::string> scene_names;
    if (!std::filesystem::exists(scene_root) ||
        !std::filesystem::is_directory(scene_root)) {
        return scene_names;
    }

    const std::vector<std::filesystem::path> scene_files =
        ResourcePath::CollectFilesRecursively(scene_root, ".scene");
    scene_names.reserve(scene_files.size());
    for (const std::filesystem::path &scene_file : scene_files) {
        std::error_code relative_error;
        const std::filesystem::path relative_path =
            std::filesystem::relative(scene_file, scene_root, relative_error);
        if (relative_error || relative_path.empty()) continue;

        std::filesystem::path scene_name = relative_path;
        scene_name.replace_extension();
        scene_names.emplace_back(scene_name.generic_string());
    }

    std::sort(scene_names.begin(), scene_names.end());
    return scene_names;
}

void RenderExternalEditorsWindow(EditorConfigData &editor_config,
                                 bool &show_window,
                                 EditorOverlayResult &result) {
    if (!show_window) return;

    if (!ImGui::Begin("External Editors", &show_window)) {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("Double-click non-.scene files in Project to open external editor.");
    ImGui::TextUnformatted("By default the clicked file path is appended as command argument.");
    ImGui::TextDisabled("Placeholders: {file} and {resources}");
    ImGui::TextDisabled("Leave command empty to disable external open for that file type.");
    ImGui::Separator();

    const ImGuiTableFlags table_flags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
        ImGuiTableFlags_BordersV | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("external_editors_table", 3, table_flags)) {
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Config Key", ImGuiTableColumnFlags_WidthFixed,
                                120.0f);
        ImGui::TableSetupColumn("Command");
        ImGui::TableHeadersRow();

        for (const EditorExternalFileType type :
             EditorConfig::ExternalEditorFileTypes()) {
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(EditorConfig::ExternalEditorDisplayLabel(type));

            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s",
                                EditorConfig::ExternalEditorConfigKey(type));

            ImGui::TableNextColumn();
            std::string &command = EditorConfig::ExternalEditorCommand(editor_config, type);
            const std::string input_id = std::string("##external_editor_cmd_") +
                                         EditorConfig::ExternalEditorConfigKey(type);
            if (InputTextString(input_id.c_str(), command,
                                ExternalEditorCommandHint(type))) {
                result.editor_config_changed = true;
            }
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

} // namespace

EditorOverlay::~EditorOverlay() {
    Shutdown();
}

void EditorOverlay::EnsureProjectConfigLoaded() {
    if (project_config_loaded_) return;
    project_config_cache_ = GameConfig::Read();
    project_config_loaded_ = true;
}

void EditorOverlay::ShowTransientNotice(const std::string &text,
                                        double duration_seconds) {
    transient_notice_text_ = text;
    show_transient_notice_ = true;
    transient_notice_expire_time_ = ImGui::GetTime() + duration_seconds;
}

void EditorOverlay::ShowNotice(const std::string &text,
                               double duration_seconds) {
    ShowTransientNotice(text, duration_seconds);
}

// Create the ImGui context and bind it to the shared SDL window and renderer3
bool EditorOverlay::Initialize(SDL_Window *window, SDL_Renderer *renderer, std::string_view font) {
    if (initialized_) return true;
    if (window == nullptr || renderer == nullptr) return false;

    IMGUI_CHECKVERSION();
    // create the global context for imgui
    ImGui::CreateContext();

    // config IO style: enable docking and set up font/theme.
    // Keyboard navigation fights gameplay controls in play mode: arrow keys
    // should remain runtime input instead of moving focus across toolbar buttons.
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // The panel layout is per user, like editor.config, not per launch folder.
    std::error_code ini_error;
    std::filesystem::create_directories(ResourcePath::UserConfigRoot(), ini_error);
    imgui_ini_path_ = (ResourcePath::UserConfigRoot() / "imgui.ini").string();
    io.IniFilename = imgui_ini_path_.c_str();
    LoadEditorDefaultFont(io, font);
    ImGuiStyle &style = ImGui::GetStyle();
    EditorTheme::Apply(style, "midnight", 1.0f);

    if (!ImGui_ImplSDL3_InitForSDLRenderer(window, renderer)) {
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplSDLRenderer3_Init(renderer)) {
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    initialized_ = true;
    return true;
}

// Apply cached editor settings to the live ImGui style and font scale.
void EditorOverlay::ApplyConfig(const EditorConfigData &config, int window_width,
                                int window_height) {
    if (!initialized_) return;

    const float effective_ui_scale = ComputeEffectiveUiScale(config, window_width, window_height);
    if (std::fabs(effective_ui_scale - applied_ui_scale_) < 0.001f && config.theme == applied_theme_) {
        return;
    }

    ImGuiStyle &style = ImGui::GetStyle();
    EditorTheme::Apply(style, config.theme, effective_ui_scale);
    applied_theme_ = config.theme;

    style.SeparatorSize = std::max(style.SeparatorSize, 1.0f);
    style.SeparatorTextBorderSize = std::max(style.SeparatorTextBorderSize, 1.0f);
    style.DockingSeparatorSize = std::max(style.DockingSeparatorSize, 1.0f);
    // Dear ImGui sanity checks require this value to remain strictly positive
    style.WindowBorderHoverPadding = std::max(style.WindowBorderHoverPadding, 1.0f);
    // Since Dear ImGui 1.92, global font scaling lives on style.FontScaleMain
    style.FontScaleMain = effective_ui_scale;
    ImGuiIO &io = ImGui::GetIO();
    io.FontGlobalScale = 1.0f;
    applied_ui_scale_ = effective_ui_scale;
}

// Update routing context used to decide whether input goes to editor or
// runtime while the viewport is embedded.
void EditorOverlay::SetRuntimeInputRoutingState(bool play_mode_active,
                                                bool play_mode_paused) {
    const bool entering_play = !play_mode_active_for_input_ && play_mode_active;
    const bool resuming_from_pause =
        play_mode_active_for_input_ && play_mode_paused_for_input_ &&
        play_mode_active && !play_mode_paused;
    play_mode_active_for_input_ = play_mode_active;
    play_mode_paused_for_input_ = play_mode_paused;
    if (entering_play || resuming_from_pause) {
        runtime_input_focus_ = true;
        return;
    }
    if (!play_mode_active_for_input_) {
        runtime_input_focus_ = false;
    }
}

const ScriptReloadResult &EditorOverlay::ExecuteScriptReload(const std::vector<std::string> &types) {
    // Render runs on the editor owner thread after engine callbacks complete.
    // No scene document, runtime mirror, play-state or map draft is rebuilt here.
    script_reload_selection_ = types;
    has_script_reload_result_ = true;
    try {
        script_reload_result_ = ComponentManager::ReloadScripts(types);
    } catch (const std::exception &) {
        script_reload_result_ = {};
        script_reload_result_.types = types;
        script_reload_result_.diagnostics.push_back({"", "", "host_reset_required",
            "Unexpected host failure; runtime reset/reopen required; rollback not guaranteed", 0});
    }
    return script_reload_result_;
}

void EditorOverlay::RenderScriptReloadWindow() {
    if (!show_script_reload_window_) return;
    ImGui::SetNextWindowPos(ImVec2(80, 80), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Reload Lua Scripts", &show_script_reload_window_)) {
        ImGui::End();
        return;
    }
    ImGui::BeginDisabled(script_reload_selection_.empty());
    if (ImGui::Button("Reload Selected Scripts", ImVec2(220, 0))) {
        const auto selected = script_reload_selection_;
        ExecuteScriptReload(selected);
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped("Reload reads saved Lua files. Existing instances and data stay live; this action does not restart play or save scene/map drafts.");
    ImGui::Separator();
    auto types = ComponentManager::GetRegisteredComponentTypes();
    std::sort(types.begin(), types.end());
    if (ImGui::BeginChild("ReloadScriptSources", ImVec2(0, 180), ImGuiChildFlags_Borders)) {
        bool any = false;
        for (const auto &type : types) {
            const auto source = ComponentManager::GetComponentScriptSource(type);
            if (source.empty()) continue;
            any = true;
            auto found = std::find(script_reload_selection_.begin(), script_reload_selection_.end(), type);
            bool selected = found != script_reload_selection_.end();
            ImGui::PushID(type.c_str());
            if (ImGui::Checkbox(type.c_str(), &selected)) {
                if (selected) script_reload_selection_.push_back(type);
                else script_reload_selection_.erase(found);
            }
            ImGui::TextWrapped("%s", source.c_str());
            ImGui::PopID();
        }
        if (!any) ImGui::TextDisabled("No registered Lua component sources in this project.");
    }
    ImGui::EndChild();
    ImGui::SeparatorText("Last Reload Result");
    if (!has_script_reload_result_) ImGui::TextDisabled("No reload requested in this project.");
    else if (script_reload_result_.ok)
        ImGui::Text("Reloaded %zu type(s), updated %d live instance(s).", script_reload_result_.types.size(), script_reload_result_.instances_updated);
    else if (std::any_of(script_reload_result_.diagnostics.begin(), script_reload_result_.diagnostics.end(),
                        [](const auto &diagnostic) { return diagnostic.stage == "host_reset_required"; }))
        ImGui::TextWrapped("Unexpected host failure; runtime reset/reopen required; rollback not guaranteed.");
    else ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "Reload rejected; inspect the diagnostics below.");
    if (ImGui::BeginChild("ScriptReloadDiagnostics", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        for (const auto &diagnostic : script_reload_result_.diagnostics) {
            ImGui::TextWrapped("%s | %s | line %d", diagnostic.type.c_str(), diagnostic.stage.c_str(), diagnostic.line);
            ImGui::TextWrapped("%s", diagnostic.resource.c_str());
            ImGui::TextWrapped("%s", diagnostic.message.c_str());
            ImGui::Separator();
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void EditorOverlay::NotifySceneOpened() {
    selected_actor_index_ = -1;
    selected_runtime_actor_uid_ = Actor::kInvalidUID;
}

void EditorOverlay::NotifyProjectChanged() {
    EditorPanels::InvalidateProjectPanelCache();
    project_config_loaded_ = false;
    selected_actor_index_ = -1;
    selected_runtime_actor_uid_ = Actor::kInvalidUID;
    play_mode_active_for_input_ = false;
    play_mode_paused_for_input_ = false;
    runtime_input_focus_ = false;
    viewport_runtime_image_valid_ = false;
    viewport_transport_bar_valid_ = false;
    applied_ui_scale_ = 0.0f;
    applied_theme_.clear();
    show_open_project_window_ = false;
    open_project_error_.clear();
    show_script_reload_window_ = false;
    has_script_reload_result_ = false;
    script_reload_selection_.clear();
    script_reload_result_ = {};
}

// Forward one SDL event to ImGui and report whether the editor/runtime capture it
/*
Editor not initialized -> runtime
EditorPlaybackHotkey -> editor
playmode + runtime focus -> runtime

then: dependes on whether Imgui wants to capture it.
*/
bool EditorOverlay::ProcessEvent(const SDL_Event &event) {
    if (!initialized_) return false;
    /*
    In play mode, runtime keyboard input wins before ImGui sees the event.
    Otherwise ImGui keyboard navigation can move focus across Play/Pause/Stop
    while the user is using arrow keys or WASD for gameplay.
    */
    if (play_mode_active_for_input_ && ShouldCaptureKeyboardEvent(event.type) &&
        !IsEditorPlaybackHotkeyEvent(event)) {
        return false;
    }

    // give SDL input event to ImGui for processing
    ImGui_ImplSDL3_ProcessEvent(&event);
    const ImGuiIO &io = ImGui::GetIO();
    if (IsEditorPlaybackHotkeyEvent(event)) return true;
    if (!play_mode_active_for_input_) runtime_input_focus_ = false;
    /*
    Gameplay keyboard should feel immediate after pressing Play.  ImGui can
    still capture text-entry fields, but normal key events go to runtime while
    play mode is active so WASD/arrow controls are not eaten by docked panels.
    */
    if (play_mode_active_for_input_ && ShouldCaptureKeyboardEvent(event.type) &&
        !io.WantTextInput) {
        return false;
    }

    // check if the the event's position is inside the runtime viewport
    float mouse_x = 0;
    float mouse_y = 0;
    const bool has_mouse_pos = TryGetMousePositionFromEvent(event, mouse_x, mouse_y);
    const bool mouse_in_runtime_view =
        has_mouse_pos && viewport_runtime_image_valid_ &&
        IsPointInsideRect(mouse_x, mouse_y, viewport_runtime_image_min_x_,
                          viewport_runtime_image_min_y_,
                          viewport_runtime_image_max_x_,
                          viewport_runtime_image_max_y_);
    const bool mouse_in_transport_bar =
        has_mouse_pos && viewport_transport_bar_valid_ &&
        IsPointInsideRect(mouse_x, mouse_y, viewport_transport_bar_min_x_,
                          viewport_transport_bar_min_y_,
                          viewport_transport_bar_max_x_,
                          viewport_transport_bar_max_y_);

    /*
    Clicking the runtime viewport grants runtime focus for subsequent input.
    Clicking outside returns focus to editor.
    */
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        runtime_input_focus_ = play_mode_active_for_input_ && mouse_in_runtime_view && !mouse_in_transport_bar;
    }

    /*
    While runtime has input focus in play mode,
    route keyboard and viewport mouse input to runtime even if ImGui requests capture.
    */
    if (play_mode_active_for_input_ && runtime_input_focus_) {
        if (ShouldCaptureKeyboardEvent(event.type)) {
            return false;
        }
        if (ShouldCaptureMouseEvent(event.type) && mouse_in_runtime_view &&
            !mouse_in_transport_bar) {
            return false;
        }
    }

    /*
    Mirror Imgui's capture flags so the editor can consume UI input
    without blocking essential runtime window events.
    */
    if (ShouldCaptureMouseEvent(event.type)) {
        return io.WantCaptureMouse;
    }
    if (ShouldCaptureKeyboardEvent(event.type)) {
        return io.WantCaptureKeyboard;
    }
    return false;
}

SDL_Event EditorOverlay::BuildRuntimeEvent(const SDL_Event &event,
                                           const Engine &engine) const {
    SDL_Event runtime_event = event;
    if (!play_mode_active_for_input_ || !viewport_runtime_image_valid_) {
        return runtime_event;
    }
    if (!ShouldCaptureMouseEvent(event.type)) {
        return runtime_event;
    }

    float mouse_x = 0;
    float mouse_y = 0;
    if (!TryGetMousePositionFromEvent(event, mouse_x, mouse_y)) {
        return runtime_event;
    }
    if (!IsPointInsideRect(mouse_x, mouse_y, viewport_runtime_image_min_x_,
                           viewport_runtime_image_min_y_,
                           viewport_runtime_image_max_x_,
                           viewport_runtime_image_max_y_)) {
        return runtime_event;
    }

    const float viewport_width =
        std::max(1.0f, viewport_runtime_image_max_x_ -
                           viewport_runtime_image_min_x_);
    const float viewport_height =
        std::max(1.0f, viewport_runtime_image_max_y_ -
                           viewport_runtime_image_min_y_);
    const int runtime_width =
        std::max(1, engine.GetConfig().window_width);
    const int runtime_height =
        std::max(1, engine.GetConfig().window_height);
    const int runtime_x = std::clamp(
        static_cast<int>(std::lround(
            (static_cast<float>(mouse_x) - viewport_runtime_image_min_x_) /
            viewport_width * static_cast<float>(runtime_width))),
        0, runtime_width - 1);
    const int runtime_y = std::clamp(
        static_cast<int>(std::lround(
            (static_cast<float>(mouse_y) - viewport_runtime_image_min_y_) /
            viewport_height * static_cast<float>(runtime_height))),
        0, runtime_height - 1);

    if (runtime_event.type == SDL_EVENT_MOUSE_MOTION) {
        runtime_event.motion.x = runtime_x;
        runtime_event.motion.y = runtime_y;
    } else if (runtime_event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
               runtime_event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        runtime_event.button.x = runtime_x;
        runtime_event.button.y = runtime_y;
    }
    return runtime_event;
}

// Draw the editor UI for the current scene cache and runtime frame
EditorOverlayResult EditorOverlay::Render(Engine &engine,
                                          SceneDocument &scene_document,
                                          EditorConfigData &editor_config,
                                          bool editor_config_confirmation_pending,
                                          bool play_mode_active,
                                          bool play_mode_paused,
                                          bool edit_mode_live_preview_enabled,
                                          bool scene_editing_enabled,
                                          bool scene_save_enabled) {
    EditorOverlayResult result;
    if (!initialized_) return result;

    // get the SDL_Renderer 
    SDL_Renderer *renderer = engine.GetRenderer();
    if (renderer == nullptr) return result;

    // standard new frame setup for imgui with SDL_Renderer backend
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    // Global save should stay out of the way while the user is typing into
    // rename fields, editor settings, or other focused widgets.
    const ImGuiIO &io = ImGui::GetIO();
    const bool allow_global_save_shortcut = !io.WantTextInput && !ImGui::IsAnyItemActive();

    // Ctrl+S: shortcut key for save scene
    if (scene_save_enabled && allow_global_save_shortcut &&
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S,
                        ImGuiInputFlags_RouteGlobal)) {
        result.save_scene_requested = true;
    }
    // F5: shortcut key for play/stop
    if (ImGui::Shortcut(ImGuiKey_F5, ImGuiInputFlags_RouteGlobal)) {
        if (play_mode_active) {
            result.play_mode_stop_requested = true;
        } else {
            result.play_mode_start_requested = true;
        }
    }
    // F6: shortcut key for pause/resume
    if (play_mode_active &&
        ImGui::Shortcut(ImGuiKey_F6, ImGuiInputFlags_RouteGlobal)) {
        result.play_mode_pause_toggle_requested = true;
    }
    if (ImGui::Shortcut(ImGuiKey_F11, ImGuiInputFlags_RouteGlobal)) {
        result.toggle_fullscreen_requested = true;
    }

    /*
    build main menu and the dock space, then render panels:
    status, project, viewport, scene, hierarchy, inspector
    */
    const bool window_fullscreen = IsWindowFullscreen(engine.GetWindow());
    BuildMainMenuBar(show_metrics_window_,
                     show_editor_settings_window_, show_game_config_window_,
                     show_rendering_config_window_,
                     show_external_editors_window_, show_script_reload_window_,
                     show_new_project_window_,
                     show_open_project_window_, new_project_name_,
                     new_project_error_, open_project_path_, editor_config,
                     scene_document, play_mode_active, play_mode_paused,
                     edit_mode_live_preview_enabled, window_fullscreen,
                     scene_save_enabled, reset_dock_layout_requested_, result);

    ImGuiViewport *main_viewport = ImGui::GetMainViewport();
    const ImGuiID dockspace_id = ImGui::DockSpaceOverViewport(0, main_viewport, ImGuiDockNodeFlags_None);
    // Without a saved docking layout every panel would open as a small
    // floating window in the same corner; start from the default instead.
    if (!dock_layout_checked_) {
        dock_layout_checked_ = true;
        const ImGuiDockNode *node = ImGui::DockBuilderGetNode(dockspace_id);
        if (node == nullptr || !node->IsSplitNode()) reset_dock_layout_requested_ = true;
    }
    if (reset_dock_layout_requested_) {
        reset_dock_layout_requested_ = false;
        BuildDefaultDockLayout(dockspace_id, main_viewport->WorkSize);
        select_default_tabs_in_ = 60;
    }
    RenderMapDraftTools(play_mode_active);

    EditorPanels::RenderStatusPanel(engine, scene_document,
                                    selected_actor_index_,
                                    selected_runtime_actor_uid_,
                                    play_mode_active, play_mode_paused,
                                    edit_mode_live_preview_enabled,
                                    applied_ui_scale_);

    const EditorPanels::ProjectPanelResult project_panel_result =
        EditorPanels::RenderProjectPanel(ResourcePath::ResourcesRootPath(),
                                         renderer);
    if (project_panel_result.open_scene_requested) {
        result.open_scene_requested = true;
        result.requested_scene_path = project_panel_result.requested_scene_path;
    }
    if (project_panel_result.open_external_editor_requested) {
        const EditorExternalFileType file_type = project_panel_result.requested_external_file_type;
        const std::string &configured_command =
            EditorConfig::ExternalEditorCommand(editor_config, file_type);
        // Missing command should never block the editor loop. Surface a short
        // notice and let users jump directly to Tools -> External Editors.
        if (IsBlankCommand(configured_command)) {
            ShowExternalEditorMissingConfigNotice(file_type);
        } else {
            result.open_external_editor_requested = true;
            result.requested_external_file_type = file_type;
            result.requested_external_file_path = project_panel_result.requested_external_file_path;
            result.requested_external_project_root = project_panel_result.requested_external_project_root;
        }
    }
    if (!play_mode_active) {
        selected_runtime_actor_uid_ = Actor::kInvalidUID;
    } else if (selected_actor_index_ >= 0 &&
               selected_actor_index_ <
                   static_cast<int>(scene_document.GetActorCount())) {
        const SceneDocument::ActorUID selected_actor_uid = scene_document.GetActorUID( static_cast<std::size_t>(selected_actor_index_));
        const Actor *selected_runtime_actor =
            engine.GetRuntimeActorByUID(selected_runtime_actor_uid_);
        if (selected_runtime_actor == nullptr ||
            selected_runtime_actor->uid != selected_actor_uid) {
                const Actor *runtime_actor =
                    engine.GetRuntimeActorByUID(selected_actor_uid);
                selected_runtime_actor_uid_ =
                    (runtime_actor != nullptr) ? runtime_actor->uid
                                               : Actor::kInvalidUID;
        }
    }

    result.scene_changed |= EditorPanels::RenderHierarchyPanel(
        engine, scene_document, selected_actor_index_,
        selected_runtime_actor_uid_,
        play_mode_active, scene_editing_enabled, &result.scene_edit_commands);
    result.scene_changed |= EditorPanels::RenderInspectorPanel(
        engine, scene_document, selected_actor_index_,
        selected_runtime_actor_uid_,
        play_mode_active, scene_editing_enabled, &result.scene_edit_commands,
        &tilemap_document_);
    if (!play_mode_active) SyncTilemapPreview(engine);

    const EditorPanels::SceneViewPanelResult scene_view_panel_result =
        EditorPanels::RenderSceneViewPanel(
            engine, scene_document, selected_actor_index_,
            selected_runtime_actor_uid_, editor_config.scene_view_width,
            editor_config.scene_view_height, play_mode_active,
            play_mode_paused, scene_editing_enabled,
            &result.scene_edit_commands);
    result.scene_changed |= scene_view_panel_result.scene_changed;

    const EditorPanels::ViewportControlsResult viewport_controls =
        EditorPanels::RenderViewportPanel(engine, play_mode_active,
                                          play_mode_paused);
    result.runtime_view_visible = viewport_controls.panel_visible;
    if (viewport_controls.panel_visible) {
        engine.SetRuntimeRenderTargetSize(
            viewport_controls.requested_texture_width,
            viewport_controls.requested_texture_height);
    }
    result.play_mode_start_requested |= viewport_controls.play_mode_start_requested;
    result.play_mode_pause_toggle_requested |= viewport_controls.play_mode_pause_toggle_requested;
    result.play_mode_stop_requested |= viewport_controls.play_mode_stop_requested;

    // Runtime input routing should continue to follow the game viewport,
    // not the editable scene view.
    viewport_runtime_image_valid_ = viewport_controls.has_runtime_image;
    viewport_runtime_image_min_x_ = viewport_controls.runtime_image_min_x;
    viewport_runtime_image_min_y_ = viewport_controls.runtime_image_min_y;
    viewport_runtime_image_max_x_ = viewport_controls.runtime_image_max_x;
    viewport_runtime_image_max_y_ = viewport_controls.runtime_image_max_y;
    viewport_transport_bar_valid_ = viewport_controls.has_transport_bar;
    viewport_transport_bar_min_x_ = viewport_controls.transport_bar_min_x;
    viewport_transport_bar_min_y_ = viewport_controls.transport_bar_min_y;
    viewport_transport_bar_max_x_ = viewport_controls.transport_bar_max_x;
    viewport_transport_bar_max_y_ = viewport_controls.transport_bar_max_y;

    if (show_metrics_window_) ImGui::ShowMetricsWindow(&show_metrics_window_);

    RenderEditorSettingsWindow(engine, editor_config, result);
    RenderProjectConfigWindows();
    RenderNewProjectWindow(editor_config, result);
    RenderOpenProjectWindow(result);
    RenderEditorConfigConfirmationWindow(editor_config_confirmation_pending,
                                         result);
    RenderExternalEditorsWindow(editor_config, show_external_editors_window_,
                                result);
    RenderScriptReloadWindow();
    RenderTransientNotifications();
    // After a fresh layout, bring Project to the front of its tab group once
    // it sits in a tab bar (within a second). Scene leads its group because
    // the Viewport does not take focus as it first appears.
    if (select_default_tabs_in_ > 0) {
        --select_default_tabs_in_;
        ImGuiWindow *project = ImGui::FindWindowByName("Project");
        if (project != nullptr && project->DockNode != nullptr && project->DockNode->TabBar != nullptr) {
            project->DockNode->TabBar->NextSelectedTabId = project->TabId;
            select_default_tabs_in_ = 0;
        }
    }
    if (scene_mirror_pending_ || !scene_edit_error_.empty()) {
        ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Scene edit status", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(scene_mirror_pending_ ? "Preview is out of date. Your draft is retained." : "The edit was not applied.");
            ImGui::BeginChild("edit_error", ImVec2(400.0f, 90.0f), true);
            ImGui::TextWrapped("%s", scene_edit_error_.c_str());
            ImGui::EndChild();
            if (scene_mirror_pending_ && ImGui::Button("Retry preview")) result.retry_scene_mirror_requested = true;
            if (!scene_mirror_pending_ && ImGui::Button("Dismiss")) result.dismiss_scene_edit_error_requested = true;
        }
        ImGui::End();
    }

    ImGui::Render();
    SDL_SetRenderScale(renderer,
                       std::max(0.01f, io.DisplayFramebufferScale.x),
                       std::max(0.01f, io.DisplayFramebufferScale.y));
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_SetRenderScale(renderer, 1.0f, 1.0f);
    return result;
}

// Queue a short-lived popup when a file type has no external tool command.
void EditorOverlay::ShowExternalEditorMissingConfigNotice(EditorExternalFileType type) {
    ShowTransientNotice(std::string("No external tool configured for ") +
                            EditorConfig::ExternalEditorDisplayLabel(type) +
                            ". Open Tools -> External Editors to configure one.",
                        4.0);
}

void EditorOverlay::RenderEditorSettingsWindow(Engine &engine,
                                               EditorConfigData &editor_config,
                                               EditorOverlayResult &result) {
    if (!show_editor_settings_window_) return;

    if (!ImGui::Begin("Editor Settings", &show_editor_settings_window_)) {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted( "These values control the editor window and overlay only.");
    ImGui::TextDisabled(
        "Window moves are auto-accepted. Fullscreen/layout changes still need Apply/Revert.");
    ImGui::Separator();

    bool config_changed = false;
    bool reapply_window_settings = false;

    if (ImGui::BeginCombo("Theme", EditorTheme::DisplayName(editor_config.theme))) {
        for (const std::string &name : EditorTheme::Names()) {
            const bool selected = name == editor_config.theme;
            if (ImGui::Selectable(EditorTheme::DisplayName(name), selected)) {
                editor_config.theme = name;
                config_changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    const auto font_label = [](std::string_view name) { return name == "inter" ? "Inter (bundled)" : "System UI font"; };
    if (ImGui::BeginCombo("Font", font_label(editor_config.font))) {
        for (const char *name : {"platform", "inter"}) {
            if (ImGui::Selectable(font_label(name), editor_config.font == name)) {
                editor_config.font = name;
                config_changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(after restart)");

    int window_width = editor_config.window_width;
    int window_height = editor_config.window_height;
    int reference_width = editor_config.reference_width;
    int reference_height = editor_config.reference_height;
    int scene_view_width = editor_config.scene_view_width;
    int scene_view_height = editor_config.scene_view_height;
    float ui_scale = editor_config.ui_scale;
    bool window_fullscreen = editor_config.window_fullscreen;
    bool window_maximized = editor_config.window_maximized;

    if (ImGui::InputInt("Editor Width", &window_width)) {
        editor_config.window_width = std::max(640, window_width);
        config_changed = true;
        reapply_window_settings = true;
    }
    if (ImGui::InputInt("Editor Height", &window_height)) {
        editor_config.window_height = std::max(360, window_height);
        config_changed = true;
        reapply_window_settings = true;
    }
    if (ImGui::Checkbox("Fullscreen", &window_fullscreen)) {
        editor_config.window_fullscreen = window_fullscreen;
        if (window_fullscreen) {
            editor_config.window_maximized = false;
        }
        config_changed = true;
        reapply_window_settings = true;
    }
    if (ImGui::Checkbox("Maximized", &window_maximized)) {
        editor_config.window_maximized = window_maximized;
        if (window_maximized) {
            editor_config.window_fullscreen = false;
        }
        config_changed = true;
        reapply_window_settings = true;
    }
    if (ImGui::InputFloat("UI Scale", &ui_scale, 0.05f, 0.25f, "%.2f")) {
        editor_config.ui_scale = std::max(0.5f, ui_scale);
        config_changed = true;
    }
    if (ImGui::InputInt("Reference Width", &reference_width)) {
        editor_config.reference_width = std::max(1, reference_width);
        config_changed = true;
    }
    if (ImGui::InputInt("Reference Height", &reference_height)) {
        editor_config.reference_height = std::max(1, reference_height);
        config_changed = true;
    }
    if (ImGui::InputInt("Scene View Width", &scene_view_width)) {
        editor_config.scene_view_width = std::max(64, scene_view_width);
        config_changed = true;
    }
    if (ImGui::InputInt("Scene View Height", &scene_view_height)) {
        editor_config.scene_view_height = std::max(64, scene_view_height);
        config_changed = true;
    }

    ImGui::Separator();
    ImGui::TextDisabled("Current window position: %d, %d",
                        editor_config.window_x, editor_config.window_y);

    if (config_changed) {
        ApplyConfig(editor_config, engine.GetWindowWidth(),
                    engine.GetWindowHeight());
        result.editor_config_changed = true;
    }
    if (reapply_window_settings) {
        result.editor_window_settings_changed = true;
    }

    ImGui::End();
}

void EditorOverlay::RenderProjectConfigWindows() {
    EnsureProjectConfigLoaded();

    auto persist_project_config = [this]() {
        if (!GameConfig::Write(project_config_cache_)) {
            ShowTransientNotice(
                "Unable to write project config. Check the current project's game.config and rendering.config.",
                5.0);
        }
    };

    if (show_game_config_window_) {
        if (ImGui::Begin("Game Config", &show_game_config_window_)) {
            ImGui::Text("These values write to %s immediately.",
                        (ResourcePath::ResourcesRootPath() / "game.config")
                            .string()
                            .c_str());
            ImGui::TextDisabled( "Current runtime/editor session keeps its already-loaded config.");
            if (ImGui::Button("Reload from Disk")) {
                project_config_cache_ = GameConfig::Read();
            }
            ImGui::Separator();

            bool config_changed = false;
            config_changed |= InputTextString(
                "Game Title", project_config_cache_.game_title,
                "Shown by the standalone runtime window");

                const std::vector<std::string> scene_names = CollectAvailableSceneNames();
            const char *preview_text =
                project_config_cache_.initial_scene_name.empty()
                    ? "<none>"
                    : project_config_cache_.initial_scene_name.c_str();
            if (ImGui::BeginCombo("Initial Scene", preview_text)) {
                for (const std::string &scene_name : scene_names) {
                    const bool selected = project_config_cache_.initial_scene_name == scene_name;
                    if (ImGui::Selectable(scene_name.c_str(), selected)) {
                        project_config_cache_.initial_scene_name = scene_name;
                        config_changed = true;
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            config_changed |= InputTextString(
                "Initial Scene Path", project_config_cache_.initial_scene_name,
                "e.g. ballgame/hw8_platformer_ballgame");


            if (config_changed) {
                persist_project_config();
            }
        }
        ImGui::End();
    }

    if (show_rendering_config_window_) {
        if (ImGui::Begin("Rendering Config", &show_rendering_config_window_)) {
            ImGui::Text("These values write to %s immediately.",
                        (ResourcePath::ResourcesRootPath() /
                         "rendering.config")
                            .string()
                            .c_str());
            ImGui::TextDisabled( "Current runtime/editor session keeps its already-loaded config.");
            if (ImGui::Button("Reload from Disk")) {
                project_config_cache_ = GameConfig::Read();
            }
            ImGui::Separator();

            bool config_changed = false;
            int width = project_config_cache_.window_width;
            int height = project_config_cache_.window_height;
            float zoom_factor = project_config_cache_.zoom_factor;
            float clear_color[3] = {
                project_config_cache_.clear_color_r / 255.0f,
                project_config_cache_.clear_color_g / 255.0f,
                project_config_cache_.clear_color_b / 255.0f};

            if (ImGui::InputInt("Runtime Width", &width)) {
                project_config_cache_.window_width = std::max(1, width);
                config_changed = true;
            }
            if (ImGui::InputInt("Runtime Height", &height)) {
                project_config_cache_.window_height = std::max(1, height);
                config_changed = true;
            }
            if (ImGui::InputFloat("Zoom Factor", &zoom_factor, 0.1f, 0.5f,
                                  "%.2f")) {
                                      project_config_cache_.zoom_factor = std::max(0.05f, zoom_factor);
                config_changed = true;
            }
            if (ImGui::ColorEdit3("Clear Color", clear_color,
                                  ImGuiColorEditFlags_DisplayRGB |
                                      ImGuiColorEditFlags_Float)) {
                project_config_cache_.clear_color_r = static_cast<int>(
                    std::round(std::max(0.0f, std::min(1.0f, clear_color[0])) *
                               255.0f));
                project_config_cache_.clear_color_g = static_cast<int>(
                    std::round(std::max(0.0f, std::min(1.0f, clear_color[1])) *
                               255.0f));
                project_config_cache_.clear_color_b = static_cast<int>(
                    std::round(std::max(0.0f, std::min(1.0f, clear_color[2])) *
                               255.0f));
                config_changed = true;
            }
            ImGui::TextDisabled("RGB: %d, %d, %d",
                                project_config_cache_.clear_color_r,
                                project_config_cache_.clear_color_g,
                                project_config_cache_.clear_color_b);

            if (config_changed) {
                persist_project_config();
            }
        }
        ImGui::End();
    }
}

void EditorOverlay::RenderNewProjectWindow(EditorConfigData &editor_config,
                                           EditorOverlayResult &result) {
    if (!show_new_project_window_) return;

    ImGui::SetNextWindowSize(ImVec2(640.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::Begin("New Project", &show_new_project_window_)) {
        ImGui::End();
        return;
    }

    // Unity Hub's layout: pick a template, name the project, choose where
    // its folder goes. The location is remembered for the next project.
    const std::vector<ProjectTemplate> templates = ProjectTemplates::List();
    new_project_template_index_ = std::clamp(
        new_project_template_index_, 0, static_cast<int>(templates.size()) - 1);
    if (new_project_location_.empty()) {
        new_project_location_ = editor_config.new_project_location.string();
    }
    if (new_project_name_.empty()) {
        new_project_name_ = ProjectTemplates::UniqueProjectName(
            TrimWhitespace(new_project_location_), "NewProject");
    }

    if (ImGui::BeginCombo("Template",
                          templates[new_project_template_index_].name.c_str())) {
        for (int index = 0; index < static_cast<int>(templates.size()); ++index) {
            const bool selected = index == new_project_template_index_;
            if (ImGui::Selectable(templates[index].name.c_str(), selected)) {
                new_project_template_index_ = index;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    InputTextString("Project Name", new_project_name_, "e.g. MyGame");
    InputTextString("Location", new_project_location_,
                    "e.g. /Users/me/Games, C:\\\\Games");
#if defined(_WIN32) || defined(__APPLE__)
    if (ImGui::Button("Choose Location...", ImVec2(160.0f, 0.0f))) {
        const std::optional<std::filesystem::path> selected_location =
            OpenNativeProjectFolderPicker();
        if (selected_location.has_value()) {
            new_project_location_ =
                selected_location->lexically_normal().string();
            new_project_error_.clear();
        }
    }
#endif

    const std::filesystem::path location =
        std::filesystem::path(TrimWhitespace(new_project_location_))
            .lexically_normal();
    const std::string project_name = TrimWhitespace(new_project_name_);
    const std::filesystem::path project_root = location / project_name;
    ImGui::TextDisabled("Will create: %s", project_root.string().c_str());

    if (!new_project_error_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s",
                           new_project_error_.c_str());
    }

    bool close_window = false;
    if (ImGui::Button("Create", ImVec2(120.0f, 0.0f))) {
        std::string error;
        if (!location.is_absolute()) {
            error = "Location must be an absolute folder path.";
        } else if (ProjectTemplates::IsValidProjectName(project_name, error) &&
                   ProjectTemplates::Create(
                       templates[new_project_template_index_], project_root,
                       project_name, error)) {
            editor_config.new_project_location = location;
            result.open_project_requested = true;
            result.requested_project_root = project_root;
            close_window = true;
        }
        new_project_error_ = error;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) {
        close_window = true;
    }
    if (close_window) {
        show_new_project_window_ = false;
        new_project_name_.clear();
        new_project_location_.clear();
        new_project_error_.clear();
    }

    ImGui::End();
}

void EditorOverlay::RenderOpenProjectWindow(EditorOverlayResult &result) {
    if (!show_open_project_window_) return;

    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::Begin("Open Project", &show_open_project_window_)) {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted(
        "Choose the folder that should behave as this project's root.");
    ImGui::TextDisabled(
        "Absolute paths and paths relative to the engine working directory both work.");
    ImGui::Separator();

    InputTextString("Project Folder", open_project_path_,
                    "e.g. /Users/me/Games/MyGame, ../my-game, C:\\\\Games\\\\MyProject");
    if (!open_project_error_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s",
                           open_project_error_.c_str());
    }

    if (ImGui::Button("Open", ImVec2(120.0f, 0.0f))) {
        const std::filesystem::path candidate =
            ResourcePath::NormalizeProjectRoot(open_project_path_);
        if (candidate.empty()) {
            open_project_error_ = "Path is empty.";
        } else if (!std::filesystem::exists(candidate) ||
                   !std::filesystem::is_directory(candidate)) {
            open_project_error_ = "Folder does not exist.";
        } else {
            result.open_project_requested = true;
            result.requested_project_root = candidate;
            open_project_error_.clear();
            show_open_project_window_ = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) {
        show_open_project_window_ = false;
        open_project_error_.clear();
    }

    ImGui::End();
}

void EditorOverlay::RenderEditorConfigConfirmationWindow( bool editor_config_confirmation_pending, EditorOverlayResult &result) {
    if (!editor_config_confirmation_pending) return;

    ImGuiViewport *viewport = ImGui::GetMainViewport();
    if (viewport == nullptr) return;

    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
               viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.97f);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("Confirm Overlay Change", nullptr, flags)) {
        ImGui::TextUnformatted( "Editor overlay/window changes are pending confirmation.");
        ImGui::TextDisabled( "You can keep editing. Apply will persist the latest layout/settings.");
        ImGui::Spacing();
        if (ImGui::Button("Apply Overlay", ImVec2(150.0f, 0.0f))) {
            result.editor_config_apply_requested = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert Overlay", ImVec2(150.0f, 0.0f))) {
            result.editor_config_revert_requested = true;
        }
    }
    ImGui::End();
}

// Render non-blocking top-right notifications for editor actions.
void EditorOverlay::RenderTransientNotifications() {
    if (!show_transient_notice_) return;

    if (ImGui::GetTime() >= transient_notice_expire_time_) {
        show_transient_notice_ = false;
        return;
    }

    ImGuiViewport *viewport = ImGui::GetMainViewport();
    if (viewport == nullptr) return;

    const float margin = 16.0f;
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - margin,
               viewport->WorkPos.y + margin),
        ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.95f);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##external_editor_notice", nullptr, flags)) {
        ImGui::TextWrapped("%s", transient_notice_text_.c_str());
        if (ImGui::SmallButton("Configure")) {
            show_external_editors_window_ = true;
            show_transient_notice_ = false;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Dismiss")) {
            show_transient_notice_ = false;
        }
    }
    ImGui::End();
}

// Release ImGui backend resources and destroy the ImGui context.
void EditorOverlay::Shutdown() {
    if (!initialized_) return;

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    initialized_ = false;
    NotifyProjectChanged();
    tilemap_document_.Clear();
    applied_map_previews_.clear();
}

void EditorOverlay::SyncTilemapPreview(Engine &engine) {
    std::unordered_map<std::string, AppliedMapPreview> next;
    bool refreshed_clean_document = false;
    if (tilemap_document_.HasAsset()) {
        for (const auto &actor : engine.GetRuntimeActors()) {
            if (actor.runtime_destroyed) continue;
            for (const auto &spec : ComponentManager::GetRuntimeComponentSpecs(actor.uid)) {
                if (spec.type != "Tilemap") continue;
                auto *map = ComponentManager::GetTilemap(actor.uid, spec.key);
                if (!map || map->map_resource.empty()) continue;
                const auto path = std::filesystem::weakly_canonical(std::filesystem::absolute(ResourcePath::ResourcesRootPath() / "maps" / map->map_resource));
                if (path != std::filesystem::weakly_canonical(tilemap_document_.GetPath())) continue;
                const auto identity = std::to_string(actor.uid) + ":" + spec.key;
                const auto previous = applied_map_previews_.find(identity);
                const bool same_instance = previous != applied_map_previews_.end() &&
                    previous->second.actor_generation == actor.runtime_generation && previous->second.instance == map;
                if (!same_instance && !tilemap_document_.HasUnsavedChanges() && !refreshed_clean_document) {
                    // A scene reload has already loaded the disk asset. A clean
                    // old editor cache must never overwrite that newer runtime.
                    if (!tilemap_document_.Revert()) ShowNotice(tilemap_document_.GetLastError());
                    refreshed_clean_document = true;
                }
                const AppliedMapPreview desired{actor.runtime_generation, tilemap_document_.GetRevision(), map};
                const bool changed_document = previous != applied_map_previews_.end() &&
                    previous->second.document_revision != desired.document_revision;
                if ((tilemap_document_.HasUnsavedChanges() &&
                     (!same_instance || changed_document || map->GetRevision() == 0)) ||
                    (same_instance && changed_document)) {
                    map->ReplaceAsset(tilemap_document_.GetAsset());
                }
                next.emplace(identity, desired);
            }
        }
        ComponentManager::CommitTilemaps();
    }
    applied_map_previews_ = std::move(next);
}

void EditorOverlay::RenderMapDraftTools(bool play_mode_active) {
    if (!tilemap_document_.HasAsset()) return;
    ImGui::SetNextWindowSize(ImVec2(420, 160), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Map Asset Draft")) {
        ImGui::TextWrapped("%s", tilemap_document_.GetPath().string().c_str());
        ImGui::TextUnformatted(tilemap_document_.HasUnsavedChanges() ? "Unsaved authoring changes" : "Saved authoring copy");
        if (play_mode_active) ImGui::TextDisabled("Stop playback to edit map assets.");
        ImGui::BeginDisabled(play_mode_active);
        if (ImGui::Button("Save Map")) {
            if (!tilemap_document_.Save()) ShowNotice(tilemap_document_.GetLastError());
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard Changes")) tilemap_document_.DiscardChanges();
        ImGui::BeginDisabled(!tilemap_document_.CanUndo());
        if (ImGui::Button("Undo Map")) tilemap_document_.Undo();
        ImGui::EndDisabled(); ImGui::SameLine();
        ImGui::BeginDisabled(!tilemap_document_.CanRedo());
        if (ImGui::Button("Redo Map")) tilemap_document_.Redo();
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        if (!tilemap_document_.GetLastError().empty())
            ImGui::TextWrapped("%s", tilemap_document_.GetLastError().c_str());
    }
    ImGui::End();
}
