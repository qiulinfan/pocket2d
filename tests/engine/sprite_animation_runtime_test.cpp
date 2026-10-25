#include "core/Engine.h"
#include "core/FrameClock.h"
#include "input/Input.h"
#include "rendering/SpriteAnimation.h"
#include "rendering/SpriteRenderer.h"
#include "physics/RayCast.h"
#include "scripting/ComponentManager.h"
#include "shared/resources/ResourcePath.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
void Check(bool b,const std::string &message){if(!b)throw std::runtime_error(message);}
auto Ref(const char *key){return ComponentManager::GetComponentByKey(1,key);}
SpriteAnimation *Anim(){return Ref("anim").cast<SpriteAnimation *>().value();}
SpriteRenderer *Sprite(){return Ref("sprite").cast<SpriteRenderer *>().value();}
int Count(const char *field,const char *key="controller"){return Ref(key)[field].cast<int>().value();}
std::string Trace(){return ComponentManager::GetComponentByKey(2,"view")["trace"].cast<std::string>().value();}
void Set(const char *key,const char *field,Actor::ComponentPropertyValue value){
 Check(ComponentManager::SetRuntimeComponentPropertyValue(1,key,field,value),std::string("fixture setup rejected ")+field);
}
class Driver {
 Engine &engine_;std::set<SDL_Scancode> held_;
public:
 explicit Driver(Engine &engine):engine_(engine){Check(engine.GetLastSceneLoadError().empty(),engine.GetLastSceneLoadError());engine.InitializeHeadless();}
 void Step(int n=1,std::initializer_list<SDL_Scancode> keys={}){
  std::set<SDL_Scancode> next(keys);
  auto emit=[](SDL_Scancode key,bool down){SDL_Event e{};e.type=down?SDL_EVENT_KEY_DOWN:SDL_EVENT_KEY_UP;e.key.scancode=key;e.key.key=SDL_GetKeyFromScancode(key,SDL_KMOD_NONE,false);e.key.down=down;Input::ProcessEvent(e);};
  for(auto key:held_)if(!next.count(key))emit(key,false);
  for(auto key:next)if(!held_.count(key))emit(key,true);
  held_=std::move(next);const int before=FrameClock::GetFrameNumber();
  for(int i=0;i<n;++i)engine_.StepSimulation();
  Check(FrameClock::GetFrameNumber()==before+n,"frame clock mismatch");
  Check(ComponentManager::GetRuntimeErrorCount()==0,ComponentManager::GetLastRuntimeError());
 }
};
void State(int clock,const char *clip,int frame,int elapsed,bool playing=true){
 Check(FrameClock::GetFrameNumber()==clock,"wrong simulation frame");
 auto a=Ref("anim");
 Check(a["current_clip"].cast<std::string>().value()==clip,"wrong animation clip at "+std::to_string(clock));
 Check(a["frame"].cast<int>().value()==frame,"wrong animation frame at "+std::to_string(clock));
 Check(a["elapsed_ticks"].cast<int>().value()==elapsed,"wrong elapsed ticks at "+std::to_string(clock));
 Check(a["playing"].cast<bool>().value()==playing,"wrong playing state at "+std::to_string(clock));
}
std::string Event(const std::string &type,const std::string &clip,int frame,const std::string &marker){
 std::string out;for(const auto *label:{"A","C","Z"})out+=std::string(label)+":"+type+":"+clip+":"+std::to_string(frame)+":"+marker+";";return out;
}
void Timeline(){
 Engine e;Driver d(e);State(0,"idle",1,0);Check(Sprite()->HasAnimationOverride(),"frame-zero pose missing");
 d.Step();State(1,"idle",1,1);d.Step(2);State(3,"idle",1,3);d.Step();State(4,"idle",1,0);
 d.Step(1,{SDL_SCANCODE_RIGHT});State(5,"walk",1,0);
 d.Step(1,{SDL_SCANCODE_RIGHT});State(6,"walk",1,1);
 d.Step(1,{SDL_SCANCODE_RIGHT});State(7,"walk",2,0);
 d.Step(2,{SDL_SCANCODE_RIGHT});State(9,"walk",1,0);
 Check(Count("frame_events")==5 && Count("play_calls")==9,"held input restarted animation");
 d.Step(1,{SDL_SCANCODE_SPACE});State(10,"attack",1,0);
 d.Step();State(11,"attack",2,0);Check(Count("hit_events")==1,"duration-one entry marker lost");
 d.Step();State(12,"attack",2,1);d.Step();State(13,"idle",1,0);d.Step();State(14,"idle",1,1);
 Check(Count("completed_events")==1 && Count("frame_events")==8,"completion/entry count wrong");
 const auto expected=Event("frame","idle",1,"rest")+Event("frame","idle",1,"rest")+
  Event("frame","walk",1,"left")+Event("frame","walk",2,"right")+Event("frame","walk",1,"left")+
  Event("frame","attack",1,"swing")+Event("frame","attack",2,"hit")+Event("completed","attack",2,"")+Event("frame","idle",1,"rest");
 Check(Trace()==expected,"event order, payload, or one-shot multiplicity mismatch");
 Check(Ref("controller")["last_component_key"].cast<std::string>().value()=="anim","event component key incorrect");
 Check(Sprite()->sprite=="base.png","animation changed authored sprite");
}
void Controls(){
 Engine e;Driver d(e);d.Step(1,{SDL_SCANCODE_RIGHT});d.Step(1,{SDL_SCANCODE_RIGHT});
 d.Step(1,{SDL_SCANCODE_RIGHT,SDL_SCANCODE_P});State(3,"walk",2,0,false);
 d.Step(5,{SDL_SCANCODE_RIGHT,SDL_SCANCODE_T});State(8,"walk",2,0,false);
 Check(Anim()->IsPaused(),"same-clip Play resumed paused animation");
 d.Step(1,{SDL_SCANCODE_RIGHT,SDL_SCANCODE_R});State(9,"walk",2,0);
 d.Step(2,{SDL_SCANCODE_RIGHT});State(11,"walk",1,0);
 d.Step(1,{SDL_SCANCODE_RIGHT,SDL_SCANCODE_D});State(12,"walk",1,1,false);
 const int frozen_events=Count("frame_events");d.Step(5,{SDL_SCANCODE_RIGHT,SDL_SCANCODE_T});State(17,"walk",1,1,false);
 Check(Count("frame_events")==frozen_events,"disabled animation dispatched events");
 d.Step(1,{SDL_SCANCODE_RIGHT,SDL_SCANCODE_D});State(18,"walk",1,1);
 d.Step(1,{SDL_SCANCODE_RIGHT});State(19,"walk",2,0);
 d.Step(1,{SDL_SCANCODE_RIGHT,SDL_SCANCODE_S});State(20,"",0,0,false);
 Check(!Sprite()->HasAnimationOverride(),"Stop did not restore base pose");
 d.Step(4,{SDL_SCANCODE_RIGHT,SDL_SCANCODE_T});State(24,"",0,0,false);
 d.Step(1,{SDL_SCANCODE_RIGHT,SDL_SCANCODE_R});State(25,"",0,0,false);
 d.Step(1,{SDL_SCANCODE_RIGHT,SDL_SCANCODE_X});State(26,"walk",1,0);
 d.Step(1,{SDL_SCANCODE_RIGHT});d.Step(1,{SDL_SCANCODE_RIGHT,SDL_SCANCODE_X});State(28,"walk",1,0);
}
void QueuesAndCompletion(){
 for(int mode=0;mode<3;++mode){
  Engine e;Driver d(e);Set("controller","auto_select",false);
  if(mode==0)Anim()->Pause();else if(mode==1)Anim()->enabled=false;else Anim()->Stop();
  d.Step(5);Check(Trace().empty(),"frozen/stopped queued entry was dispatched");
  if(mode==0)Anim()->Resume();else if(mode==1)Anim()->enabled=true;else Check(Anim()->Play("idle"),"play rejected");
  d.Step();Check(Trace()==Event("frame","idle",1,"rest"),"resume lost/duplicated queued entry");
 }
 Engine e;Driver d(e);Set("controller","auto_select",false);Check(Anim()->Play("attack"),"attack rejected");
 d.Step();State(1,"attack",2,0);d.Step(2);State(3,"attack",2,2,false);
 Check(Anim()->IsCompleted() && Count("completed_events")==1,"one shot did not complete");
 const auto completed_id=Anim()->GetPlaybackId();Check(!Anim()->Play("attack"),"same clip Play changed playback");
 Check(Anim()->GetPlaybackId()==completed_id,"same clip Play changed playback identity");d.Step(5);State(8,"attack",2,2,false);
 Check(Count("completed_events")==1,"same clip Play restarted completed clip");
 const auto id=Anim()->GetPlaybackId();bool rejected=false;try{Anim()->Play("missing");}catch(const std::runtime_error &){rejected=true;}Check(rejected,"unknown clip accepted");
 Check(Anim()->GetPlaybackId()==id && Anim()->IsCompleted(),"invalid clip mutated state");
 Check(Anim()->Restart("attack"),"Restart failed");d.Step(3);Check(Count("completed_events")==2,"Restart did not repeat one shot");
}
void Reentry(){
 for(const char *mode:{"stop","restart","remove","destroy"}){
  Engine e;Driver d(e);Set("controller","auto_select",false);Set("a_observer","reentry",std::string(mode));Anim()->Play("attack");
  d.Step();Check(Trace()=="A:frame:attack:1:swing;",std::string("stale callback batch survived ")+mode);
  if(std::string(mode)=="stop"){State(1,"",0,0,false);Check(!Sprite()->HasAnimationOverride(),"reentrant Stop retained pose");}
  else if(std::string(mode)=="restart"){
   State(1,"walk",1,0);d.Step();State(2,"walk",1,1);
   Check(Trace()=="A:frame:attack:1:swing;"+Event("frame","walk",1,"left"),"reentrant restart delivered recursively or lost queued entry");
  }else if(std::string(mode)=="remove")Check(Ref("anim").isNil() && !Sprite()->HasAnimationOverride(),"removed animation retained target pose");
  else Check(e.GetRuntimeActorByUID(1)==nullptr,"destroy callback retained actor");
 }
}
void PauseInCallbackAndEmptyClip(){
 {
  Engine e;Driver d(e);Set("controller","auto_select",false);Set("a_observer","reentry",std::string("pause"));Anim()->Play("attack");
  d.Step();State(1,"attack",2,0,false);
  const auto expected=Event("frame","attack",1,"swing")+Event("frame","attack",2,"hit");
  Check(Trace()==expected && Anim()->IsPaused(),"Pause in callback suppressed the already generated event batch");
  d.Step(4);State(5,"attack",2,0,false);Check(Trace()==expected,"paused animation advanced in future ticks");
  Anim()->Resume();d.Step(2);State(7,"attack",2,2,false);
  Check(Trace()==expected+Event("completed","attack",2,""),"resumed one-shot completion mismatch");
 }
 Engine e;Driver d(e);auto scene=SceneFormat::LoadSceneAsset("animation");auto &specs=scene.actors.front().component_specs;
 SceneFormat::UpsertComponentProperty(*SceneFormat::FindComponentSpec(specs,"anim"),"clip",std::string(""));
 SceneFormat::UpsertComponentProperty(*SceneFormat::FindComponentSpec(specs,"controller"),"auto_select",false);
 Check(e.LoadSceneAsset(scene),e.GetLastSceneLoadError());State(0,"",0,0,false);
 Check(!Sprite()->HasAnimationOverride(),"empty configured clip selected a pose");d.Step(5);State(5,"",0,0,false);
 Check(Trace().empty(),"empty configured clip emitted animation events");
}

