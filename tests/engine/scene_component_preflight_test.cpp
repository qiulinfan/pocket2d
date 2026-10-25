#include "core/Engine.h"
#include "core/FrameClock.h"
#include "physics/Rigidbody.h"
#include "particles/ParticleSystem.h"
#include "physics/RayCast.h"
#include "scene/ActorHandle.h"
#include "scripting/ComponentManager.h"
#include "scripting/SceneComponentPlan.h"
#include "shared/resources/ResourcePath.h"
#include "shared/scene_format/SceneFormat.h"
#include "SDL3/SDL.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void Check(bool value,const std::string &message){if(!value)throw std::runtime_error(message);}
void Lua(lua_State *state,const char *code){
    const int top=lua_gettop(state);const int status=luaL_dostring(state,code);
    const std::string message=status==LUA_OK?"":lua_tostring(state,-1);lua_settop(state,top);Check(status==LUA_OK,message);
}
struct Fixture {
    std::filesystem::path previous=ResourcePath::ResourcesRootPath();
    std::filesystem::path root=std::filesystem::temp_directory_path()/
        ("pocket-scene-component-plan-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    bool had_save_root=false;std::string previous_save_root;
    Fixture(){
        if(const char *value=SDL_getenv("POCKET_SAVE_ROOT")){had_save_root=true;previous_save_root=value;}
        SDL_setenv_unsafe("POCKET_SAVE_ROOT",(root/"save-root").string().c_str(),1);
        for(const char *directory:{"scenes","component_types","items","images","animations"})std::filesystem::create_directories(root/directory);
        std::ofstream(root/"game.config")<<R"({"initial_scene":"old","save_namespace":"PreflightGuard"})";
        std::ofstream(root/"scenes/old.scene")<<R"({"actors":[{"uid":1,"name":"old","components":{
          "body":{"type":"Rigidbody","gravity_scale":0},"logic":{"type":"Stable"},"transform":{"type":"Transform"}}}]})";
        std::ofstream(root/"scenes/target.scene")<<R"({"actors":[]})";
        std::ofstream(root/"items/items.items.json")<<R"({"version":1,"items":[{"id":"ore","display_name":"Ore","kind":"material","max_stack":10}]})";
        std::ofstream(root/"component_types/Stable.lua")<<R"(
Stable={starts=0,updates=0,pulses=0,
 OnStart=function(self)
  self.starts=self.starts+1;self.bag=Inventory.Open("items.items.json"):Create(2);assert(self.bag:Add("ore",7).ok)
  Event.Subscribe("preflight-pulse",self,self.Pulse)
 end,
 OnUpdate=function(self)self.updates=self.updates+1 end,
 Pulse=function(self,event)self.pulses=self.pulses+1 end,
 OnDestroy=function(self)DestroyedOld=(DestroyedOld or 0)+1 end}
)";
        std::ofstream(root/"component_types/BadOwned.lua")<<"BadOwned={owned=Rigidbody()}";
        std::ofstream(root/"component_types/BadThread.lua")<<"BadThread={thread=coroutine.create(function()end)}";
        std::ofstream(root/"component_types/BadPointer.lua")<<"local value=1;local function f()return value end;BadPointer={pointer=debug.upvalueid(f,1)}";
        std::ofstream(root/"component_types/Dynamic.lua")<<R"(
