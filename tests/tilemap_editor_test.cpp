#include "core/Engine.h"
#include "core/FrameClock.h"
#include "editor/documents/SceneDocument.h"
#include "editor/documents/TilemapDocument.h"
#include "editor/panels/InspectorPanel.h"
#include "shared/resources/ResourcePath.h"
#include "shared/scene_format/SceneFormat.h"
#include "imgui.h"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value,const std::string &message){if(!value)throw std::runtime_error(message);}
struct Project {
    std::filesystem::path previous=ResourcePath::ResourcesRootPath();
    std::filesystem::path path;
    explicit Project(const std::filesystem::path &fixture) {
        path=std::filesystem::temp_directory_path()/
            ("pocket-tilemap-inspector-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::copy(fixture,path,std::filesystem::copy_options::recursive);
        std::filesystem::copy_file(path/"maps/farm.tilemap.json",path/"maps/other.tilemap.json");
        ResourcePath::SetResourcesRootPath(path);
        auto scene=SceneFormat::LoadSceneAsset("farm");
        auto other=scene.actors.front();other.uid=11;other.name="other_terrain";
        auto *map=SceneFormat::FindComponentSpec(other.component_specs,"map");
        Check(map!=nullptr,"fixture map component missing");
        SceneFormat::UpsertComponentProperty(*map,"map_resource",std::string("other.tilemap.json"));
        scene.actors.push_back(std::move(other));
        Check(SceneFormat::SaveSceneAsset(scene),"could not prepare inspector scene");
    }
    ~Project(){ResourcePath::SetResourcesRootPath(previous);std::error_code error;std::filesystem::remove_all(path,error);}
};
struct ImGuiScope {
    ImGuiScope() {
        ImGui::CreateContext();
        auto &io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize=ImVec2(960,640);io.DeltaTime=1.0f/60.0f;
        unsigned char *pixels=nullptr;int width=0,height=0;
        io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
        Check(pixels!=nullptr && width>0 && height>0,"ImGui font atlas did not initialize");
        io.Fonts->SetTexID(static_cast<ImTextureID>(1));
    }
    ~ImGuiScope(){ImGui::DestroyContext();}
};
void Draw(const Engine &engine,SceneDocument &scene,TilemapDocument &map,int selected,bool play=false) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(ImVec2(940,620));
    const bool scene_changed=EditorPanels::RenderInspectorPanel(engine,scene,selected,
        scene.GetActorUID(static_cast<std::size_t>(selected)),play,!play,nullptr,&map);
    ImGui::Render();
    Check(!scene_changed,"drawing inspector silently modified the scene");
    Check(ImGui::GetDrawData()!=nullptr && ImGui::GetDrawData()->TotalVtxCount>0,
          "inspector did not create draw data");
    Check(FrameClock::GetFrameNumber()==0,"editor inspector advanced gameplay");
}
void Run(const std::filesystem::path &fixture) {
    Project project(fixture);
    Engine engine;Check(engine.GetLastSceneLoadError().empty(),engine.GetLastSceneLoadError());
    engine.InitializeHeadless();
    SceneDocument scene;Check(scene.LoadFromSceneName("farm"),"editor scene load failed");
    TilemapDocument map;
    ImGuiScope gui;
    Draw(engine,scene,map,0);
    Check(map.HasAsset() && map.GetPath()==project.path/"maps/farm.tilemap.json",
          "inspector did not open the selected Tilemap resource");
    const auto initial_revision=map.GetRevision();
    Draw(engine,scene,map,0);
    Check(map.GetRevision()==initial_revision,"inspector reloaded a stable document every frame");
    Check(map.SetCell("ground",8,8,3),"could not edit authoring map");
    Draw(engine,scene,map,0,true);
    Check(map.HasUnsavedChanges(),"play-mode inspector discarded authoring edits");
    Check(TilemapAsset::LoadFile(project.path/"maps/farm.tilemap.json",project.path/"images").ground.cells[8*64+8]==2,
          "play-mode inspector saved runtime/authoring changes implicitly");
    Draw(engine,scene,map,static_cast<int>(scene.GetActorCount()-1));
    Check(map.HasUnsavedChanges() && map.GetPath()==project.path/"maps/farm.tilemap.json",
          "selecting another map bypassed the unsaved-edit guard");
    Check(map.Revert(),map.GetLastError());
    Draw(engine,scene,map,static_cast<int>(scene.GetActorCount()-1));
    Check(map.GetPath()==project.path/"maps/other.tilemap.json" && !map.HasUnsavedChanges(),
          "explicit discard did not allow the inspector to switch maps");
}
}
int main(int argc,char **argv) {
    try {
        Check(argc==2,"usage: tilemap_editor_test FIXTURE_PROJECT");
        Run(std::filesystem::absolute(argv[1]));
        std::cout<<"tilemap_editor_test: ImGui inspector resource binding and dirty-switch guard passed\n";
        return 0;
    } catch(const std::exception &error) {
        std::cerr<<"tilemap_editor_test: "<<error.what()<<'\n';return 1;
    }
}