void Preflight(){
 for(int mode=0;mode<6;++mode){
  Engine e;Driver d(e);d.Step(2,{SDL_SCANCODE_RIGHT});
  auto *old=Anim();const auto playback=old->GetPlaybackId();const auto gen=e.GetRuntimeActorByUID(1)->runtime_generation;
  const auto bodies=RayCast::GetBodyCount();const auto trace=Trace();
  auto scene=SceneFormat::LoadSceneAsset("animation");scene.scene_name="invalid_candidate";
  auto &specs=scene.actors.front().component_specs;auto *spec=SceneFormat::FindComponentSpec(specs,"anim");
  if(mode==0)SceneFormat::UpsertComponentProperty(*spec,"renderer_key",std::string("missing"));
  if(mode==1)SceneFormat::UpsertComponentProperty(*spec,"renderer_key",std::string("body"));
  if(mode==2)SceneFormat::UpsertComponentProperty(*spec,"clip",std::string("missing"));
  if(mode==3)SceneFormat::UpsertComponentProperty(*spec,"animation_resource",std::string("missing.animation.json"));
  if(mode==4){auto duplicate=*spec;duplicate.key="second_anim";specs.push_back(duplicate);}
  if(mode==5)SceneFormat::UpsertComponentProperty(*spec,"enabled",std::string("yes"));
  Check(!e.LoadSceneAsset(scene),"invalid animation preload accepted case "+std::to_string(mode));
  Check(e.GetRuntimeActorByUID(1)->runtime_generation==gen && Anim()==old,"rejected preload replaced live generation");
  Check(Anim()->GetPlaybackId()==playback && Anim()->GetFrame()==1 && Anim()->GetElapsedTicks()==1,"rejected preload reset playback");
  Check(Trace()==trace && RayCast::GetBodyCount()==bodies,"rejected preload altered callbacks/physics");
  Check(e.GetCurrentSceneName()=="animation","rejected preload changed scene");d.Step(1,{SDL_SCANCODE_RIGHT});State(3,"walk",2,0);
 }
}
void TargetRebind(){
 Engine e;Driver d(e);Set("controller","auto_select",false);d.Step();
 auto removed=Ref("sprite");const auto playback=Anim()->GetPlaybackId();
 ComponentManager::RemoveComponent(1,removed);d.Step(2);
 Check(Ref("sprite").isNil() && Anim()->IsPlaying() && Anim()->GetElapsedTicks()==3,
       "missing target must not freeze internal animation timeline");
 auto replacement=ComponentManager::AddComponent(1,"SpriteRenderer");
 Check(!replacement.isNil(),"could not add replacement renderer");
 const auto key=replacement["key"].cast<std::string>().value();
 Check(ComponentManager::RenameComponentKey(1,key,"sprite"),"could not restore target key");
 replacement["sprite"]=std::string("base.png");
 d.Step();
 Check(Sprite()==replacement.cast<SpriteRenderer *>().value() && Sprite()!=removed.cast<SpriteRenderer *>().value(),"target did not bind replacement instance");
 Check(Sprite()->HasAnimationOverride() && Anim()->GetPlaybackId()==playback && Anim()->GetElapsedTicks()==0,
       "same-key renderer replacement lost live pose or restarted clip");
 removed["sprite"]=std::string("floor.png");removed["scale_x"]=99;
 Check(Sprite()->sprite=="base.png" && Sprite()->scale_x==1,"detached renderer mutated replacement");
 Anim()->Stop();Check(!Sprite()->HasAnimationOverride(),"Stop did not clear rebound target");
}
void Readonly(){
 Engine e;Driver d(e);d.Step(2,{SDL_SCANCODE_RIGHT});
 const auto playback=Anim()->GetPlaybackId();const auto pose=Sprite()->GetAnimationOverrideOwner();
 const auto ref=Ref("anim");luabridge::setGlobal(ref.state(),ref,"__animation_under_test");
 const int result=luaL_dostring(ref.state(),R"(
  for key,value in pairs({current_clip="attack",frame=99,elapsed_ticks=99,playing=false,paused=true,
                        completed=true,animation_resource="missing",renderer_key="body",clip="attack"}) do
    assert(not pcall(function() __animation_under_test[key]=value end),"readonly write accepted: "..key)
  end
  assert(not pcall(function() __animation_under_test:Play("missing") end),"unknown clip accepted")
 )");
 Check(result==LUA_OK,result==LUA_OK?"":std::string(lua_tostring(ref.state(),-1)));
 Check(Anim()->GetPlaybackId()==playback && Sprite()->GetAnimationOverrideOwner()==pose,"rejected Lua write replaced playback/pose");
 State(2,"walk",1,1);
 for(const char *field:{"current_clip","frame","elapsed_ticks","playing","paused","completed"})
  Check(!ComponentManager::SetRuntimeComponentPropertyValue(1,"anim",field,7),std::string("runtime wrote readonly field ")+field);
 State(2,"walk",1,1);
}
void ClearAndRestoreResource(){
 Engine e;Driver d(e);Set("controller","auto_select",false);d.Step();
 Check(ComponentManager::SetRuntimeComponentPropertyValue(1,"anim","animation_resource",std::string("")),
       "clearing optional animation resource was rejected");
 State(1,"",0,0,false);
 Check(!Anim()->HasAsset() && !Sprite()->HasAnimationOverride(),"cleared resource retained asset or pose");
 const auto trace=Trace();d.Step(5);State(6,"",0,0,false);
 Check(Trace()==trace,"inert animation resource emitted queued events");
 Check(ComponentManager::SetRuntimeComponentPropertyValue(1,"anim","animation_resource",std::string("hero.animation.json")),
       "restoring valid animation resource was rejected");
 State(6,"idle",1,0);
 Check(Anim()->HasAsset() && Sprite()->HasAnimationOverride(),"restored resource failed to bind authored initial pose");
 Check(Trace()==trace,"restoring resource dispatched entry before next tick");
 d.Step();State(7,"idle",1,1);
 Check(Trace()==trace+Event("frame","idle",1,"rest"),"restored resource lost/duplicated queued entry");
}

