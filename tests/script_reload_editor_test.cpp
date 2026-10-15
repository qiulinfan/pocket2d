#include "core/Engine.h"
#include "core/FrameClock.h"
#include "editor/core/EditorOverlay.h"
#include "editor/documents/SceneDocument.h"
#include "scene/Tilemap.h"
#include "scripting/ComponentManager.h"
#include "shared/resources/ResourcePath.h"
#include "shared/scene_format/SceneFormat.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <SDL3_image/SDL_image.h>
#include <memory>
#include "imgui.h"
#include "imgui_internal.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
void Check(bool value,const std::string &message){if(!value)throw std::runtime_error(message);}
std::string Bytes(const std::filesystem::path &path){std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),{}};}
void Write(const std::filesystem::path &path,const std::string &text){std::ofstream file(path);file<<text;Check(file.good(),"temporary source write failed");}
std::string Script(int increment){return "EditorProbe={starts=0,updates=0,health=9,OnStart=function(self) self.starts=self.starts+1;self.health=2;self.memory={value=7} end,OnUpdate=function(self) self.updates=self.updates+"+std::to_string(increment)+" end}\n";}
struct Project {
 std::filesystem::path old_root=ResourcePath::ResourcesRootPath(),old_cwd=std::filesystem::current_path(),path;
 explicit Project(const std::filesystem::path &fixture){
  path=std::filesystem::temp_directory_path()/("pocket-reload-overlay-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::copy(fixture,path,std::filesystem::copy_options::recursive);
  std::filesystem::current_path(path);ResourcePath::SetResourcesRootPath(path);
  Write(path/"component_types/EditorProbe.lua",Script(1));
  auto scene=SceneFormat::LoadSceneAsset("farm");SceneFormat::ActorRecord actor;
  actor.uid=60;actor.name="editor_reload_probe";
  Actor::ComponentSpec script;script.key="probe";script.type="EditorProbe";actor.component_specs.push_back(script);
  scene.actors.push_back(actor);Check(SceneFormat::SaveSceneAsset(scene),"fixture scene write failed");
 }
 ~Project(){std::filesystem::current_path(old_cwd);ResourcePath::SetResourcesRootPath(old_root);std::error_code ec;std::filesystem::remove_all(path,ec);}
};
int Value(const char *field){return ComponentManager::GetComponentByKey(60,"probe")[field].cast<int>().value();}
EditorOverlayResult Draw(EditorOverlay &overlay,Engine &engine,SceneDocument &scene,EditorConfigData &config,bool paused=false){
 const int frame=FrameClock::GetFrameNumber();
 if(paused)engine.RunPresentPausedFrame(false);else engine.RunRenderFrozenFrame(false,false);
 const auto result=overlay.Render(engine,scene,config,false,paused,paused,false,!paused,!paused);
 Check(FrameClock::GetFrameNumber()==frame,"editor reload render advanced gameplay");
 Check(!result.scene_changed && !result.save_scene_requested && !result.play_mode_start_requested &&
       !result.play_mode_stop_requested && !result.play_mode_pause_toggle_requested,
       "reload UI changed scene save/mirror/play state");
 Check(ImGui::GetDrawData()!=nullptr && ImGui::GetDrawData()->TotalVtxCount>0,"reload UI did not render");
 return result;
}
void ClickReload(EditorOverlay &overlay,Engine &engine,SceneDocument &scene,EditorConfigData &config,bool paused=false){
 Draw(overlay,engine,scene,config,paused);
 auto *window=ImGui::FindWindowByName("Reload Lua Scripts");
 Check(window && window->Active && !window->Hidden && !window->Collapsed,"reload window is not accessible");
 ImGui::FocusWindow(window);
 const auto center=ImVec2(window->DC.CursorStartPos.x+110,window->DC.CursorStartPos.y+ImGui::GetFrameHeight()*0.5f);
 auto &io=ImGui::GetIO();io.AddMousePosEvent(center.x,center.y);io.AddMouseButtonEvent(ImGuiMouseButton_Left,true);
 Draw(overlay,engine,scene,config,paused);
 io.AddMousePosEvent(center.x,center.y);io.AddMouseButtonEvent(ImGuiMouseButton_Left,false);
 Draw(overlay,engine,scene,config,paused);
 Check(overlay.HasScriptReloadResult(),"actual ImGui reload button did not execute the shared handler");
}
void Run(const std::filesystem::path &fixture,const std::filesystem::path &capture){
 Project project(fixture);
 SDL_setenv_unsafe("SDL_VIDEO_DRIVER","dummy",1);SDL_setenv_unsafe("SDL_AUDIO_DRIVER","dummy",1);
 SDL_setenv_unsafe("POCKET_RENDER_DRIVER","software",1);SDL_setenv_unsafe("POCKET_RENDER_VSYNC","0",1);
 Engine engine;Check(engine.GetLastSceneLoadError().empty(),engine.GetLastSceneLoadError());
 engine.InitializeRuntime();SDL_HideWindow(engine.GetWindow());engine.SetRenderRuntimeToTexture(true);engine.SetRuntimeRenderTargetSize(320,240);
 Check(std::string(SDL_GetRendererName(engine.GetRenderer()))=="software","software renderer not selected");
 engine.StepSimulation();Check(Value("starts")==1 && Value("updates")==1,"probe did not actually start and update");
 SceneDocument scene;Check(scene.LoadFromSceneName("farm"),"scene document failed to load");
 const auto scene_bytes=Bytes(project.path/"scenes/farm.scene"),map_bytes=Bytes(project.path/"maps/farm.tilemap.json");
 Check(scene.SetActorName(0,"unsaved_terrain_label"),"could not create scene authoring draft");
 EditorOverlay overlay;Check(overlay.Initialize(engine.GetWindow(),engine.GetRenderer()),"Overlay initialization failed");
 ImGui::GetIO().IniFilename=nullptr;
 EditorConfigData config;config.window_width=960;config.window_height=720;config.reference_width=960;config.reference_height=720;
 config.scene_view_width=320;config.scene_view_height=240;config.current_project_root=project.path;
 overlay.ApplyConfig(config,960,720);
 auto &map_document=overlay.GetTilemapDocument();Check(map_document.Open(project.path/"maps/farm.tilemap.json"),map_document.GetLastError());
 Check(map_document.SetCell("ground",8,8,3),"could not create map draft");
 overlay.ShowScriptReloadWindow();overlay.SetScriptReloadSelection({"EditorProbe"});
 Draw(overlay,engine,scene,config);
 const auto generation=engine.GetRuntimeActorByUID(60)->runtime_generation;
 auto reference=ComponentManager::GetComponentByKey(60,"probe");
 const int frame=FrameClock::GetFrameNumber();
 auto invariant=[&]{
  Check(FrameClock::GetFrameNumber()==frame,"reload advanced frame");
  Check(Value("starts")==1 && Value("health")==2,"reload restarted component or reset live health");
  Check(engine.GetRuntimeActorByUID(60)->runtime_generation==generation,"reload replaced actor generation");
  Check(reference.rawequal(ComponentManager::GetComponentByKey(60,"probe")),"reload replaced the live component table");
  Check(scene.IsDirty() && scene.GetActorDisplayName(0)=="unsaved_terrain_label","reload discarded scene authoring draft");
  Check(overlay.HasUnsavedMapChanges() && map_document.GetAsset().ground.cells[8*64+8]==3,"reload discarded map authoring draft");
  Check(Bytes(project.path/"scenes/farm.scene")==scene_bytes && Bytes(project.path/"maps/farm.tilemap.json")==map_bytes,"reload wrote an authoring draft to disk");
 };
 Write(project.path/"component_types/EditorProbe.lua",Script(10));
 ClickReload(overlay,engine,scene,config);
 Check(overlay.GetLastScriptReloadResult().ok && overlay.GetLastScriptReloadResult().instances_updated==1,"UI reload failed");invariant();
 Write(project.path/"component_types/EditorProbe.lua","EditorProbe={");
 const auto failure=overlay.ExecuteScriptReload({"EditorProbe"});
 Check(!failure.ok && !failure.diagnostics.empty(),"editor omitted syntax diagnostics");
 for(int i=0;i<3;++i)Draw(overlay,engine,scene,config,true);
 Check(!overlay.GetLastScriptReloadResult().ok && !overlay.GetLastScriptReloadResult().diagnostics.empty(),"editor diagnostics expired or vanished during pause");
 invariant();
 if(!capture.empty()){
  std::filesystem::create_directories(capture.parent_path());
  std::unique_ptr<SDL_Surface,decltype(&SDL_DestroySurface)> surface(SDL_RenderReadPixels(engine.GetRenderer(),nullptr),SDL_DestroySurface);
  Check(surface!=nullptr,"editor diagnostic screenshot readback failed");
  Check(IMG_SavePNG(surface.get(),capture.string().c_str()),"editor diagnostic screenshot save failed");
 }

 Write(project.path/"component_types/EditorProbe.lua",Script(20));
 ClickReload(overlay,engine,scene,config,true);
 Check(overlay.GetLastScriptReloadResult().ok,"retry through actual paused-editor button failed");invariant();
 engine.StepSimulation();Check(Value("updates")==21 && Value("starts")==1,"reloaded method did not take effect when gameplay resumed");
 Check(ComponentManager::GetRuntimeErrorCount()==0,ComponentManager::GetLastRuntimeError());
 overlay.NotifyProjectChanged();Check(!overlay.HasScriptReloadResult(),"new project retained old reload diagnostics");
 overlay.Shutdown();
}
}
int main(int argc,char **argv){try{Check(argc==2 || argc==3,"usage: script_reload_editor_test TILEMAP_FIXTURE [CAPTURE_PNG]");Run(std::filesystem::absolute(argv[1]),argc==3?std::filesystem::absolute(argv[2]):std::filesystem::path{});
 std::cout<<"script_reload_editor_test: real UI reload, persistent diagnostics, pause and authoring draft preservation passed\n";return 0;
}catch(const std::exception &error){std::cerr<<"script_reload_editor_test: "<<error.what()<<'\n';return 1;}}