GetterLookups=0;SynthesizedUpdates=0
Dynamic=setmetatable({seen=0},{__index=function(_,key)
 GetterLookups=GetterLookups+1
 if key=="OnUpdate" then return function()SynthesizedUpdates=SynthesizedUpdates+1 end end
 if key=="OnAnimationEvent" then return function(self,event)self.seen=self.seen+1 end end
 return nil
end})
)";
        auto *atlas=SDL_CreateSurface(8,4,SDL_PIXELFORMAT_RGBA32);Check(atlas!=nullptr,"create atlas");
        SDL_FillSurfaceRect(atlas,nullptr,SDL_MapSurfaceRGBA(atlas,255,255,255,255));
        const bool saved=SDL_SaveBMP(atlas,(root/"images/atlas.bmp").string().c_str());SDL_DestroySurface(atlas);Check(saved,"save atlas");
        std::ofstream(root/"animations/test.animation.json")<<R"({"version":1,"image":"atlas.bmp","columns":2,"rows":1,"tile_width_px":4,"tile_height_px":4,"clips":[{"name":"loop","loop":true,"frames":[{"row":1,"column":1,"ticks":1},{"row":1,"column":2,"ticks":1}]}]})";
        ResourcePath::SetResourcesRootPath(root);
    }
    ~Fixture(){ResourcePath::SetResourcesRootPath(previous);
        if(had_save_root)SDL_setenv_unsafe("POCKET_SAVE_ROOT",previous_save_root.c_str(),1);else SDL_unsetenv_unsafe("POCKET_SAVE_ROOT");
        std::error_code error;std::filesystem::remove_all(root,error);}
    SceneFormat::SceneAsset Target(const Actor::ComponentSpec &spec) const {
        SceneFormat::SceneAsset asset;asset.scene_name="target";asset.scene_path=root/"scenes/target.scene";
        SceneFormat::ActorRecord actor;actor.uid=10;actor.name="new";actor.component_specs={spec};asset.actors.push_back(actor);return asset;
    }
};
void RejectedScenesKeepWorld() {
    Fixture fixture;Engine engine;engine.InitializeHeadless();engine.StepSimulation();
    auto old=ComponentManager::GetComponentByKey(1,"logic");auto *state=old.state();
    auto body_ref=ComponentManager::GetComponentByKey(1,"body");auto *body=body_ref.cast<Rigidbody *>().value();
    auto *old_actor=engine.GetRuntimeActorByUID(1);ActorHandle handle(old_actor);
    auto world = RayCast::GetPhysicsWorld();auto native_body = body->body;const auto position=body->GetPosition();
    const int frame=FrameClock::GetFrameNumber();
    Lua(state,"old_instance=Actor.Find('old'):GetComponent('Stable');old_bag=old_instance.bag;DestroyedOld=0");
    const auto reject=[&](const Actor::ComponentSpec &spec){
        const bool accepted=engine.LoadSceneAsset(fixture.Target(spec));
        Check(!accepted,"invalid scene component accepted: "+spec.type);
        Check(!engine.GetLastSceneLoadError().empty(),"scene rejection lost diagnostic");
        Check(FrameClock::GetFrameNumber()==frame && engine.GetRuntimeActorByUID(1)==old_actor && handle.IsValid(),"rejected scene invalidated old actor/frame/index");
        Check(RayCast::GetPhysicsWorld()==world && body->body==native_body && RayCast::GetBodyCount()==1 &&
              body->GetPosition().x==position.x && body->GetPosition().y==position.y,"rejected scene altered native physics");
        Lua(state,R"(
          assert(Actor.Find("old"):GetComponent("Stable")==old_instance and old_instance.bag==old_bag)
          assert(old_bag:GetSlot(1).count==7 and DestroyedOld==0)
          local before=old_instance.pulses;Event.Publish("preflight-pulse",{});assert(old_instance.pulses==before+1)
        )");
    };
    for(const auto &spec:std::vector<Actor::ComponentSpec>{
        {"bad","BadOwned",{}},{"bad","BadThread",{}},{"bad","BadPointer",{}},
        {"bad","Transform",{{"not_a_member",1}}},{"bad","Transform",{{"actor",true}}},
        {"bad","Transform",{{"key",std::string("different")}}},{"bad","Transform",{{"x",std::string("bad")}}},
        {"bad","Transform",{{"x",1e300}}},{"bad","Transform",{{"x",std::numeric_limits<double>::infinity()}}},
        {"bad","Rigidbody",{{"x",1e300}}},{"bad","Rigidbody",{{"gravity_scale",1e38}}},
        {"bad","Rigidbody",{{"gravity_scale",-1e38}}},{"bad","Rigidbody",{{"angular_friction",-1.0}}},
        {"bad","Rigidbody",{{"density",-1.0}}},{"bad","Rigidbody",{{"width",0.0}}},
        {"bad","Rigidbody",{{"radius",0.0}}},{"bad","Rigidbody",{{"body_type",std::string("invalid")}}},
        {"bad","SpriteRenderer",{{"sprite_row",1.5}}},{"bad","SpriteRenderer",{{"enabled",std::string("false")}}},
        {"bad","ParticleSystem",{{"duration_frames",std::numeric_limits<int>::max()},{"frames_between_bursts",2}}},
        {"bad","ParticleSystem",{{"duration_frames",1000000},{"burst_quantity",2}}},
        {"bad","ParticleSystem",{{"start_scale_min",2.0},{"start_scale_max",1.0}}},
        {"bad","ParticleSystem",{{"start_speed_min",-3e38},{"start_speed_max",3e38}}},
        {"bad","Stable",{{"actor",std::string("wrong")}}}}) reject(spec);
    Check(ComponentManager::GetRuntimeErrorCount()==0,ComponentManager::GetLastRuntimeError());
    for(int tick=0;tick<3;++tick)engine.StepSimulation();
    Check(old["updates"].cast<int>().value()==4 && std::isfinite(body->GetPosition().x) && std::isfinite(body->GetPosition().y) &&
          std::isfinite(body->GetVelocity().x) && std::isfinite(body->GetVelocity().y),
          "old world could not continue with finite physics after rejected scene plans");
    Check(lua_gettop(state)==0,"preflight failures leaked Lua stack");
}
void PositivePreparationAndWorldOverflow() {
    Fixture fixture;Engine engine;engine.InitializeHeadless();engine.StepSimulation();
    std::string error;
    const auto prepare=[&](const std::vector<Actor> &actors){
        auto scripts=ComponentManager::PrepareSceneScripts({},actors,{},error);Check(scripts!=nullptr,error);
        auto components=PrepareSceneComponents(actors,*scripts,error);return static_cast<bool>(components);
    };
    Actor actor;actor.uid=10;actor.actor_name="prepared";
    actor.component_specs={{"particles","ParticleSystem",{{"duration_frames",1},{"frames_between_bursts",1},{"burst_quantity",256000},
                           {"end_scale",std::numeric_limits<double>::quiet_NaN()}}}};
    Check(prepare({actor}),"verified 256k authored particle workload or unset end_scale rejected: "+error);
    actor.component_specs={{"body","Rigidbody",{{"gravity_scale",1e16}}}};
    Check(prepare({actor}),"derived gravity check imposed an unnecessarily small gameplay cap: "+error);
    actor.component_specs={{"transform","Transform",{{"x",3e38}}}};
    Actor child;child.uid=11;child.parent_uid=10;child.component_specs={{"transform","Transform",{{"x",3e38}}}};
    Check(!prepare({actor,child}) && error.find("world pose")!=std::string::npos,"combined finite local Transforms overflowed without preflight rejection");
    child.component_specs={{"transform","Transform",{{"x",1.0}}}};
    Check(prepare({actor,child}),"finite incoming world poses rejected: "+error);
    Check(engine.GetRuntimeActorByUID(1)!=nullptr && engine.GetRuntimeActorByUID(10)==nullptr,"detached prepare registered actors");
}
void StaticLifecycleAndDynamicAnimationEvents() {
    Fixture fixture;Engine engine;engine.InitializeHeadless();engine.StepSimulation();
    auto *state=ComponentManager::GetComponentByKey(1,"logic").state();
    const auto added=ComponentManager::AddComponent(1,"Dynamic");Check(!added.isNil(),"runtime AddComponent failed");
    Lua(state,"assert(GetterLookups==0 and SynthesizedUpdates==0)");
    engine.StepSimulation();Lua(state,"assert(GetterLookups==0 and SynthesizedUpdates==0)");
    auto target=fixture.Target({"dynamic","Dynamic",{}});
    target.actors[0].component_specs.push_back({"sprite","SpriteRenderer",{}});
    target.actors[0].component_specs.push_back({"animation","SpriteAnimation",{{"animation_resource",std::string("test.animation.json")},{"clip",std::string("loop")}}});
    Check(engine.LoadSceneAsset(target),engine.GetLastSceneLoadError());
    Lua(state,"assert(GetterLookups==0 and SynthesizedUpdates==0)");
    engine.StepSimulation();
    Lua(state,R"(
      local dynamic=Actor.Find("new"):GetComponent("Dynamic")
      assert(GetterLookups==2 and dynamic.seen==2 and SynthesizedUpdates==0)
    )");
    Check(ComponentManager::GetRuntimeErrorCount()==0,ComponentManager::GetLastRuntimeError());
    Check(lua_gettop(state)==0,"static lifecycle/dynamic animation dispatch leaked stack");
}
void HostNativeEditsPreserveOldInstances() {
    Fixture fixture;
    std::ofstream(fixture.root/"scenes/old.scene")<<R"({"actors":[{"uid":1,"name":"old","components":{
      "body":{"type":"Rigidbody","gravity_scale":0,"width":1.0},
      "particles":{"type":"ParticleSystem","duration_frames":4,"burst_quantity":1},"sprite":{"type":"SpriteRenderer"},
      "logic":{"type":"Stable"},"transform":{"type":"Transform"}}}]})";
    Engine engine;engine.InitializeHeadless();engine.StepSimulation();
    const auto *owner=engine.GetRuntimeActorByUID(1);ActorHandle handle(owner);
    const auto specs=owner->component_specs;
    auto body_ref=ComponentManager::GetComponentByKey(1,"body");auto *body=body_ref.cast<Rigidbody *>().value();auto native_body = body->body;
    auto particle_ref=ComponentManager::GetComponentByKey(1,"particles");auto *particles=particle_ref.cast<ParticleSystem *>().value();
    auto script_ref=ComponentManager::GetComponentByKey(1,"logic");auto *state=script_ref.state();
    Lua(state,"edit_old_instance=Actor.Find('old'):GetComponent('Stable');edit_old_actor=edit_old_instance.actor;edit_old_bag=edit_old_instance.bag;DestroyedOld=0");
    const auto unchanged=[&]{
        Check(handle.IsValid() && engine.GetRuntimeActorByUID(1)==owner,"rejected native edit changed actor identity");
        Check(ComponentManager::GetComponentByKey(1,"body").cast<Rigidbody *>().value()==body && body->body==native_body && body->width==1,
              "rejected native edit destroyed/replaced Rigidbody");
        Check(ComponentManager::GetComponentByKey(1,"particles").cast<ParticleSystem *>().value()==particles && particles->duration_frames==4,
              "rejected native edit destroyed/replaced ParticleSystem");
        const auto &current=owner->component_specs;
        Check(current.size()==specs.size(),"rejected edit changed authored component count");
        for(std::size_t i=0;i<current.size();++i) {
            Check(current[i].key==specs[i].key && current[i].type==specs[i].type && current[i].overrides.size()==specs[i].overrides.size(),"rejected edit changed authored spec shape");
            for(std::size_t j=0;j<current[i].overrides.size();++j)
                Check(current[i].overrides[j].name==specs[i].overrides[j].name && current[i].overrides[j].value==specs[i].overrides[j].value,
                      "rejected edit changed authored property");
        }
        Lua(state,"assert(Actor.Find('old'):GetComponent('Stable')==edit_old_instance and edit_old_instance.actor==edit_old_actor and edit_old_instance.actor:IsValid() and edit_old_instance.key=='logic' and edit_old_instance.bag==edit_old_bag and DestroyedOld==0)");
    };
    for(const std::string field:{"actor","key"}) {
        Check(!ComponentManager::SetComponentPropertyValue(1,"logic",field,std::string("forbidden")),
              "low-level host property writer changed script component identity");unchanged();
        Check(!ComponentManager::SetRuntimeComponentPropertyValue(1,"logic",field,std::string("forbidden")),
              "runtime host property writer changed script component identity");unchanged();
    }
    Check(ComponentManager::RenameComponentKey(1,"logic","renamed"),"internal rename was blocked by host identity guards");
    Check(ComponentManager::GetComponentByKey(1,"renamed")["key"].cast<std::string>().value()=="renamed","internal rename did not update engine-owned key");
    Check(ComponentManager::RenameComponentKey(1,"renamed","logic"),"internal rename back failed");unchanged();
    Check(!ComponentManager::SetRuntimeComponentPropertyValue(1,"body","width",0.0),"manager accepted zero-width body");unchanged();
    Check(!ComponentManager::SetRuntimeComponentPropertyValue(1,"particles","duration_frames",std::numeric_limits<int>::max()),"manager accepted particle overflow");unchanged();
    for(const auto &mutation:std::vector<SceneFormat::SceneMutation>{
            {SceneFormat::SetComponentPropertyMutation{1,"logic","actor",std::string("forbidden")}},
            {SceneFormat::SetComponentPropertyMutation{1,"logic","key",std::string("forbidden")}},
            {SceneFormat::SetComponentPropertyMutation{1,"body","width",0.0}},
            {SceneFormat::SetComponentPropertyMutation{1,"transform","x",std::numeric_limits<double>::quiet_NaN()}},
            {SceneFormat::SetComponentPropertyMutation{1,"sprite","not_a_member",1}},
            {SceneFormat::SetComponentPropertyMutation{1,"particles","duration_frames",std::numeric_limits<int>::max()}},
            {SceneFormat::SetComponentTypeMutation{1,"body","Transform"}},
            {SceneFormat::SetComponentTypeMutation{1,"logic","BadOwned"}}}) {
        Check(!engine.ApplySceneEditCommand(SceneFormat::SceneEditCommand::Single(mutation)),"host accepted invalid native property/type edit");unchanged();
    }
    std::string error;const bool gc_running=lua_gc(state,LUA_GCISRUNNING)!=0;
    {
        auto first=PrepareRuntimeComponent({"one","Rigidbody",{}},error);Check(first!=nullptr,error);
        auto second=PrepareRuntimeComponent({"two","ParticleSystem",{}},error);Check(second!=nullptr,error);
        Check(lua_gc(state,LUA_GCISRUNNING)==0,"component preparation did not pause GC");
        auto moved=std::make_unique<PreparedRuntimeComponent>(std::move(*first));first.reset();second.reset();
        Check(lua_gc(state,LUA_GCISRUNNING)==0,"nested/moved component plan resumed GC prematurely");
        moved.reset();
    }
    Check((lua_gc(state,LUA_GCISRUNNING)!=0)==gc_running,"component token failed to restore GC state");unchanged();
    Check(ComponentManager::GetRuntimeErrorCount()==0,ComponentManager::GetLastRuntimeError());
    engine.StepSimulation();Check(script_ref["updates"].cast<int>().value()==2,"old scene failed to continue after invalid edits");
}

