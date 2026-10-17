// Editor UI driven by Dear ImGui Test Engine: real clicks on the viewport
// transport buttons, the Runtime menu and the F5 shortcut start and stop a
// separate play World exactly as the editor host does, and File > New Project
// copies a template into a chosen location.
#include "core/Engine.h"
#include "core/FrameClock.h"
#include "core/World.h"
#include "editor/core/EditorOverlay.h"
#include "editor/core/EditorSceneSession.h"
#include "scripting/ComponentManager.h"
#include "shared/resources/ResourcePath.h"
#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"
#include <SDL3/SDL.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
void Check(bool condition, const std::string &message) { if (!condition) throw std::runtime_error(message); }

// The host's view of play mode, shared with the test coroutines.
struct Host {
    Engine *edit = nullptr;
    EditorSceneSession *session = nullptr;
    std::unique_ptr<World> play_world;
    std::unique_ptr<Engine> play;
    int starts = 0, stops = 0;
    std::filesystem::path new_project_location;
    std::filesystem::path opened_project;
    bool Playing() const { return play != nullptr; }

    void Start() {
        play_world = std::make_unique<World>();
        play = std::make_unique<Engine>(*play_world);
        play->InitializeEmbedded(edit->GetWindow(), edit->GetRenderer());
        play->SetRenderRuntimeToTexture(true);
        play->SetRuntimeRenderTargetSize(320, 240);
        Check(session->EnterPlayMode(*play), "enter play: " + play->GetLastSceneLoadError());
        ++starts;
    }
    void Stop() {
        Check(session->EndPlaySession(*edit), "end play session");
        play->ShutdownRuntime();
        play.reset();
        play_world.reset();
        ++stops;
    }
};
Host *g_host = nullptr;

void ExpectToggle(ImGuiTestContext *ctx, bool playing) {
    for (int frame = 0; frame < 10 && g_host->Playing() != playing; ++frame) ctx->Yield();
    IM_CHECK(g_host->Playing() == playing);
}

void RegisterTests(ImGuiTestEngine *engine) {
    ImGuiTest *test = IM_REGISTER_TEST(engine, "editor", "viewport_play_stop_buttons");
    test->TestFunc = [](ImGuiTestContext *ctx) {
        // Without the editor app's dock layout the viewport starts tiny; the
        // transport bar appears once the runtime image is at least 120x80.
        ctx->WindowMove("//Viewport", ImVec2(200, 60));
        ctx->WindowResize("//Viewport", ImVec2(520, 420));
        ctx->WindowFocus("//Viewport");
        ctx->SetRef("//Viewport");
        ctx->ItemClick("transport_play/##transport_icon");
        ExpectToggle(ctx, true);
        ctx->ItemClick("transport_stop/##transport_icon");
        ExpectToggle(ctx, false);
    };
    test = IM_REGISTER_TEST(engine, "editor", "runtime_menu_play_stop");
    test->TestFunc = [](ImGuiTestContext *ctx) {
        ctx->SetRef("##MainMenuBar");
        ctx->MenuClick("Runtime/Play");
        ExpectToggle(ctx, true);
        ctx->MenuClick("Runtime/Stop");
        ExpectToggle(ctx, false);
    };
    test = IM_REGISTER_TEST(engine, "editor", "f5_play_stop");
    test->TestFunc = [](ImGuiTestContext *ctx) {
        ctx->KeyPress(ImGuiKey_F5);
        ExpectToggle(ctx, true);
        ctx->KeyPress(ImGuiKey_F5);
        ExpectToggle(ctx, false);
    };
    test = IM_REGISTER_TEST(engine, "editor", "new_project_from_template");
    test->TestFunc = [](ImGuiTestContext *ctx) {
        ctx->SetRef("##MainMenuBar");
        ctx->MenuClick("File/New Project...");
        ctx->SetRef("//New Project");
        ctx->ComboClick("Template/ballgame");
        ctx->ItemInputValue("Project Name", "UiGame");
        ctx->ItemInputValue("Location", g_host->new_project_location.string().c_str());
        ctx->ItemClick("Create");
        for (int frame = 0; frame < 10 && g_host->opened_project.empty(); ++frame) ctx->Yield();
        IM_CHECK(g_host->opened_project == g_host->new_project_location / "UiGame");
    };
}