void BadJson(const std::filesystem::path &fixture){
 const auto temporary=std::filesystem::temp_directory_path()/
  ("pocket-animation-preflight-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 struct Cleanup{std::filesystem::path original,temp;~Cleanup(){ResourcePath::SetResourcesRootPath(original);std::error_code ec;std::filesystem::remove_all(temp,ec);}} cleanup{fixture,temporary};
 std::filesystem::copy(fixture,temporary,std::filesystem::copy_options::recursive);
 ResourcePath::SetResourcesRootPath(temporary);
 Engine e;Driver d(e);d.Step(2,{SDL_SCANCODE_RIGHT});
 auto *old=Anim();const auto gen=e.GetRuntimeActorByUID(1)->runtime_generation;
 const auto id=old->GetPlaybackId();const auto bodies=RayCast::GetBodyCount();const auto trace=Trace();
 std::ofstream(temporary/"animations/hero.animation.json")<<"{\"version\":1,\"clips\":[";
 Check(!e.LoadSceneAsset(SceneFormat::LoadSceneAsset("animation")),"bad JSON preload accepted");
 Check(Anim()==old && e.GetRuntimeActorByUID(1)->runtime_generation==gen && Anim()->GetPlaybackId()==id,
       "bad JSON preload destroyed current world");
 Check(Trace()==trace && RayCast::GetBodyCount()==bodies,"bad JSON preload mutated events or bodies");
 State(2,"walk",1,1);d.Step(1,{SDL_SCANCODE_RIGHT});State(3,"walk",2,0);
}

void Reload(){
 Engine e;Driver d(e);d.Step(2,{SDL_SCANCODE_RIGHT});auto old=Ref("anim");auto *stale=old.cast<SpriteAnimation *>().value();
 const auto generation=e.GetRuntimeActorByUID(1)->runtime_generation;
 Check(e.LoadSceneAsset(SceneFormat::LoadSceneAsset("animation")),e.GetLastSceneLoadError());
 Check(e.GetRuntimeActorByUID(1)->runtime_generation!=generation,"reload reused actor generation");
 const auto owner=Sprite()->GetAnimationOverrideOwner();Check(Sprite()->HasAnimationOverride(),"reload missing initial pose");
 bool rejected=false;try{stale->Restart("attack");}catch(const std::runtime_error &error){rejected=std::string(error.what()).find("expired")!=std::string::npos;}
 Check(rejected,"stale animator Restart was not rejected as expired");stale->OnStart();stale->ApplyPose();stale->Stop();
 Check(!stale->IsPlaying() && Sprite()->GetAnimationOverrideOwner()==owner && Anim()->GetCurrentClip()=="idle","stale animator controlled replacement generation");
 auto scene=SceneFormat::LoadSceneAsset("animation");auto *spec=SceneFormat::FindComponentSpec(scene.actors.front().component_specs,"anim");spec->key="zz_anim";
 Check(e.LoadSceneAsset(scene),e.GetLastSceneLoadError());
 Check(Sprite()->HasAnimationOverride(),"late-key animation failed to bind frame-zero target");
 auto renamed=ComponentManager::GetComponentByKey(1,"zz_anim");Check(renamed["frame"].cast<int>().value()==1,"late-key autoplay frame differs");
 d.Step();Check(Ref("controller")["last_component_key"].cast<std::string>().value()=="zz_anim","renamed key event payload wrong");
}
}
int main(int argc,char **argv){try{
 Check(argc==2,"usage: sprite_animation_runtime_test FIXTURE_PROJECT");
 ResourcePath::SetResourcesRootPath(std::filesystem::absolute(argv[1]));
 Timeline();Controls();QueuesAndCompletion();Reentry();PauseInCallbackAndEmptyClip();Preflight();Reload();TargetRebind();Readonly();ClearAndRestoreResource();
 BadJson(std::filesystem::absolute(argv[1]));
 std::cout<<"sprite_animation_runtime_test: timeline, controls, queues, callback reentry, preload and generations passed\n";return 0;
}catch(const std::exception &e){std::cerr<<"sprite_animation_runtime_test: "<<e.what()<<'\n';return 1;}}
