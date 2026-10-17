#include "core/Engine.h"
#include "core/FrameClock.h"
#include "editor/documents/SceneDocument.h"
#include "editor/documents/TilemapDocument.h"
#include "editor/panels/InspectorPanel.h"
#include "shared/resources/ResourcePath.h"
#include "shared/scene_format/SceneFormat.h"
#include "scene/Tilemap.h"
#include "imgui.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <tuple>

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
    static TilemapBrush brush;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(ImVec2(940,620));
    const bool scene_changed=EditorPanels::RenderInspectorPanel(engine,scene,selected,
        scene.GetActorUID(static_cast<std::size_t>(selected)),play,!play,nullptr,&map,&brush);
    ImGui::Render();
    Check(!scene_changed,"drawing inspector silently modified the scene");
    Check(ImGui::GetDrawData()!=nullptr && ImGui::GetDrawData()->TotalVtxCount>0,
          "inspector did not create draw data");
    Check(FrameClock::GetFrameNumber()==0,"editor inspector advanced gameplay");
}

// A map with an extra layer and two autotile terrains: "dirt" (any neighbour
// connects) on IDs 1-4 and "ore" (same terrain only) on 5-8; mask m maps to ID
// (m % 4) + base.
TilemapAsset::Data LayeredMap() {
    auto data=TilemapAsset::LoadResource("farm.tilemap.json");
    data.extra_layers.push_back({"walls",{-50,std::vector<int>(data.ground.cells.size(),0)}});
    TilemapAsset::Terrain dirt{"dirt",false,{}},ore{"ore",true,{}};
    for(int mask=0;mask<16;++mask){dirt.frames[mask]={mask%4+1};ore.frames[mask]={mask%4+5};}
    data.autotile={false,{dirt,ore}};
    return data;
}
std::vector<int> AllCells(const TilemapAsset::Data &data) {
    std::vector<int> cells;
    for(int layer=0;layer<TilemapAsset::LayerCount(data);++layer){
        const auto &layer_cells=TilemapAsset::LayerAtIndex(data,layer).cells;
        cells.insert(cells.end(),layer_cells.begin(),layer_cells.end());
    }
    return cells;
}
void DocumentEditing(const std::filesystem::path &project) {
    TilemapAsset::SaveResource("layers.tilemap.json",LayeredMap());
    TilemapDocument map;
    Check(map.Open(project/"maps/layers.tilemap.json"),map.GetLastError());
    const auto saved=map.GetAsset();
    const int width=saved.width;
    // Each layer keeps its own dirty cells; the same index on two layers no longer collides.
    const int ground=saved.ground.cells[9*width+9];
    Check(map.SetCell(0,9,9,ground==3?4:3) && map.SetCell("walls",9,9,2),"layer edits failed");
    Check(map.SetCell(2,9,9,0) && map.HasUnsavedChanges(),"reverting a walls cell hid the ground edit");
    Check(map.SetCell(0,9,9,ground) && !map.HasUnsavedChanges(),"restoring every layer left the map dirty");
    Check(!map.SetCell(3,0,0,1) && !map.GetLastError().empty(),"a layer past the asset was accepted");
    // A brush stroke is one undo step, however many cells and paints it covers.
    TilemapBrush brush;brush.layer=2;brush.tile=6;brush.size=3;
    const auto before_stroke=AllCells(map.GetAsset());
    const auto stroke_revision=map.GetRevision();
    map.BeginStroke();
    Check(map.Paint(brush,20,20) && map.Paint(brush,21,20),"brush paint changed nothing");
    map.EndStroke();
    const auto painted=AllCells(map.GetAsset());
    Check(map.GetAsset().extra_layers[0].layer.cells[19*width+19]==6 && map.GetAsset().extra_layers[0].layer.cells[21*width+22]==6,
          "a 3x3 brush did not cover its square");
    const auto changes=map.ChangesSince(stroke_revision);
    Check(changes && changes->size()==12 && std::all_of(changes->begin(),changes->end(),[](const auto &cell){return cell.layer==2;}),
          "the change log does not list exactly the painted walls cells");
    Check(map.Undo() && AllCells(map.GetAsset())==before_stroke,"one undo did not revert the whole stroke");
    Check(map.Redo() && AllCells(map.GetAsset())==painted,"redo did not replay the stroke");
    brush.tile=99;
    Check(!map.Paint(brush,5,5) && map.GetLastError()=="tile ID is outside this atlas","an out-of-atlas brush painted");
    // Terrain brushes frame the cell and re-frame its neighbours exactly as the
    // runtime Tilemap:SetTerrain does, on solid and on an extra layer.
    Check(map.DiscardChanges(),"discard failed");
    Check(!map.ChangesSince(stroke_revision),"a discard kept a patchable change log");
    Tilemap runtime;runtime.Initialize(map.GetAsset(),0,0);
    const std::vector<std::tuple<int,int,int,int>> placements{
        {1,10,10,0},{1,11,10,0},{1,10,11,0},{1,12,10,1},{1,11,10,-1},{2,30,30,0},{2,31,30,0},{2,30,31,1},{2,0,0,0}};
    const char *names[]{"dirt","ore"};
    map.BeginStroke();
    for(const auto &[layer,x,y,terrain]:placements){
        map.SetTerrain(layer,x,y,terrain);
        runtime.SetTerrain(TilemapAsset::LayerName(map.GetAsset(),layer),x,y,terrain<0?"":names[terrain]);
        Check(map.GetLastError().empty(),map.GetLastError());
    }
    map.EndStroke();
    runtime.CommitPending();
    Check(AllCells(map.GetAsset())==AllCells(runtime.GetAsset()),"document terrain framing differs from the runtime's");
    const int dirt_cell=map.GetAsset().solid.cells[10*width+10];
    Check(dirt_cell>=1 && dirt_cell<=4 && map.GetAsset().extra_layers[0].layer.cells[0]==2,
          "terrain cells were not framed from their neighbours (the corner joins the outside up and left)");
    Check(map.Undo() && AllCells(map.GetAsset())==AllCells(saved),"undo left re-framed neighbours behind");
    runtime.OnDestroy();
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
    DocumentEditing(project.path);
}
}
int main(int argc,char **argv) {
    try {
        Check(argc==2,"usage: tilemap_editor_test FIXTURE_PROJECT");
        Run(std::filesystem::absolute(argv[1]));
        std::cout<<"tilemap_editor_test: inspector resource binding, dirty-switch guard, per-layer dirty cells, strokes and terrain framing passed\n";
        return 0;
    } catch(const std::exception &error) {
        std::cerr<<"tilemap_editor_test: "<<error.what()<<'\n';return 1;
    }
}
