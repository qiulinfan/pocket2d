// Editor UI driven by Dear ImGui Test Engine: real clicks on the viewport
// transport buttons, the Runtime menu and the F5 shortcut start and stop a
// separate play World exactly as the editor host does, and File > New Project
// copies a template into a chosen location. With --tilemap, real drags in the
// Scene view paint a 1000x400 map's extra layer and autotile terrain.
#include "core/Engine.h"
#include "core/FrameClock.h"
#include "core/World.h"
#include "editor/core/EditorOverlay.h"
#include "editor/core/EditorSceneSession.h"
#include "scene/Tilemap.h"
#include "scripting/ComponentManager.h"
#include "shared/resources/ResourcePath.h"
#include "shared/scene_format/SceneFormat.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
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
        // The default dock layout puts the Viewport behind the Scene tab.
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
    // A fresh folder per run (SDL's tick counter is still 0 before SDL starts,
    // so it cannot name one): a failed run's leftovers never block the next.
    const std::filesystem::path new_project_location =
        std::filesystem::temp_directory_path() /
        ("pocket2d-editor-ui-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
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

// --tilemap: a world-sized map with an extra layer and two autotile terrains.
constexpr int kMapWidth = 1000, kMapHeight = 400;
constexpr Actor::UID kMapActor = 10;
struct MapHost {
    EditorOverlay *overlay = nullptr;
    Engine *edit = nullptr;
    bool capture_requested = false;  // the host saves the next frame
    Tilemap *Map() const { return ComponentManager::GetTilemap(kMapActor, "map"); }
};
MapHost *g_map = nullptr;

TilemapAsset::Data MakeWorldMap() {
    TilemapAsset::Data data;
    data.width = kMapWidth;
    data.height = kMapHeight;
    data.cell_size = 0.16f;
    data.atlas = {"tiles/farm.bmp", 4, 2, 16, 16};
    const std::size_t count = static_cast<std::size_t>(kMapWidth) * kMapHeight;
    data.ground.cells.assign(count, 0);
    data.solid.cells.assign(count, 0);
    data.extra_layers.push_back({"walls", {-50, std::vector<int>(count, 0)}});
    // Terrain "dirt" (any neighbour connects) uses IDs 1-4, "stone" (same
    // terrain only) 5-8; mask m maps to ID (m % 4) + base.
    TilemapAsset::Terrain dirt{"dirt", false, {}}, stone{"stone", true, {}};
    for (int mask = 0; mask < 16; ++mask) {
        dirt.frames[mask] = {mask % 4 + 1};
        stone.frames[mask] = {mask % 4 + 5};
    }
    data.autotile = {false, {dirt, stone}};
    for (int x = 0; x < kMapWidth; ++x) {
        const int surface = 60 + static_cast<int>(8 * std::sin(x / 23.0));
        for (int y = surface; y < kMapHeight; ++y) {
            const auto cell = static_cast<std::size_t>(y) * kMapWidth + x;
            const bool cave = std::hypot((x % 90) - 45, (y % 70) - 35) < 14 && y > surface + 20;
            data.ground.cells[cell] = 7;
            if (!cave) data.solid.cells[cell] = y > 150 + (x % 37) ? 5 : 1;
        }
    }
    TilemapAsset::FrameAll(data);
    return data;
}

// Cells of a layer that differ between two assets.
std::vector<std::pair<int, int>> ChangedCells(const TilemapAsset::Data &a, const TilemapAsset::Data &b, int layer) {
    std::vector<std::pair<int, int>> cells;
    const auto &before = TilemapAsset::LayerAtIndex(a, layer).cells;
    const auto &after = TilemapAsset::LayerAtIndex(b, layer).cells;
    for (std::size_t i = 0; i < before.size(); ++i)
        if (before[i] != after[i]) cells.push_back({static_cast<int>(i % kMapWidth), static_cast<int>(i / kMapWidth)});
    return cells;
}

void SelectMapAndBrush(ImGuiTestContext *ctx, const char *layer) {
    ctx->Yield(10);  // the default dock layout settles over the first frames
    ctx->SetRef("//Hierarchy");
    ctx->ItemClick("**/world");
    ctx->Yield(2);
    // The map tools sit under the Tilemap component's ID scope.
    ctx->SetRef("//Inspector/map");
    ctx->ComboClick((std::string("Layer/") + layer).c_str());
}

// Drags across the Scene canvas between two fractions of its size.
void DragScene(ImGuiTestContext *ctx, ImVec2 from, ImVec2 to) {
    const ImGuiTestItemInfo canvas = ctx->ItemInfo("//Scene/scene_canvas");
    const ImRect rect = canvas.RectFull;
    const auto at = [&](ImVec2 f) { return ImVec2(rect.Min.x + rect.GetWidth() * f.x, rect.Min.y + rect.GetHeight() * f.y); };
    ctx->MouseSetViewport(canvas.Window);
    ctx->MouseMoveToPos(at(from));
    ctx->MouseDown(0);
    ctx->MouseMoveToPos(at(to));
    ctx->MouseUp(0);
    ctx->Yield(2);
}

void RegisterTilemapTests(ImGuiTestEngine *engine) {
    ImGuiTest *test = IM_REGISTER_TEST(engine, "tilemap", "paint_extra_layer_stroke");
    test->TestFunc = [](ImGuiTestContext *ctx) {
        // Test functions run on the engine's coroutine thread.
        WorldScope scope(g_map->edit->GetWorld());
        auto &document = g_map->overlay->GetTilemapDocument();
        SelectMapAndBrush(ctx, "walls");
        IM_CHECK(document.HasAsset() && TilemapAsset::LayerCount(document.GetAsset()) == 3);
        ctx->ItemCheck("Paint in Scene view");
        ctx->ItemInputValue("Tile ID (0 = empty)", 3);
        IM_CHECK(g_map->overlay->GetTilemapBrush().paint && g_map->overlay->GetTilemapBrush().layer == 2);
        const auto before = document.GetAsset();
        g_map->Map()->ResetDiagnostics();
        DragScene(ctx, ImVec2(0.2f, 0.6f), ImVec2(0.8f, 0.6f));
        const auto walls = ChangedCells(before, document.GetAsset(), 2);
        // One fast drag paints a gap-free row on the walls layer only.
        IM_CHECK_GT((int)walls.size(), 3);
        IM_CHECK(ChangedCells(before, document.GetAsset(), 0).empty() && ChangedCells(before, document.GetAsset(), 1).empty());
        for (std::size_t i = 0; i < walls.size(); ++i) {
            IM_CHECK_EQ(walls[i].second, walls[0].second);
            IM_CHECK_EQ(walls[i].first, walls[0].first + (int)i);
        }
        Tilemap *map = g_map->Map();
        for (const auto &[x, y] : walls) IM_CHECK_EQ(*map->GetCell("walls", x, y), 3);
        // Each preview commit during the drag rebuilt only the touched chunks of
        // that layer and no collision; copying the asset would rebuild all
        // 3 x 416 chunk layers.
        const int chunks = (walls.back().first / Tilemap::kChunkSize) - (walls.front().first / Tilemap::kChunkSize) + 1;
        IM_CHECK_LE((int)map->GetDiagnostics().render_chunk_rebuilds, 4 * chunks);
        IM_CHECK_EQ((int)map->GetDiagnostics().collision_chunk_rebuilds, 0);
        // The whole stroke is one undo step.
        ctx->SetRef("//Scene");
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
        ctx->Yield(2);
        IM_CHECK(!document.HasUnsavedChanges() && document.CanRedo() && !document.CanUndo());
        for (const auto &[x, y] : walls) IM_CHECK_EQ(*map->GetCell("walls", x, y), 0);
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z);
        ctx->Yield(2);
        IM_CHECK(document.HasUnsavedChanges());
        for (const auto &[x, y] : walls) IM_CHECK_EQ(*map->GetCell("walls", x, y), 3);
    };
    test = IM_REGISTER_TEST(engine, "tilemap", "terrain_brush_reframes");
    test->TestFunc = [](ImGuiTestContext *ctx) {
        // Test functions run on the engine's coroutine thread.
        WorldScope scope(g_map->edit->GetWorld());
        auto &document = g_map->overlay->GetTilemapDocument();
        SelectMapAndBrush(ctx, "solid (collides)");
        ctx->ItemClick("Terrain (autotile)");
        ctx->ComboClick("Terrain/stone");
        ctx->ItemInputValue("Brush Size", 3);
        const auto &brush = g_map->overlay->GetTilemapBrush();
        IM_CHECK(brush.mode == TilemapBrush::Mode::Terrain && brush.terrain == 1 && brush.layer == 1 && brush.size == 3);
        const auto before = document.GetAsset();
        g_map->Map()->ResetDiagnostics();
        DragScene(ctx, ImVec2(0.3f, 0.25f), ImVec2(0.7f, 0.9f));
        const auto &asset = document.GetAsset();
        const auto solid = ChangedCells(before, asset, 1);
        IM_CHECK_GT((int)solid.size(), 6);
        // Every terrain cell, painted or re-framed neighbour, shows the frame
        // its neighbourhood selects, and the preview matches the document.
        const auto terrain_of = TilemapAsset::TerrainIndex(asset);
        const auto cell = [&](int x, int y) {
            if (x < 0 || y < 0 || x >= kMapWidth || y >= kMapHeight) return 1;
            return asset.solid.cells[static_cast<std::size_t>(y) * kMapWidth + x];
        };
        for (const auto &[x, y] : solid) {
            const int id = cell(x, y);
            IM_CHECK(id > 0 && terrain_of[id] >= 0);
            IM_CHECK_EQ(id, TilemapAsset::FrameTerrainCell(asset, terrain_of, terrain_of[id], x, y, cell));
            IM_CHECK_EQ(*g_map->Map()->GetCell("solid", x, y), id);
        }
        // Collision is rebuilt for the touched chunks only, not the map's 416.
        const auto &diagnostics = g_map->Map()->GetDiagnostics();
        IM_CHECK_GT((int)diagnostics.collision_chunk_rebuilds, 0);
        IM_CHECK_LE((int)diagnostics.collision_chunk_rebuilds, 12);
        // Undo restores the painted cells and every re-framed neighbour.
        ctx->SetRef("//Inspector/map");
        ctx->ItemClick("Undo Map");
        IM_CHECK(ChangedCells(before, document.GetAsset(), 1).empty());
        // Wider strokes on a zoomed-out view for the capture.
        ctx->ItemClick("Redo Map");
        ctx->MouseMove("//Scene/scene_canvas");
        ctx->MouseWheelY(-6.0f);
        ctx->ItemInputValue("//Inspector/map/Brush Size", 6);
        DragScene(ctx, ImVec2(0.15f, 0.8f), ImVec2(0.85f, 0.45f));
        ctx->ItemClick("//Inspector/map/Tile");
        SelectMapAndBrush(ctx, "walls");
        ctx->ItemInputValue("Brush Size", 4);
        DragScene(ctx, ImVec2(0.1f, 0.3f), ImVec2(0.9f, 0.3f));
        ctx->MouseMoveToPos(ImVec2(ctx->ItemInfo("//Scene/scene_canvas").RectFull.GetCenter()));
        g_map->capture_requested = true;
        ctx->Yield(2);
        // For comparison: copying the whole asset into the preview, as every
        // edit did before cell patches.
        const auto start = std::chrono::steady_clock::now();
        g_map->Map()->ReplaceAsset(document.GetAsset());
        g_map->Map()->CommitPending();
        ctx->LogInfo("full asset preview copy: %.2f ms",
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    };
}

void RunTilemap(const std::filesystem::path &fixture, const std::filesystem::path &capture) {
    const std::filesystem::path project = std::filesystem::temp_directory_path() /
        ("pocket2d-editor-ui-map-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    std::filesystem::copy(fixture, project, std::filesystem::copy_options::recursive);
    ResourcePath::SetResourcesRootPath(project);
    TilemapAsset::SaveResource("world.tilemap.json", MakeWorldMap());
    std::ofstream(project / "game.config") << R"({"game_title":"Map Paint","initial_scene":"world"})";
    std::ofstream(project / "scenes/world.scene") << R"({"actors":[{"uid":10,"name":"world","components":{)"
        R"("map":{"type":"Tilemap","map_resource":"world.tilemap.json","enabled":true},)"
        R"("transform":{"type":"Transform","x":-40,"y":-8,"rotation":0}}}]})";
    Engine edit;
    edit.InitializeRuntime();
    if (capture.empty()) SDL_HideWindow(edit.GetWindow());
    SDL_SetWindowSize(edit.GetWindow(), 1280, 800);
    edit.SetRenderRuntimeToTexture(true);
    edit.SetRuntimeRenderTargetSize(320, 240);
    EditorSceneSession session;
    session.LoadInitialScene(edit);
    session.SyncRuntimeMirrorIfDirty(edit);
    Check(ComponentManager::GetTilemap(kMapActor, "map") != nullptr, "world map actor did not load");

    EditorOverlay overlay;
    Check(overlay.Initialize(edit.GetWindow(), edit.GetRenderer()), "cannot initialize the editor overlay");
    ImGui::GetIO().IniFilename = nullptr;
    EditorConfigData config;
    config.window_width = config.reference_width = 1280;
    config.window_height = config.reference_height = 800;
    config.scene_view_width = 320;
    config.scene_view_height = 240;
    config.current_project_root = project;
    overlay.ApplyConfig(config, 1280, 800);
    MapHost host{&overlay, &edit};
    g_map = &host;

    ImGuiTestEngine *engine = ImGuiTestEngine_CreateContext();
    ImGuiTestEngineIO &test_io = ImGuiTestEngine_GetIO(engine);
    test_io.ConfigVerboseLevel = ImGuiTestVerboseLevel_Info;
    test_io.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
    test_io.ConfigRunSpeed = ImGuiTestRunSpeed_Fast;
    test_io.ConfigNoThrottle = true;
    test_io.ConfigLogToTTY = true;
    ImGuiTestEngine_Start(engine, ImGui::GetCurrentContext());
    RegisterTilemapTests(engine);
    ImGuiTestEngine_QueueTests(engine, ImGuiTestGroup_Tests, "tilemap");
    double slowest_frame_ms = 0, idle_ms = 0;
    int idle_frames = 0;
    for (int frame = 0; frame < 5000 && !ImGuiTestEngine_IsTestQueueEmpty(engine); ++frame) {
        const bool painting = ImGui::IsMouseDown(ImGuiMouseButton_Left) && host.overlay->GetTilemapDocument().InStroke();
        const auto start = std::chrono::steady_clock::now();
        edit.RunRenderFrozenFrame(false, false);
        overlay.Render(edit, session.GetSceneDocument(), config, false, false, false, false, true, true);
        if (host.capture_requested && !capture.empty()) {
            host.capture_requested = false;
            std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(SDL_RenderReadPixels(edit.GetRenderer(), nullptr), SDL_DestroySurface);
            Check(surface != nullptr && IMG_SavePNG(surface.get(), capture.string().c_str()), "cannot save the map capture");
        }
        const double frame_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (painting) slowest_frame_ms = std::max(slowest_frame_ms, frame_ms);
        else if (frame > 30) { idle_ms += frame_ms; ++idle_frames; }
        edit.PresentFrame(false);
        ImGuiTestEngine_PostSwap(engine);
    }
    Check(ImGuiTestEngine_IsTestQueueEmpty(engine), "tilemap UI tests did not finish");
    int tested = 0, succeeded = 0;
    ImGuiTestEngine_GetResult(engine, tested, succeeded);
    ImGuiTestEngine_Stop(engine);
    overlay.Shutdown();
    ImGuiTestEngine_DestroyContext(engine);
    std::cout << "editor_ui_test: slowest painting frame on a " << kMapWidth << "x" << kMapHeight
              << " map: " << slowest_frame_ms << " ms (mean other frame "
              << idle_ms / std::max(1, idle_frames) << " ms)\n";
    Check(tested == 2 && succeeded == 2, "tilemap UI tests: " + std::to_string(succeeded) + "/" + std::to_string(tested) + " passed");
    std::error_code error;
    std::filesystem::remove_all(project, error);
}
} // namespace

int main(int argc, char **argv) {
    try {
        const bool tilemap = argc >= 3 && std::string(argv[1]) == "--tilemap";
        Check(argc == 2 || (tilemap && argc <= 4), "usage: editor_ui_test FIXTURE_PROJECT | --tilemap MAP_FIXTURE [CAPTURE.png]");
        // A tilemap capture opens a real window on the native renderer
        // (POCKET_WINDOW_DISPLAY / POCKET_WINDOW_BACKGROUND choose where).
        const bool on_screen = tilemap && argc == 4;
        SDL_setenv_unsafe("SDL_AUDIO_DRIVER", "dummy", 1);
        SDL_setenv_unsafe("POCKET_RENDER_VSYNC", "0", 1);
        if (!on_screen) {
            SDL_setenv_unsafe("SDL_VIDEO_DRIVER", "dummy", 1);
            SDL_setenv_unsafe("POCKET_RENDER_DRIVER", "software", 1);
        }
        if (tilemap) {
            RunTilemap(std::filesystem::absolute(argv[2]), argc == 4 ? std::filesystem::path(argv[3]) : std::filesystem::path());
            std::cout << "editor_ui_test: Scene-view strokes paint an extra layer and autotile terrain, undo as one step\n";
            return 0;
        }
        Run(std::filesystem::absolute(argv[1]));
        std::cout << "editor_ui_test: viewport buttons, Runtime menu and F5 start and stop play; New Project copies a template\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "editor_ui_test: " << error.what() << '\n';
        return 1;
    }
}