void RejectedDefaultFinalizersHaveNoDeferredEffects() {
    Fixture fixture;
    std::ofstream(fixture.root/"component_types/BadDefaults.lua")<<R"(
FinalizerCalls=0
local owned=setmetatable({value=1},{__gc=function()
 FinalizerCalls=FinalizerCalls+1
 local old=Actor.Find("old")
 if old then old:GetComponent("Stable").updates=999;Actor.Destroy(old) end
 Save.Write("candidate",1,{ran=true})
end})
BadDefaults={owned=owned}
)";
    std::ofstream(fixture.root/"component_types/BadClose.lua")<<R"(BadClose={owned=setmetatable({},{__close=function()FinalizerCalls=FinalizerCalls+1 end})})";
    std::ofstream(fixture.root/"component_types/BadWeak.lua")<<R"(BadWeak={owned=setmetatable({},{__mode="v"})})";
    Engine engine;engine.InitializeHeadless();engine.StepSimulation();
    auto old=ComponentManager::GetComponentByKey(1,"logic");auto *state=old.state();
    ActorHandle owner(engine.GetRuntimeActorByUID(1));
    for(const auto &type:std::vector<std::string>{"BadDefaults","BadClose","BadWeak"}) {
        auto scene=fixture.Target({"a_bad_defaults",type,{}});
        scene.actors.front().component_specs.push_back({"z_bad_field","Transform",{{"unknown",1}}});
        Check(!engine.LoadSceneAsset(scene),"default finalizer/weak metatable accepted");
        Check(engine.GetLastSceneLoadError().find("default metatable")!=std::string::npos,
              "default graph must reject before cloning, not only at the later invalid spec");
        Lua(state,R"(
          assert(FinalizerCalls==0 and Save.Read("candidate").code=="not_found")
          collectgarbage("collect");collectgarbage("collect")
          assert(FinalizerCalls==0 and Save.Read("candidate").code=="not_found")
          assert(Actor.Find("old"):GetComponent("Stable").updates==1)
        )");
        Check(owner.IsValid() && old["updates"].cast<int>().value()==1,"candidate finalizer modified old world after rejection");
    }
    Check(ComponentManager::GetRuntimeErrorCount()==0,ComponentManager::GetLastRuntimeError());
    Check(lua_gettop(state)==0,"default finalizer validation leaked stack");
}

}
int main(){try{
    RejectedScenesKeepWorld();PositivePreparationAndWorldOverflow();StaticLifecycleAndDynamicAnimationEvents();HostNativeEditsPreserveOldInstances();RejectedDefaultFinalizersHaveNoDeferredEffects();
    std::cout<<"PASS scene component preparation: native schema/default graphs/old world/static lifecycle/animation events\n";return 0;
}catch(const std::exception &error){std::cerr<<"FAIL scene component preflight: "<<error.what()<<'\n';return 1;}}
