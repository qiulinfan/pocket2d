#include "core/Engine.h"
#include "core/FrameClock.h"
#include "editor/core/EditorSceneSession.h"
#include "physics/Rigidbody.h"
#include "scripting/ComponentManager.h"
#include "scene/ActorHandle.h"
#include "scene/Scene.h"
#include "shared/resources/ResourcePath.h"
#include "shared/scene_format/SceneFormat.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
void Check(bool value,const std::string &message){if(!value)throw std::runtime_error(message);}
void Write(const std::filesystem::path &path,const std::string &text){std::filesystem::create_directories(path.parent_path());std::ofstream stream(path);stream<<text;Check(stream.good(),"write fixture");}
std::string Read(const std::filesystem::path &path){std::ifstream stream(path,std::ios::binary);return {std::istreambuf_iterator<char>(stream),{}};}
constexpr auto script=R"lua(
LOADS=(LOADS or 0)+1
if ARMED then
 local survivor=Actor.Find("survivor")
 if survivor then survivor:GetComponent("Probe").health=1;survivor:GetComponent("Rigidbody"):SetPosition(Vector2(9,9)) end
 local victim=Actor.Find("victim");if victim then Actor.Destroy(victim) end
 Save.Write("unexpected",1,{loads=LOADS});math.random()
end
Probe={starts=0,updates=0,health=3,events=0,loads=0,
 OnStart=function(self)
  self.starts=self.starts+1;ARMED=true
  self.bag=Inventory.Open("scene.items.json"):Create(2);self.bag:Add("herb",3,"")
  Event.Subscribe("scene_ping",self,self.OnPing)
 end,
 OnPing=function(self,event) self.events=self.events+1 end,
 OnUpdate=function(self) self.updates=self.updates+1;self.loads=LOADS end,
 OnDestroy=function(self) DESTROYS=(DESTROYS or 0)+1 end}
)lua";
constexpr auto main_scene=R"({"actors":[
 {"uid":1,"name":"survivor","components":{"probe":{"type":"Probe"},"body":{"type":"Rigidbody","gravity_scale":0,"has_trigger":false},"transform":{"type":"Transform"}}},
 {"uid":2,"name":"victim","components":{"transform":{"type":"Transform","x":3}}}]})";
