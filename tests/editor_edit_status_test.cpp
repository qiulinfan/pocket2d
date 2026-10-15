#include "core/Engine.h"
#include "core/FrameClock.h"
#include "editor/core/EditorOverlay.h"
#include "editor/core/EditorSceneSession.h"
#include "shared/resources/ResourcePath.h"
#include "SDL3_image/SDL_image.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void Check(bool value, const std::string &message) { if (!value) throw std::runtime_error(message); }
void Write(const std::filesystem::path &path, const std::string &text) {
    std::ofstream stream(path); stream << text; Check(stream.good(), "cannot write temporary editor fixture");
}
std::string Scene(double width) {
    return "{\"actors\":[{\"uid\":1,\"name\":\"owner\",\"components\":{\"body\":{\"type\":\"Rigidbody\",\"body_type\":\"static\",\"has_trigger\":false,\"width\":" +
        std::to_string(width) + "},\"transform\":{\"type\":\"Transform\"}}}]}";
}
struct Project {
    std::filesystem::path previous_root = ResourcePath::ResourcesRootPath();
    std::filesystem::path previous_cwd = std::filesystem::current_path();
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("pocket-editor-edit-status-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Project() {
        for (const char *directory : {"scenes", "component_types", "images"}) std::filesystem::create_directories(root / directory);
        Write(root / "game.config", R"({"initial_scene":"main"})");
        Write(root / "rendering.config", R"({"x_resolution":960,"y_resolution":720})");
        Write(root / "scenes/main.scene", Scene(1));
        std::filesystem::current_path(root); ResourcePath::SetResourcesRootPath(root);
    }
    ~Project() {
        std::filesystem::current_path(previous_cwd); ResourcePath::SetResourcesRootPath(previous_root);
        std::error_code error; std::filesystem::remove_all(root, error);
    }
};
EditorOverlayResult Draw(EditorOverlay &overlay, Engine &engine, EditorSceneSession &session, EditorConfigData &config) {
    const int frame = FrameClock::GetFrameNumber();
    overlay.SetSceneEditStatus(session.GetLastEditError(), session.IsRuntimeMirrorPending());
    engine.RunRenderFrozenFrame(false, false);
    auto result = overlay.Render(engine, session.GetSceneDocument(), config, false, false, false, false, true, true);
    Check(FrameClock::GetFrameNumber() == frame, "edit status UI advanced simulation");
    Check(ImGui::GetDrawData() && ImGui::GetDrawData()->TotalVtxCount > 0, "editor overlay did not render");
    Check(!result.scene_changed && !result.save_scene_requested && !result.play_mode_start_requested &&
          !result.play_mode_stop_requested && !result.play_mode_pause_toggle_requested,
          "status UI changed unrelated scene/save/play state");
    return result;
}
bool Visible() {
    const auto *window = ImGui::FindWindowByName("Scene edit status");
    return window && window->Active && !window->Hidden && !window->Collapsed;
}
EditorOverlayResult Click(EditorOverlay &overlay, Engine &engine, EditorSceneSession &session,
                          EditorConfigData &config, const char *label) {
    Draw(overlay, engine, session, config); Draw(overlay, engine, session, config);
    auto *window = ImGui::FindWindowByName("Scene edit status");
    Check(Visible(), "scene edit status window not visible");
    ImGui::FocusWindow(window);
    // The status button is the window's final item, below its fixed-height error child.
    // Use actual ImGui layout state rather than a hardcoded screen coordinate.
    const ImVec2 point(window->DC.CursorStartPos.x + ImGui::CalcTextSize(label).x * 0.5f + ImGui::GetStyle().FramePadding.x,
                       window->DC.CursorPosPrevLine.y + ImGui::GetFrameHeight() * 0.5f);
    Check(window->Rect().Contains(point), "status action falls outside its real ImGui window");
    auto &io = ImGui::GetIO(); io.AddMousePosEvent(point.x, point.y); io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    Draw(overlay, engine, session, config);
    io.AddMousePosEvent(point.x, point.y); io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    return Draw(overlay, engine, session, config);
}
void Run(const std::filesystem::path &capture) {
    Project project;
    SDL_setenv_unsafe("SDL_VIDEO_DRIVER", "dummy", 1); SDL_setenv_unsafe("SDL_AUDIO_DRIVER", "dummy", 1);
    SDL_setenv_unsafe("POCKET_RENDER_DRIVER", "software", 1); SDL_setenv_unsafe("POCKET_RENDER_VSYNC", "0", 1);
    Engine engine; engine.InitializeRuntime(); SDL_HideWindow(engine.GetWindow());
    engine.SetRenderRuntimeToTexture(true); engine.SetRuntimeRenderTargetSize(320, 240);
    Check(std::string(SDL_GetRendererName(engine.GetRenderer())) == "software", "test did not use real SDL software renderer");
    EditorSceneSession session; session.LoadInitialScene(engine); session.SyncRuntimeMirrorIfDirty(engine);
    Check(!session.IsRuntimeMirrorPending(), "initial document did not synchronize");
    engine.StepSimulation();
    EditorOverlay overlay; Check(overlay.Initialize(engine.GetWindow(), engine.GetRenderer()), "cannot initialize ImGui overlay");
    ImGui::GetIO().IniFilename = nullptr;
    EditorConfigData config; config.window_width = config.reference_width = 960; config.window_height = config.reference_height = 720;
    config.scene_view_width = 320; config.scene_view_height = 240; config.current_project_root = project.root;
    overlay.ApplyConfig(config, 960, 720);
    // 1. A rejected synchronized edit is dismissible, without changing the old document.
    Check(!session.GetSceneDocument().SetComponentProperty(0, "body", "width", 0.0), "invalid synchronized edit succeeded");
    Check(!session.GetLastEditError().empty() && !session.IsRuntimeMirrorPending(), "rejected edit did not produce dismissible status");
    const auto dismissed = Click(overlay, engine, session, config, "Dismiss");
    Check(dismissed.dismiss_scene_edit_error_requested && !dismissed.retry_scene_mirror_requested,
          "actual ImGui Dismiss button did not return its action");
    session.ClearEditError(); Draw(overlay, engine, session, config);
    Check(!Visible() && session.GetLastEditError().empty(), "dismissed status remained visible");
    // 2. A separately edited invalid draft stays pending and exposes the actual retry action.
    Write(project.root / "scenes/main.scene", Scene(0));
    session.LoadInitialScene(engine); session.SyncRuntimeMirrorIfDirty(engine);
    Check(session.IsRuntimeMirrorPending() && !session.GetLastEditError().empty(), "invalid draft did not produce pending status");
    Draw(overlay, engine, session, config); Draw(overlay, engine, session, config);
    Check(Visible(), "pending preview status was not rendered");
    if (!capture.empty()) {
        std::filesystem::create_directories(capture.parent_path());
        std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> pixels(SDL_RenderReadPixels(engine.GetRenderer(), nullptr), SDL_DestroySurface);
        Check(pixels && IMG_SavePNG(pixels.get(), capture.string().c_str()), "status screenshot readback/save failed");
    }
    const auto attempt = engine.GetSceneLoadAttemptRevision();
    const auto retry = Click(overlay, engine, session, config, "Retry preview");
    Check(retry.retry_scene_mirror_requested && !retry.dismiss_scene_edit_error_requested,
          "actual ImGui Retry preview button did not return its action");
    session.RetryRuntimeMirror(); session.SyncRuntimeMirrorIfDirty(engine);
    Check(engine.GetSceneLoadAttemptRevision() == attempt + 1 && session.IsRuntimeMirrorPending(),
          "UI retry did not trigger exactly one new preparation attempt");
    Draw(overlay, engine, session, config); Check(Visible(), "still-invalid retry hid pending status");
    // 3. Repairing the retained draft synchronizes it and removes the status window.
    Check(session.GetSceneDocument().SetComponentProperty(0, "body", "width", 1.0), "cannot repair retained draft");
    session.SyncRuntimeMirrorIfDirty(engine); Draw(overlay, engine, session, config);
    Check(!session.IsRuntimeMirrorPending() && session.GetLastEditError().empty() && !Visible(),
          "successful repair did not remove scene edit status");
    Check(session.GetSceneDocument().IsDirty(), "repair silently saved/cleared authoring draft");
    overlay.Shutdown();
}
}
int main(int argc, char **argv) {
    try {
        Check(argc <= 2, "usage: editor_edit_status_test [CAPTURE_PNG]");
        const auto capture = argc == 2 ? std::filesystem::absolute(argv[1]) : std::filesystem::path{};
        Run(capture);
        std::cout << "editor_edit_status_test: real Dismiss, Retry preview and repaired-draft status passed\n"; return 0;
    } catch (const std::exception &error) {
        std::cerr << "editor_edit_status_test: " << error.what() << '\n'; return 1;
    }
}
