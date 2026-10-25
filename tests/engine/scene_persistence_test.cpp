#include "core/Engine.h"
#include "core/FrameClock.h"
#include "physics/Rigidbody.h"
#include "scene/ActorHandle.h"
#include "scene/Scene.h"
#include "scripting/ComponentManager.h"
#include "shared/resources/ResourcePath.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value,const std::string &message){if(!value)throw std::runtime_error(message);}
void Write(const std::filesystem::path &path,const std::string &text){std::filesystem::create_directories(path.parent_path());std::ofstream file(path);file<<text;Check(file.good(),"temporary fixture write failed");}
struct Fixture {
 std::filesystem::path previous=ResourcePath::ResourcesRootPath(),root;
 Fixture(){
  root=std::filesystem::temp_directory_path()/("pocket-scene-persistence-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  ResourcePath::SetResourcesRootPath(root);
  Write(root/"game.config",R"({"initial_scene":"main"})");
  Write(root/"component_types/Probe.lua",R"lua(
ROOT_LOADS=(ROOT_LOADS or 0)+1
Probe={starts=0,updates=0,version=1,
 OnStart=function(self) self.starts=self.starts+1;self.memory={coins=7} end,
 OnUpdate=function(self) self.updates=self.updates+1 end,
 OnDestroy=function(self) DESTROYS=(DESTROYS or 0)+1 end}
)lua");
  Write(root/"scenes/main.scene",R"({"actors":[
   {"uid":1,"name":"parent","components":{"probe":{"type":"Probe"},"body":{"type":"Rigidbody","gravity_scale":0,"has_collider":false,"has_trigger":false},"transform":{"type":"Transform"}}},
   {"uid":2,"parent_uid":1,"name":"child","components":{"probe":{"type":"Probe"},"body":{"type":"Rigidbody","body_type":"kinematic","x":1,"gravity_scale":0,"has_collider":false,"has_trigger":false},"transform":{"type":"Transform","x":1}}},
   {"uid":3,"name":"victim","components":{"probe":{"type":"Probe"},"transform":{"type":"Transform","x":5}}}]})");
  Write(root/"scenes/valid.scene",R"({"actors":[{"uid":10,"name":"incoming","components":{"probe":{"type":"Probe"},"transform":{"type":"Transform","x":8}}}]})");
  Write(root/"scenes/collision.scene",R"({"actors":[{"uid":1,"name":"collision","components":{"probe":{"type":"Probe"}}}]})");
  Write(root/"scenes/cross_parent.scene",R"({"actors":[{"uid":11,"parent_uid":1,"name":"cross_scene_child","components":{"probe":{"type":"Probe"}}}]})");
  Write(root/"scenes/zone/variant.scene",R"({"actors":[{"uid":20,"name":"variant","components":{"probe":{"type":"Probe"}}}]})");
 }
 ~Fixture(){ResourcePath::SetResourcesRootPath(previous);std::error_code error;std::filesystem::remove_all(root,error);}
};
auto Ref(Actor::UID uid){return ComponentManager::GetComponentByKey(uid,"probe");}
int Field(Actor::UID uid,const char *name){return Ref(uid)[name].cast<int>().value();}
int Global(lua_State *state,const char *name){auto value=luabridge::getGlobal(state,name);return value.isNil()?0:value.cast<int>().value();}
void Run(){
 Fixture fixture;Engine engine;Check(engine.GetLastSceneLoadError().empty(),engine.GetLastSceneLoadError());engine.InitializeHeadless();engine.StepSimulation();
 const ActorHandle parent(engine.GetRuntimeActorByUID(1)),child(engine.GetRuntimeActorByUID(2)),victim(engine.GetRuntimeActorByUID(3));
 Check(parent.IsValid()&&child.IsValid()&&victim.IsValid(),"initial actor handles missing");
 const auto parent_ref=Ref(1),child_ref=Ref(2);const luabridge::LuaRef child_memory=child_ref["memory"];
 auto *parent_body=ComponentManager::GetComponentByKey(1,"body").cast<Rigidbody *>().value();
 auto *child_body=ComponentManager::GetComponentByKey(2,"body").cast<Rigidbody *>().value();
 auto *state=parent_ref.state();
 Check(Global(state,"ROOT_LOADS")==1 && Field(1,"starts")==1 && Field(2,"starts")==1,"startup did not execute/start once");
 parent_body->SetVelocity(Vector2(1,0));
 const auto original_source=ComponentManager::GetComponentScriptSource("Probe");
 const auto original_directory=Scene::GetActiveSceneSubdirectory();
 auto retained_identity=[&]{
  Check(parent.IsValid()&&child.IsValid(),"persistent handle expired");
  Check(parent.Resolve()==engine.GetRuntimeActorByUID(1)&&child.Resolve()==engine.GetRuntimeActorByUID(2),"persistent UID was rebound or renumbered");
  Check(child.Resolve()->parent_uid==1,"persistent parent edge changed");
  Check(parent_ref.rawequal(Ref(1))&&child_ref.rawequal(Ref(2))&&child_memory.rawequal(Ref(2)["memory"]),"persistent Lua instance/nested data replaced");
  Check(parent_body==ComponentManager::GetComponentByKey(1,"body").cast<Rigidbody *>().value() &&
        child_body==ComponentManager::GetComponentByKey(2,"body").cast<Rigidbody *>().value(),"persistent physics component replaced");
  Check(Field(1,"starts")==1&&Field(2,"starts")==1,"persistent OnStart repeated");
  Check(Global(state,"ROOT_LOADS")==1,"scene load re-executed old source");
 };
 auto rejected=[&](const char *scene){
  const int frame=FrameClock::GetFrameNumber(),updates=Field(1,"updates");
  const auto attempt=engine.GetSceneLoadAttemptRevision();const auto old_scene=engine.GetCurrentSceneName();
  const auto count=engine.GetActorCount();const int destroys=Global(state,"DESTROYS");const float x=parent_body->GetPosition().x;
  engine.RequestSceneLoad(scene);engine.StepSimulation();
  Check(engine.GetSceneLoadAttemptRevision()==attempt+1&&!engine.GetLastSceneLoadError().empty()&&!engine.RequiresSceneReset(),"invalid persistent merge not rejected recoverably");
  Check(engine.GetCurrentSceneName()==old_scene&&engine.GetActorCount()==count&&Global(state,"DESTROYS")==destroys,"rejected merge destroyed/replaced actors");
  Check(FrameClock::GetFrameNumber()==frame+1&&Field(1,"updates")==updates+1,"rejected queued load did not run exactly one normal old tick");
  Check(std::abs(parent_body->GetPosition().x-x-1.0f/60.0f)<0.00001f,"rejected merge changed body beyond normal movement");
  retained_identity();
 };
 engine.MarkActorDontDestroy(child.Resolve());
 rejected("valid");Check(victim.IsValid(),"dangling-parent rejection destroyed victim");
 engine.MarkActorDontDestroy(parent.Resolve());
 rejected("collision");Check(victim.IsValid(),"UID collision rejection destroyed victim");
 rejected("cross_parent");Check(victim.IsValid(),"incoming authored parent linked to retained actor");
 const auto attempt=engine.GetSceneLoadAttemptRevision();const int frame=FrameClock::GetFrameNumber();
 engine.RequestSceneLoad("valid");engine.StepSimulation();
 Check(engine.GetSceneLoadAttemptRevision()==attempt+1&&engine.GetLastSceneLoadError().empty()&&engine.GetCurrentSceneName()=="valid","valid persistent merge rejected");
 Check(FrameClock::GetFrameNumber()==frame+1&&engine.GetActorCount()==3&&!victim.IsValid(),"valid merge did not replace only nonpersistent actors");
 retained_identity();Check(Global(state,"DESTROYS")==1&&Field(10,"starts")==1&&Field(10,"updates")==1,"incoming lifecycle or outgoing destruction count wrong");
 engine.StepSimulation();Check(Field(10,"starts")==1&&Field(10,"updates")==2,"new actor started more than once");
 // Newly discovered variant source must not replace a prototype still held by
 // persistent actors. The data-only scene utility must not even execute it.
 Write(fixture.root/"component_types/zone/Probe.lua",R"lua(Probe={starts=0,updates=0,version=7,OnStart=function(self) self.starts=self.starts+1 end,OnUpdate=function(self) self.updates=self.updates+1 end})lua");
 Write(fixture.root/"component_types/zone/FreshReadOnly.lua","DATA_ONLY_EXECUTIONS=(DATA_ONLY_EXECUTIONS or 0)+1;error('data-only load must not execute scripts')");
 Write(fixture.root/"scenes/zone/data.scene",R"({"actors":[{"uid":30,"name":"data_only","components":{"probe":{"type":"FreshReadOnly"}}}]})");
 const auto utility_attempt=engine.GetSceneLoadAttemptRevision();const auto utility_frame=FrameClock::GetFrameNumber();
 const auto records=Scene::LoadScene("zone/data");
 Check(records.size()==1&&records.front().uid==30&&records.front().actor_name=="data_only","data-only scene reader lost authored records");
 Check(Scene::GetActiveSceneSubdirectory()==original_directory&&ComponentManager::GetComponentScriptSource("Probe")==original_source,
       "data-only load changed active subdirectory/source preference");
 Check(!ComponentManager::IsRegisteredComponentType("FreshReadOnly")&&Global(state,"DATA_ONLY_EXECUTIONS")==0,
       "data-only load registered or executed a target script");
 Check(engine.GetSceneLoadAttemptRevision()==utility_attempt&&FrameClock::GetFrameNumber()==utility_frame,"data-only utility performed a runtime attempt/tick");
 std::filesystem::remove(fixture.root/"component_types/zone/FreshReadOnly.lua");
 rejected("zone/variant");
 Check(Scene::GetActiveSceneSubdirectory()==original_directory&&ComponentManager::GetComponentScriptSource("Probe")==original_source,
       "rejected variant changed retained prototype source/cache");
 Check(luabridge::getGlobal(state,"Probe")["version"].cast<int>().value()==1&&Field(1,"version")==1&&Field(2,"version")==1,
       "rejected variant changed global or retained prototype values");
 parent.Resolve()->dont_destroy_on_scene_load=false;child.Resolve()->dont_destroy_on_scene_load=false;
 engine.RequestSceneLoad("zone/variant");engine.StepSimulation();
 Check(engine.GetLastSceneLoadError().empty()&&!engine.RequiresSceneReset(),"valid variant rejected after persistence flags cleared");
 Check(!parent.IsValid()&&!child.IsValid()&&engine.GetRuntimeActorByUID(1)==nullptr&&engine.GetRuntimeActorByUID(2)==nullptr,
       "cleared persistence retained old generations");
 Check(engine.GetActorCount()==1&&Field(20,"starts")==1&&Field(20,"updates")==1&&Field(20,"version")==7,"variant new actor did not use new prototype exactly once");
 Check(Scene::GetActiveSceneSubdirectory()==std::filesystem::path("zone")&&ComponentManager::GetComponentScriptSource("Probe")!=original_source,
       "successful variant did not commit new source preference");
 Check(Global(state,"ROOT_LOADS")==1&&Global(state,"DESTROYS")==4,"valid variant reran root source or lost teardown counts");
 Check(ComponentManager::GetRuntimeErrorCount()==0,ComponentManager::GetLastRuntimeError());
}
}
int main(){try{Run();std::cout<<"scene_persistence_test: parent/UID merge rejection, retained identity, variant isolation and data-only scene loading passed\n";return 0;}
catch(const std::exception &error){std::cerr<<"scene_persistence_test: "<<error.what()<<'\n';return 1;}}