struct Fixture {
 std::filesystem::path root,previous=ResourcePath::ResourcesRootPath();std::string previous_env;bool had_env=false;
 Fixture(){
  root=std::filesystem::temp_directory_path()/("pocket-scene-transition-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  if(const char *value=SDL_getenv("POCKET_SAVE_ROOT")){had_env=true;previous_env=value;}
  SDL_setenv_unsafe("POCKET_SAVE_ROOT",(root/"saves").string().c_str(),1);ResourcePath::SetResourcesRootPath(root/"project");
  Write(root/"project/game.config",R"({"initial_scene":"main","save_namespace":"scene_transition"})");
  Write(root/"project/component_types/Probe.lua",script);
  Write(root/"project/component_types/Rigidbody.lua","error('native reserved file must never execute')");
  Write(root/"project/items/scene.items.json",R"({"version":1,"items":[{"id":"herb","display_name":"Herb","kind":"material","max_stack":5}]})");
  Write(root/"project/scenes/main.scene",main_scene);
  Write(root/"project/scenes/other.scene",main_scene);
  Write(root/"project/scenes/bad.scene",R"({"actors":[{"uid":99,"name":"candidate","components":{"missing":{"type":"MissingComponent"}}}]})");
  Write(root/"project/scenes/zone/bad.scene",Read(root/"project/scenes/bad.scene"));
 }
 ~Fixture(){ResourcePath::SetResourcesRootPath(previous);if(had_env)SDL_setenv_unsafe("POCKET_SAVE_ROOT",previous_env.c_str(),1);else SDL_unsetenv_unsafe("POCKET_SAVE_ROOT");std::error_code error;std::filesystem::remove_all(root,error);}
};
auto Probe(){return ComponentManager::GetComponentByKey(1,"probe");}
int Value(const char *field){return Probe()[field].cast<int>().value();}
int Global(const char *name){auto value=luabridge::getGlobal(Probe().state(),name);return value.isNil()?0:value.cast<int>().value();}
void Lua(const char *code){auto *state=Probe().state();if(luaL_dostring(state,code)!=LUA_OK){const std::string message=lua_tostring(state,-1);lua_pop(state,1);throw std::runtime_error(message);}}
void TestRejectedAsset(){
 Fixture fixture;Engine engine;Check(engine.GetLastSceneLoadError().empty(),engine.GetLastSceneLoadError());engine.InitializeHeadless();
 Check(Global("LOADS")==1,"trusted startup script executed more than once");
 Check(ComponentManager::GetComponentTypeWarningCount()==0,"reserved native script was executed");
 engine.StepSimulation();Check(Value("starts")==1,"startup lifecycle did not run once");
 Lua("local p=Actor.Find('survivor'):GetComponent('Probe');p.bag:Remove(1,1);p.health=2;math.randomseed(123);EXPECTED=math.random();math.randomseed(123)");
 const auto reference=Probe();const luabridge::LuaRef bag=reference["bag"];
 const ActorHandle handle(engine.GetRuntimeActorByUID(1));const auto generation=engine.GetRuntimeActorByUID(1)->runtime_generation;
 auto *body=ComponentManager::GetComponentByKey(1,"body").cast<Rigidbody *>().value();const auto position=body->GetPosition();
 const auto frame=FrameClock::GetFrameNumber(),updates=Value("updates");const auto revision=engine.GetSceneLoadAttemptRevision();
 const auto subdirectory=Scene::GetActiveSceneSubdirectory();
 int gate_calls=0;
 Check(!engine.LoadSceneAsset(SceneFormat::LoadSceneAsset("zone/bad"),[&]{++gate_calls;return true;}),"missing component scene accepted");
 Check(gate_calls==0,"invalid candidate invoked outgoing-document commit gate");
 Check(Scene::GetActiveSceneSubdirectory()==subdirectory,"failed asset preparation changed active scene subdirectory");
 Check(engine.GetSceneLoadAttemptRevision()==revision+1 && !engine.RequiresSceneReset(),"ordinary rejection attempt/fatal classification wrong");
 Check(FrameClock::GetFrameNumber()==frame && engine.GetCurrentSceneName()=="main","direct failed load advanced frame or scene");
 Check(reference.rawequal(Probe()) && bag.rawequal(Probe()["bag"]),"rejected scene replaced Lua/native inventory state");
 Check(handle.IsValid() && engine.GetRuntimeActorByUID(1)->runtime_generation==generation,"rejected scene invalidated actor identity");
 Check(body==ComponentManager::GetComponentByKey(1,"body").cast<Rigidbody *>().value() && body->GetPosition()==position,"rejected scene mutated body");
 Check(Global("LOADS")==1 && Global("DESTROYS")==0 && Value("health")==2 && Value("starts")==1,"preflight executed old script or lifecycle");
 Check(!std::filesystem::exists(fixture.root/"saves/scene_transition/unexpected.save.json"),"failed scene wrote a save through top-level code");
 Lua("assert(math.random()==EXPECTED,'failed scene consumed RNG');local p=Actor.Find('survivor'):GetComponent('Probe');assert(p.bag:GetSlot(1).count==2);Event.Publish('scene_ping',{})");
 Check(Value("events")==1,"failed scene lost or duplicated the live event subscription");
 const auto failure=engine.GetLastSceneLoadError();engine.StepSimulation();
 Check(Value("updates")==updates+1 && engine.GetRuntimeActorByUID(2)!=nullptr && handle.IsValid(),"failed preflight left deferred destroy work");
 Check(engine.GetLastSceneLoadError()==failure && engine.GetSceneLoadAttemptRevision()==revision+1,"normal tick rewrote attempt history");
 // Cached source identity is deliberate: ordinary scene transitions do not
 // silently reload an edited file at the same registered source path.
 Write(fixture.root/"project/component_types/Probe.lua","Probe={");
 Check(engine.LoadSceneAsset(SceneFormat::LoadSceneAsset("other")),engine.GetLastSceneLoadError());
 Check(Global("LOADS")==1 && !handle.IsValid(),"same-source transition re-executed code or retained replaced generation");
 engine.StepSimulation();Check(Value("starts")==1 && Value("health")==3,"cached source did not initialize new scene normally");
}
int TemplateHealth(const Actor &actor){
 const auto *spec=SceneFormat::FindComponentSpec(actor.component_specs,"probe");
 Check(spec!=nullptr,"template probe specification missing");
 for(const auto &property:spec->overrides)if(property.name=="health")return std::get<int>(property.value);
 throw std::runtime_error("template health override missing");
}
void TestTemplateSnapshots(){
 Fixture fixture;Engine engine;engine.InitializeHeadless();engine.StepSimulation();
 Actor::ClearTemplateCache();
 const auto path=fixture.root/"project/actor_templates/Reusable.template";
 Write(path,R"({"name":"reusable","components":{"probe":{"type":"Probe","health":11}}})");
 Check(TemplateHealth(Actor::LoadTemplate("Reusable"))==11,"could not seed live-spawn template cache");
 Write(path,R"({"name":"reusable","components":{"probe":{"type":"Probe","health":22}}})");
 auto scene=SceneFormat::LoadSceneAsset("other");scene.scene_name="template_batch";scene.actors.clear();
 for(int i=0;i<1000;++i){SceneFormat::ActorRecord record;record.uid=100+i;record.template_name="Reusable";scene.actors.push_back(std::move(record));}
 auto built=SceneFormat::BuildRuntimeActors(scene);Check(built.size()==1000,"template batch actor count changed");
 for(const auto &actor:built)Check(TemplateHealth(actor)==22,"scene preparation used stale live-spawn template content");
 auto *first=SceneFormat::FindComponentSpec(built.front().component_specs,"probe");
 SceneFormat::UpsertComponentProperty(*first,"health",999);
 for(std::size_t i=1;i<built.size();++i)Check(TemplateHealth(built[i])==22,"prepared actors share mutable component specifications");
 Check(TemplateHealth(Actor::LoadTemplate("Reusable"))==11,"scene building changed live-spawn template cache");
 // Each invocation gets a fresh local snapshot, not a new process-wide cache.
 Write(path,R"({"name":"reusable","components":{"probe":{"type":"Probe","health":33}}})");
 auto later=SceneFormat::BuildRuntimeActors(scene);
 for(const auto &actor:later)Check(TemplateHealth(actor)==33,"next scene preparation retained the previous local template snapshot");
 Check(TemplateHealth(Actor::LoadTemplate("Reusable"))==11,"new preparation invalidated live-spawn cache");
 SceneFormat::ActorRecord invalid;invalid.uid=2000;invalid.name="late_missing";
 Actor::ComponentSpec missing;missing.key="missing";missing.type="MissingComponent";invalid.component_specs.push_back(missing);scene.actors.push_back(invalid);
 const auto ref=Probe();const auto generation=engine.GetRuntimeActorByUID(1)->runtime_generation;
 const int frame=FrameClock::GetFrameNumber(),health=Value("health"),updates=Value("updates");
 auto *body=ComponentManager::GetComponentByKey(1,"body").cast<Rigidbody *>().value();const auto position=body->GetPosition();
 Check(!engine.LoadSceneAsset(scene),"template batch with late missing component was accepted");
 Check(FrameClock::GetFrameNumber()==frame&&ref.rawequal(Probe())&&engine.GetRuntimeActorByUID(1)->runtime_generation==generation,
       "failed template batch replaced old world identity");
 Check(Value("health")==health&&Value("updates")==updates&&body->GetPosition()==position&&Global("LOADS")==1&&Global("DESTROYS")==0,
       "failed template batch changed old gameplay or executed source/lifecycle");
 Check(TemplateHealth(Actor::LoadTemplate("Reusable"))==11,"rejected scene polluted live-spawn template cache");
 Write(fixture.root/"project/actor_templates/Broken.template","[]");
 bool rejected=false;try{(void)SceneFormat::LoadActorTemplateAsset("Broken");}catch(const std::exception &){rejected=true;}
 Check(rejected,"array-root actor template was silently accepted");
 scene.actors.resize(1);scene.actors.front().template_name="Broken";
 Check(!engine.LoadSceneAsset(scene)&&!engine.RequiresSceneReset(),"array-root template scene was not rejected before commit");
 Check(ref.rawequal(Probe())&&FrameClock::GetFrameNumber()==frame&&Value("health")==health,"bad template root mutated old world");
 Actor::ClearTemplateCache();
}
void TestCommitGateReentry(){
 Fixture fixture;Engine engine;engine.InitializeHeadless();engine.StepSimulation();
 const auto target=SceneFormat::LoadSceneAsset("other");
 const auto ref=Probe();const luabridge::LuaRef bag=ref["bag"];
 const ActorHandle handle(engine.GetRuntimeActorByUID(1));
 auto *body=ComponentManager::GetComponentByKey(1,"body").cast<Rigidbody *>().value();body->SetVelocity(b2Vec2(1,0));
 for(int kind=0;kind<2;++kind){
  const int frame=FrameClock::GetFrameNumber(),updates=Value("updates");const auto position=body->GetPosition();
  int calls=0;bool nested_accepted=true;
  const bool accepted=engine.LoadSceneAsset(target,[&]{
   ++calls;
   if(kind==0){engine.StepSimulation();return true;}
   nested_accepted=engine.LoadSceneAsset(SceneFormat::LoadSceneAsset("main"));return nested_accepted;
  });
  Check(!accepted&&calls==1&&!engine.RequiresSceneReset(),"precommit reentry was not rejected recoverably");
  if(kind==1)Check(!nested_accepted,"nested LoadSceneAsset committed during outer gate");
  Check(FrameClock::GetFrameNumber()==frame&&engine.GetCurrentSceneName()=="main"&&Value("updates")==updates,
        "reentrant gate advanced simulation or changed scene");
  Check(handle.IsValid()&&ref.rawequal(Probe())&&bag.rawequal(Probe()["bag"])&&body->GetPosition()==position,
        "reentrant gate changed old actor/body/Lua/native inventory identity");
  Check(Global("LOADS")==1&&Global("DESTROYS")==0&&Value("starts")==1&&Value("health")==3,
        "reentrant gate executed lifecycle or source code");
  engine.StepSimulation();
  Check(FrameClock::GetFrameNumber()==frame+1&&Value("updates")==updates+1,
        "rejection left the scene transition guard stuck");
  Check(body->GetPosition().x>position.x,"normal old-world physics failed after gate rejection");
 }
}

void TestEditorSelection(){
 Fixture fixture;Engine engine;engine.InitializeHeadless();EditorSceneSession editor;
 editor.LoadInitialScene(engine);editor.SyncRuntimeMirrorIfDirty(engine);engine.StepSimulation();
 auto &author=editor.GetAuthoringSceneDocument();
 Check(author.SetActorName(1,"authoring_draft"),"could not create dirty authoring scene");
 const auto original=Read(fixture.root/"project/scenes/main.scene");
 const auto generation=engine.GetRuntimeActorByUID(1)->runtime_generation;const auto frame=FrameClock::GetFrameNumber();
 const auto subdirectory=Scene::GetActiveSceneSubdirectory();
 Check(!editor.OpenSceneFromPath(fixture.root/"project/scenes/zone/bad.scene",engine),"editor reported successful rejected scene");
 Check(editor.GetSceneDocument().GetSceneName()=="main" && author.IsDirty() && author.GetActorDisplayName(1)=="authoring_draft","failed open replaced/discarded authoring draft");
 Check(Read(fixture.root/"project/scenes/main.scene")==original,"failed target autosaved outgoing draft");
 Check(Scene::GetActiveSceneSubdirectory()==subdirectory,"failed editor document preparation changed scene subdirectory");
 Check(engine.GetRuntimeActorByUID(1)->runtime_generation==generation && FrameClock::GetFrameNumber()==frame,"failed editor open replaced runtime");
 // A new edit on a synchronized document must now be rejected before it
 // can create an invalid authoring target. Keep the old dirty draft and live
 // native/Lua identities while checking that stronger coordinator contract.
 const auto reference=Probe();const luabridge::LuaRef inventory=reference["bag"];
 const auto body_reference=ComponentManager::GetComponentByKey(1,"body");
 auto *native_body=body_reference.cast<Rigidbody *>().value()->body;
 const auto position=body_reference.cast<Rigidbody *>().value()->GetPosition();
 const int starts_before=Value("starts"),updates_before=Value("updates"),destroys_before=Global("DESTROYS");
 Check(!author.SetComponentType(0,"probe","MissingComponent"),"synchronized authoring accepted an invalid new type");
 const auto unchanged_actor=author.BuildEffectiveActor(0);
 const auto *unchanged_probe=SceneFormat::FindComponentSpec(unchanged_actor.component_specs,"probe");
 Check(unchanged_probe&&unchanged_probe->type=="Probe"&&author.IsDirty()&&author.GetActorDisplayName(1)=="authoring_draft",
       "rejected new edit changed the previous dirty authoring draft");
 Check(!editor.IsRuntimeMirrorPending()&&!editor.GetLastEditError().empty()&&
       engine.GetRuntimeActorByUID(1)->runtime_generation==generation&&FrameClock::GetFrameNumber()==frame&&
       reference.rawequal(Probe())&&inventory.rawequal(Probe()["bag"])&&
       body_reference.rawequal(ComponentManager::GetComponentByKey(1,"body"))&&
       body_reference.cast<Rigidbody *>().value()->body==native_body&&body_reference.cast<Rigidbody *>().value()->GetPosition()==position,
       "rejected synchronized edit changed live world identities or mirror state");
 Check(Value("starts")==starts_before&&Value("updates")==updates_before&&Global("DESTROYS")==destroys_before&&
       Read(fixture.root/"project/scenes/main.scene")==original,"rejected edit ran callbacks or saved the old draft");
 // Model a pre-existing invalid/unmirrored draft explicitly. Detached copies
 // are editable without committing into Engine; assignment retains the
 // session hook, and the dirty notification enables incremental repair.
 SceneDocument invalid_author=author;
 Check(invalid_author.SetComponentType(0,"probe","MissingComponent"),"could not prepare detached invalid draft");
 author=std::move(invalid_author);editor.MarkRuntimeMirrorDirty();
 Check(!editor.EnterPlayMode(engine) && !editor.IsPlayModeActive() && !editor.HasPlaySceneDocument()&&editor.IsRuntimeMirrorPending(),
       "failed play entry published play document or discarded pending draft");
 Check(author.IsDirty()&&author.GetActorDisplayName(1)=="authoring_draft"&&
       engine.GetRuntimeActorByUID(1)->runtime_generation==generation&&FrameClock::GetFrameNumber()==frame&&
       reference.rawequal(Probe())&&inventory.rawequal(Probe()["bag"])&&
       body_reference.cast<Rigidbody *>().value()->body==native_body&&body_reference.cast<Rigidbody *>().value()->GetPosition()==position&&
       Value("starts")==starts_before&&Value("updates")==updates_before&&Global("DESTROYS")==destroys_before&&
       Read(fixture.root/"project/scenes/main.scene")==original,
       "failed entry into invalid pending draft changed old world, callbacks, or unsaved authoring work");
 Check(author.SetComponentType(0,"probe","Probe"),"could not repair authoring type");
 editor.SyncRuntimeMirrorIfDirty(engine);
 Check(!editor.IsRuntimeMirrorPending()&&editor.GetLastEditError().empty()&&author.IsDirty(),
       "repaired draft did not synchronize or lost unsaved state");
 Check(editor.EnterPlayMode(engine),engine.GetLastSceneLoadError());engine.StepSimulation();Check(editor.TogglePlayPause(),"cannot pause play");
 Check(editor.GetSceneDocument().SetActorName(0,"play_draft"),"could not create play draft");
 const auto playing_generation=engine.GetRuntimeActorByUID(1)->runtime_generation;
 Check(!editor.OpenSceneFromPath(fixture.root/"project/scenes/zone/bad.scene",engine),"bad scene accepted while paused");
 Check(editor.IsPlayModeActive() && editor.IsPlayModePaused() && editor.HasPlaySceneDocument() &&
       editor.GetSceneDocument().GetActorDisplayName(0)=="play_draft" && author.IsDirty(),"failed open lost paused play/authoring documents");
 Check(engine.GetRuntimeActorByUID(1)->runtime_generation==playing_generation,"failed paused open replaced runtime");
 Check(Scene::GetActiveSceneSubdirectory()==subdirectory,"failed paused open changed active scene subdirectory");
 Check(author.SetComponentType(0,"probe","MissingComponent"),"could not invalidate exit target");
 Check(!editor.ExitPlayMode(engine) && editor.IsPlayModeActive() && editor.IsPlayModePaused() &&
       editor.GetSceneDocument().GetActorDisplayName(0)=="play_draft","failed Stop discarded paused live session");
 Check(author.SetComponentType(0,"probe","Probe"),"could not repair exit target");
 // Force the outgoing autosave to fail after candidate validation, before
 // native world commit, without relying on permissions/root behavior.
 const auto source=fixture.root/"project/scenes/main.scene",backup=fixture.root/"project/scenes/main.backup";
 std::filesystem::rename(source,backup);std::filesystem::create_directory(source);
 Check(!editor.OpenSceneFromPath(fixture.root/"project/scenes/other.scene",engine),"failed outgoing autosave still committed target");
 Check(editor.IsPlayModeActive() && editor.IsPlayModePaused() && author.IsDirty() &&
       engine.GetRuntimeActorByUID(1)->runtime_generation==playing_generation,"save gate failure mutated editor/runtime");
 std::filesystem::remove(source);std::filesystem::rename(backup,source);
 Check(editor.OpenSceneFromPath(fixture.root/"project/scenes/other.scene",engine),engine.GetLastSceneLoadError());
 Check(!editor.IsPlayModeActive() && !editor.IsPlayModePaused() && !editor.HasPlaySceneDocument() &&
       editor.GetSceneDocument().GetSceneName()=="other" && engine.GetCurrentSceneName()=="other","successful Open failed to commit document/runtime together");
 Check(Read(source).find("authoring_draft")!=std::string::npos,"successful Open lost existing autosave policy");
}
}
int main(){try{TestRejectedAsset();TestTemplateSnapshots();TestCommitGateReentry();TestEditorSelection();std::cout<<"scene_transition_test: preflight side-effect isolation, cached sources and transactional editor selection passed\n";return 0;}
catch(const std::exception &error){std::cerr<<"scene_transition_test: "<<error.what()<<'\n';return 1;}}