void Run(const std::filesystem::path &project) {
    // Templates resolve from the repository root, as when the editor runs there.
    std::filesystem::current_path(project.parent_path().parent_path().parent_path());
    const std::filesystem::path new_project_location =
        std::filesystem::temp_directory_path() / ("pocket2d-editor-ui-" + std::to_string(SDL_GetTicksNS()));
    std::filesystem::create_directories(new_project_location);
    ResourcePath::SetResourcesRootPath(project);
    Engine edit;
    edit.InitializeRuntime();
    SDL_HideWindow(edit.GetWindow());
    edit.SetRenderRuntimeToTexture(true);
    edit.SetRuntimeRenderTargetSize(320, 240);
    EditorSceneSession session;
    session.LoadInitialScene(edit);
    session.SyncRuntimeMirrorIfDirty(edit);
    Host host{&edit, &session};
    host.new_project_location = new_project_location;
    g_host = &host;

    EditorOverlay overlay;
    Check(overlay.Initialize(edit.GetWindow(), edit.GetRenderer()), "cannot initialize the editor overlay");
    ImGui::GetIO().IniFilename = nullptr;
    EditorConfigData config;
    config.window_width = config.reference_width = 960;
    config.window_height = config.reference_height = 720;
    config.scene_view_width = 320;
    config.scene_view_height = 240;
    config.current_project_root = project;
    overlay.ApplyConfig(config, 960, 720);

    ImGuiTestEngine *engine = ImGuiTestEngine_CreateContext();
    ImGuiTestEngineIO &test_io = ImGuiTestEngine_GetIO(engine);
    test_io.ConfigVerboseLevel = ImGuiTestVerboseLevel_Warning;
    test_io.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
    test_io.ConfigRunSpeed = ImGuiTestRunSpeed_Fast;
    test_io.ConfigNoThrottle = true;
    test_io.ConfigLogToTTY = true;
    ImGuiTestEngine_Start(engine, ImGui::GetCurrentContext());
    RegisterTests(engine);
    ImGuiTestEngine_QueueTests(engine, ImGuiTestGroup_Tests, "editor");

    const int edit_frame = FrameClock::GetFrameNumber();
    for (int frame = 0; frame < 5000 && !ImGuiTestEngine_IsTestQueueEmpty(engine); ++frame) {
        if (host.Playing()) {
            host.play->RunSingleFrame(false);
        } else {
            edit.RunRenderFrozenFrame(false, false);
        }
        const auto result = overlay.Render(host.Playing() ? *host.play : edit, session.GetSceneDocument(), config,
                                           false, host.Playing(), false, false, !host.Playing(), true);
        if (result.play_mode_start_requested && !host.Playing()) host.Start();
        else if (result.play_mode_stop_requested && host.Playing()) host.Stop();
        if (result.open_project_requested) host.opened_project = result.requested_project_root;
        (host.Playing() ? *host.play : edit).PresentFrame(host.Playing());
        ImGuiTestEngine_PostSwap(engine);
    }
    Check(ImGuiTestEngine_IsTestQueueEmpty(engine), "editor UI tests did not finish");
    int tested = 0, succeeded = 0;
    ImGuiTestEngine_GetResult(engine, tested, succeeded);
    ImGuiTestEngine_Stop(engine);
    overlay.Shutdown();
    ImGuiTestEngine_DestroyContext(engine);
    if (host.Playing()) host.Stop();
    Check(tested == 4 && succeeded == 4, "editor UI tests: " + std::to_string(succeeded) + "/" + std::to_string(tested) + " passed");
    Check(host.starts == 3 && host.stops == 3, "each play test starts and stops play once");
    const std::filesystem::path created = new_project_location / "UiGame";
    Check(std::filesystem::exists(created / "game.config") &&
              std::filesystem::exists(created / "scenes") &&
              !std::filesystem::exists(created / ".pocketengine"),
          "New Project copies the ballgame template without its editor state");
    Check(config.new_project_location == new_project_location, "New Project remembers its location");
    std::filesystem::remove_all(new_project_location);
    WorldScope scope(edit.GetWorld());
    Check(FrameClock::GetFrameNumber() == edit_frame, "playing advanced the edit World");
}
} // namespace

int main(int argc, char **argv) {
    try {
        Check(argc == 2, "usage: editor_ui_test FIXTURE_PROJECT");
        SDL_setenv_unsafe("SDL_VIDEO_DRIVER", "dummy", 1);
        SDL_setenv_unsafe("SDL_AUDIO_DRIVER", "dummy", 1);
        SDL_setenv_unsafe("POCKET_RENDER_DRIVER", "software", 1);
        SDL_setenv_unsafe("POCKET_RENDER_VSYNC", "0", 1);
        Run(std::filesystem::absolute(argv[1]));
        std::cout << "editor_ui_test: viewport buttons, Runtime menu and F5 start and stop play; New Project copies a template\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "editor_ui_test: " << error.what() << '\n';
        return 1;
    }
}
