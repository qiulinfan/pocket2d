#include "core/Engine.h"
#include "core/FrameClock.h"
#include "editor/core/EditorOverlay.h"
#include "editor/documents/SceneDocument.h"
#include "scene/Tilemap.h"
#include "scripting/ComponentManager.h"
#include "shared/resources/ResourcePath.h"
#include "shared/scene_format/SceneFormat.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition,const std::string &message){if(!condition)throw std::runtime_error(message);}
struct Project {
    std::filesystem::path previous_root=ResourcePath::ResourcesRootPath();
    std::filesystem::path previous_cwd=std::filesystem::current_path();
    std::filesystem::path path;
    explicit Project(const std::filesystem::path &fixture) {
        path=std::filesystem::temp_directory_path()/
            ("pocket-tilemap-overlay-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::copy(fixture,path,std::filesystem::copy_options::recursive);
        std::filesystem::current_path(path);ResourcePath::SetResourcesRootPath(path);
        auto scene=SceneFormat::LoadSceneAsset("farm");
        auto other=scene.actors.front();other.uid=11;other.name="second_terrain";
        auto *transform=SceneFormat::FindComponentSpec(other.component_specs,"transform");
        Check(transform!=nullptr,"fixture transform missing");
        SceneFormat::UpsertComponentProperty(*transform,"x",40.0);
        SceneFormat::UpsertComponentProperty(*transform,"y",40.0);
        scene.actors.push_back(std::move(other));
        Check(SceneFormat::SaveSceneAsset(scene),"could not write temporary scene");
        std::ofstream(path/"scenes/empty.scene")<<"{\"actors\":[]}\n";
    }
    ~Project(){std::filesystem::current_path(previous_cwd);ResourcePath::SetResourcesRootPath(previous_root);
        std::error_code error;std::filesystem::remove_all(path,error);}
};
Tilemap *Map(Actor::UID uid){return ComponentManager::GetTilemap(uid,"map");}
void Draw(EditorOverlay &overlay,Engine &engine,SceneDocument &scene,EditorConfigData &config) {
    engine.RunRenderFrozenFrame(false,false);
    overlay.Render(engine,scene,config,false,false,false,false,true,true);
    Check(ImGui::GetDrawData()!=nullptr && ImGui::GetDrawData()->TotalVtxCount>0,"real Overlay produced no UI draw data");
    Check(FrameClock::GetFrameNumber()==0,"editor preview advanced gameplay");
}
void Run(const std::filesystem::path &fixture) {
    Project project(fixture);
    SDL_setenv_unsafe("SDL_VIDEO_DRIVER","dummy",1);
    SDL_setenv_unsafe("SDL_AUDIO_DRIVER","dummy",1);
    SDL_setenv_unsafe("POCKET_RENDER_DRIVER","software",1);
    SDL_setenv_unsafe("POCKET_RENDER_VSYNC","0",1);
    Engine engine;Check(engine.GetLastSceneLoadError().empty(),engine.GetLastSceneLoadError());
    engine.InitializeRuntime();SDL_HideWindow(engine.GetWindow());
    Check(std::string(SDL_GetRendererName(engine.GetRenderer()))=="software","test did not use software renderer");
    engine.SetRenderRuntimeToTexture(true);
    engine.SetRuntimeRenderTargetSize(320,240);
    SceneDocument scene;Check(scene.LoadFromSceneName("farm"),"scene document load failed");
    EditorOverlay overlay;Check(overlay.Initialize(engine.GetWindow(),engine.GetRenderer()),"Overlay initialization failed");
    ImGui::GetIO().IniFilename=nullptr;
    EditorConfigData config;config.window_width=960;config.window_height=640;
    config.reference_width=960;config.reference_height=640;config.scene_view_width=320;config.scene_view_height=240;
    config.current_project_root=project.path;
    overlay.ApplyConfig(config,960,640);
    Draw(overlay,engine,scene,config);
    auto &document=overlay.GetTilemapDocument();
    Check(document.Open(project.path/"maps/farm.tilemap.json"),document.GetLastError());
    Check(document.SetCell("ground",8,8,3),"authoring edit failed");
    Draw(overlay,engine,scene,config);
    Check(overlay.HasUnsavedMapChanges(),"Overlay lost dirty map state");
    Check(Map(10)->GetCell("ground",8,8)==3 && Map(11)->GetCell("ground",8,8)==3,
          "Overlay failed to preview every reference to the authoring asset");
    Check(TilemapAsset::LoadResource("farm.tilemap.json").ground.cells[8*64+8]==2,
          "preview implicitly saved authoring changes");
    Check(document.Save(),document.GetLastError());
    Draw(overlay,engine,scene,config);
    const auto old_generation=engine.GetRuntimeActorByUID(10)->runtime_generation;
    auto external=TilemapAsset::LoadResource("farm.tilemap.json");
    external.ground.cells[8*64+8]=4;
    TilemapAsset::SaveResource("farm.tilemap.json",external);
    Check(engine.LoadSceneAsset(scene.GetSceneAsset()),engine.GetLastSceneLoadError());
    Check(engine.GetRuntimeActorByUID(10)->runtime_generation!=old_generation,"scene reload reused actor generation");
    Check(Map(10)->GetCell("ground",8,8)==4,"scene reload did not load changed disk asset");
    Draw(overlay,engine,scene,config);
    Check(Map(10)->GetCell("ground",8,8)==4 && Map(11)->GetCell("ground",8,8)==4,
          "clean cached document overwrote new same-UID runtime asset");
    Check(document.GetAsset().ground.cells[8*64+8]==4 && !document.HasUnsavedChanges(),
          "clean authoring cache did not refresh after runtime reload");
    Check(document.SetCell("ground",8,8,3),"second authoring edit failed");
    Draw(overlay,engine,scene,config);
    Check(scene.LoadFromSceneName("empty"),"empty scene document load failed");
    Check(engine.LoadSceneAsset(scene.GetSceneAsset()),engine.GetLastSceneLoadError());
    Draw(overlay,engine,scene,config);
    auto *draft=ImGui::FindWindowByName("Map Asset Draft");
    Check(draft!=nullptr && draft->Active && !draft->Collapsed && !draft->Hidden,
          "global draft tools are unreachable after switching to a mapless scene");
    Check(overlay.HasUnsavedMapChanges() && document.GetAsset().ground.cells[8*64+8]==3,
          "mapless scene switch lost authoring content");
    Check(std::filesystem::remove(project.path/"maps/farm.tilemap.json"),"could not remove temporary source");
    Check(!document.Revert() && overlay.HasUnsavedMapChanges(),"missing source silently discarded draft");
    Check(document.DiscardChanges(),"in-memory discard failed");
    Draw(overlay,engine,scene,config);
    Check(!overlay.HasUnsavedMapChanges() && !std::filesystem::exists(project.path/"maps/farm.tilemap.json"),
          "discard did not release the exit guard without rewriting the missing source");
    overlay.Shutdown();
}
}
int main(int argc,char **argv) {
    try {Check(argc==2,"usage: tilemap_overlay_test FIXTURE_PROJECT");Run(std::filesystem::absolute(argv[1]));
        std::cout<<"tilemap_overlay_test: real preview, generation reload and mapless draft recovery passed\n";return 0;
    } catch(const std::exception &error){std::cerr<<"tilemap_overlay_test: "<<error.what()<<'\n';return 1;}
}
