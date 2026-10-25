#include "core/Engine.h"
#include "scripting/ComponentManager.h"
#include "scripting/EventBus.h"
#include "scene/Transform.h"
#include "physics/Rigidbody.h"
#include "shared/resources/ResourcePath.h"
#include "../src/engine/scripting/manager/Internal.h"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace ManagerDetail;
using namespace SceneFormat;
namespace fs=std::filesystem;
void Check(bool value,const std::string &message){if(!value)throw std::runtime_error(message);}
void Lua(const char *code){const auto top=lua_gettop(Runtime().lua_state);const int status=luaL_dostring(Runtime().lua_state,code);const std::string error=status==LUA_OK?"":lua_tostring(Runtime().lua_state,-1);lua_settop(Runtime().lua_state,top);Check(status==LUA_OK,error);}
void Put(const fs::path &path,const std::string &value){fs::create_directories(path.parent_path());std::ofstream(path)<<value;}
struct Fixture {
    fs::path previous=ResourcePath::ResourcesRootPath();
    fs::path root=fs::temp_directory_path()/("pocket-scene-edit-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture(){
        Put(root/"game.config",R"({"initial_scene":"main"})");
        Put(root/"scenes/main.scene",R"({"actors":[{"uid":1,"name":"source","components":{"logic":{"type":"Good"},"other":{"type":"Good"},"transform":{"type":"Transform","x":3}}},{"uid":2,"name":"parent","components":{"transform":{"type":"Transform","x":10}}}]})");
        Put(root/"component_types/Good.lua",R"(AUDIT={starts=0,destroys=0}; Good={health=100,updates=0,OnStart=function(self) AUDIT.starts=AUDIT.starts+1 end,OnUpdate=function(self)self.updates=self.updates+1 end,OnDestroy=function(self)AUDIT.destroys=AUDIT.destroys+1;local a=Actor.Find('source');if a then local g=a:GetComponent('Good');if g then g.health=g.health-1 end end end})");
        Put(root/"component_types/Bad.lua","Bad={}");
        ResourcePath::SetResourcesRootPath(root);
    }
    ~Fixture(){ResourcePath::SetResourcesRootPath(previous);std::error_code ignored;fs::remove_all(root,ignored);}
};
SceneEditCommand Batch(std::initializer_list<SceneMutation> commands){SceneEditCommand result;result.mutations=commands;return result;}
SceneMutation Name(Actor::UID uid,const char *name){return {SetActorNameMutation{uid,name}};}
SceneMutation Property(Actor::UID uid,const char *key,const char *field,Actor::ComponentPropertyValue value){return {SetComponentPropertyMutation{uid,key,field,std::move(value)}};}
struct Snapshot {
    std::size_t actors,components,starts,activate,destroy,update,late;
    bool operator==(const Snapshot &)const=default;
};
Snapshot State(const Engine &engine){std::size_t components=0;for(const auto &[uid,records]:Runtime().actor_components)components+=records.size();return {engine.GetRuntimeActors().size(),components,Runtime().pending_on_start.size(),Runtime().pending_actor_uids_to_activate.size(),Runtime().pending_destroy_actor_uids.size(),Runtime().on_update_components.size(),Runtime().on_late_update_components.size()};}
void RejectionsHaveNoResidue(){
    Fixture fixture;Engine engine;engine.InitializeHeadless();engine.StepSimulation();
    auto *original=engine.GetRuntimeActorByUID(1);const auto generation=original->runtime_generation;
    const auto original_ref=ComponentManager::GetComponentByKey(1,"logic");const auto baseline=State(engine);
    const auto reject=[&](const SceneEditCommand &command){
        Check(!engine.ApplySceneEditCommand(command),"invalid edit accepted");Check(!engine.GetLastSceneEditError().empty(),"missing edit diagnostic");Check(!engine.RequiresSceneReset(),"ordinary rejection requires reset");
        Check(State(engine)==baseline,"rejection changed actor/component/lifecycle state");Check(engine.GetRuntimeActorByUID(1)==original&&original->runtime_generation==generation&&original->actor_name=="source","rejection changed old actor");
        Check(AreSameLuaRef(original_ref,ComponentManager::GetComponentByKey(1,"logic")),"rejection replaced old component");
        Lua("assert(AUDIT.starts==2 and AUDIT.destroys==0 and Actor.Find('source'):GetComponentByKey('logic').health==100)");
    };
    reject(Batch({Name(1,"partial"),SceneMutation{DeleteActorMutation{999}}}));
    ActorRecord broken;broken.name="broken";broken.component_specs={{"a_good","Good",{}},{"z_body","Rigidbody",{{"width",0.0}}}};
    reject(Batch({SceneMutation{CreateActorMutation{3,std::nullopt,broken}}}));
    Check(engine.GetRuntimeActorByUID(3)==nullptr,"failed create remains visible");
    reject(Batch({SceneMutation{AddComponentMutation{1,{"body","Rigidbody",{{"width",0.0}}}}}}));
    Check(!FindComponentSpec(original->component_specs,"body"),"failed add left ghost spec");
    Lua("Bad.owned=Rigidbody()");
    reject(Batch({SceneMutation{AddComponentMutation{1,{"bad","Bad",{}}}}}));
    Lua("Bad.owned=nil");
    reject(Batch({Name(1,"partial"),Property(1,"transform","x",1e300)}));
    reject(Batch({SceneMutation{SetActorParentMutation{1,2}},SceneMutation{SetActorParentMutation{2,1}}}));
    reject(Batch({SceneMutation{AddComponentMutation{1,{"map","Tilemap",{}}}},SceneMutation{AddComponentMutation{1,{"body","Rigidbody",{}}}}}));
    reject(Batch({SceneMutation{SetComponentTypeMutation{1,"logic","Missing"}}}));
    engine.StepSimulation();Lua("assert(AUDIT.starts==2 and AUDIT.destroys==0)");
}
void OrderedSuccessKeepsIdentity(){
    Fixture fixture;Engine engine;engine.InitializeHeadless();engine.StepSimulation();
    auto *source=engine.GetRuntimeActorByUID(1);auto old_logic=ComponentManager::GetComponentByKey(1,"logic");auto old_other=ComponentManager::GetComponentByKey(1,"other");
    Lua("Actor.Find('source'):GetComponentByKey('logic').owned=Rigidbody();Actor.Find('source'):GetComponentByKey('logic').counter=42");
    ActorRecord created;created.name="created";created.parent_uid=2;created.component_specs={{"logic","Good",{}}};
    const auto command=Batch({SceneMutation{CreateActorMutation{3,1,created}},SceneMutation{AddComponentMutation{3,{"extra","Good",{}}}},SceneMutation{RenameComponentMutation{3,"extra","renamed"}},Property(3,"renamed","health",77),Name(3,"finished"),Property(1,"logic","health",83)});
    Check(engine.ApplySceneEditCommand(command),engine.GetLastSceneEditError());
    Check(engine.GetRuntimeActorByUID(1)==source&&AreSameLuaRef(old_logic,ComponentManager::GetComponentByKey(1,"logic")),"property edit replaced old identity");
    Lua("assert(Actor.Find('source'):GetComponentByKey('logic').counter==42 and Actor.Find('source'):GetComponentByKey('logic').owned and Actor.Find('source'):GetComponentByKey('logic').health==83)");
    Check(engine.GetRuntimeActorByUID(3)->parent_uid==2,"new nonroot actor lost parent");
    Check(ComponentManager::GetComponentByKey(3,"renamed")["health"].cast<int>().value()==77,"ordered create/add/rename/property did not reach final state");
    std::vector<Actor::UID> order;for(const auto &actor:engine.GetRuntimeActors())if(!actor.runtime_destroyed)order.push_back(actor.uid);Check(order==std::vector<Actor::UID>({1,3,2}),"insert-after scene order changed");
    Check(engine.ApplySceneEditCommand(Batch({SceneMutation{RenameComponentMutation{1,"logic","temporary"}},SceneMutation{RenameComponentMutation{1,"other","logic"}},SceneMutation{RenameComponentMutation{1,"temporary","other"}}})),engine.GetLastSceneEditError());
    Check(AreSameLuaRef(old_logic,ComponentManager::GetComponentByKey(1,"other"))&&AreSameLuaRef(old_other,ComponentManager::GetComponentByKey(1,"logic")),"batch key exchange lost identity");
    engine.StepSimulation();Lua("assert(AUDIT.starts==4 and AUDIT.destroys==0)");
}
void HierarchyAndFinalNativeValidation(){
    Fixture fixture;Engine engine;engine.InitializeHeadless();engine.StepSimulation();
    auto transform=ComponentManager::GetComponentByKey(1,"transform");auto *native=transform.cast<Transform *>().value();
    Check(engine.ApplySceneEditCommand(Batch({SceneMutation{SetActorParentMutation{1,2}}})),engine.GetLastSceneEditError());
    Check(std::abs(native->x+7)<0.001f&&std::abs(native->world_x-3)<0.001f,"reparent did not preserve world pose");
    Check(engine.ApplySceneEditCommand(Batch({Property(2,"transform","x",20.0),Property(1,"transform","x",2.0),SceneMutation{AddComponentMutation{1,{"body","Rigidbody",{{"gravity_scale",0.0}}}}}})),engine.GetLastSceneEditError());
    Check(AreSameLuaRef(transform,ComponentManager::GetComponentByKey(1,"transform"))&&std::abs(native->world_x-22)<0.001f,"hierarchy edits lost Transform identity/final world pose");
    engine.StepSimulation();auto *body=ComponentManager::GetComponentByKey(1,"body").cast<Rigidbody *>().value();Check(std::abs(body->GetPosition().x-22)<0.001f,"linked body did not receive final prepared pose");
    const auto snapshot=State(engine);const float previous=native->x;
    Check(!engine.ApplySceneEditCommand(Batch({Property(2,"transform","x",1e8),Property(1,"transform","x",1e8)})),"overflowing inherited body pose accepted");
    Check(!engine.RequiresSceneReset()&&native->x==previous&&State(engine)==snapshot,"derived-pose rejection mutated runtime");
    Check(engine.ApplySceneEditCommand(Batch({SceneMutation{DeleteActorMutation{2}}})),engine.GetLastSceneEditError());
    Check(engine.GetRuntimeActorByUID(1)->parent_uid==0&&native->x==previous,"delete did not detach direct child preserving authored local pose");
}
double AuthoredNumber(const Actor &actor,const char *key,const char *name) {
    const auto *spec=FindComponentSpec(actor.component_specs,key);Check(spec!=nullptr,"missing authored component");
    for(const auto &property:spec->overrides)if(property.name==name) {
        if(auto *value=std::get_if<double>(&property.value))return *value;
        if(auto *value=std::get_if<int>(&property.value))return *value;
    }
    return 0;
}
void LinkedPoseSurvivesBodyRebuild(){
    Fixture fixture;Engine engine;engine.InitializeHeadless();engine.StepSimulation();
    Check(engine.ApplySceneEditCommand(Batch({SceneMutation{AddComponentMutation{1,{"body","Rigidbody",{{"x",3.0},{"gravity_scale",0.0}}}}}})),engine.GetLastSceneEditError());
    engine.StepSimulation();
    auto body=ComponentManager::GetComponentByKey(1,"body");
    const auto transform=ComponentManager::GetComponentByKey(1,"transform");
    Check(engine.ApplySceneEditCommand(Batch({Property(1,"transform","x",5.0)})),engine.GetLastSceneEditError());
    Check(AreSameLuaRef(body,ComponentManager::GetComponentByKey(1,"body")),"Transform edit replaced linked body userdata");
    Check(AuthoredNumber(*engine.GetRuntimeActorByUID(1),"body","x")==5,"Transform edit left stale Rigidbody authored pose");
    Check(engine.ApplySceneEditCommand(Batch({Property(1,"transform","x",6.0),Property(1,"body","y",2.0)})),engine.GetLastSceneEditError());
    Check(std::abs(transform.cast<Transform *>().value()->world_x-6)<0.001f&&std::abs(transform.cast<Transform *>().value()->world_y-2)<0.001f,"ordered linked pose edit discarded an earlier axis");
    Check(engine.ApplySceneEditCommand(Batch({Property(1,"transform","x",5.0)})),engine.GetLastSceneEditError());
    Check(engine.ApplySceneEditCommand(Batch({Property(1,"body","width",2.0)})),engine.GetLastSceneEditError());
    engine.StepSimulation();
    auto *native=ComponentManager::GetComponentByKey(1,"body").cast<Rigidbody *>().value();
    Check(std::abs(native->GetPosition().x-5)<0.001f&&std::abs(transform.cast<Transform *>().value()->world_x-5)<0.001f,"body rebuild teleported to an old authored pose");
    Check(engine.ApplySceneEditCommand(Batch({Property(2,"transform","rotation",90.0),SceneMutation{SetActorParentMutation{1,2}}})),engine.GetLastSceneEditError());
    body=ComponentManager::GetComponentByKey(1,"body");
    const auto check_authored=[&] {
        const auto *pose=ComponentManager::GetComponentByKey(1,"transform").cast<Transform *>().value();
        const auto &actor=*engine.GetRuntimeActorByUID(1);
        Check(std::abs(AuthoredNumber(actor,"body","x")-pose->world_x)<0.001&&std::abs(AuthoredNumber(actor,"body","y")-pose->world_y)<0.001&&std::abs(AuthoredNumber(actor,"body","rotation")-pose->world_rotation)<0.001,"linked body authored pose differs from final world pose");
    };
    check_authored();
    Check(std::abs(body.cast<Rigidbody *>().value()->GetPosition().x-5)<0.001f,"reparent moved the live body");
    Check(engine.ApplySceneEditCommand(Batch({Property(1,"body","width",3.0)})),engine.GetLastSceneEditError());
    engine.StepSimulation();check_authored();
    Check(std::abs(ComponentManager::GetComponentByKey(1,"body").cast<Rigidbody *>().value()->GetPosition().x-5)<0.001f,"nonroot body rebuild used local coordinates as world coordinates");
    body=ComponentManager::GetComponentByKey(1,"body");
    Check(engine.ApplySceneEditCommand(Batch({Property(2,"transform","rotation",45.0)})),engine.GetLastSceneEditError());
    Check(AreSameLuaRef(body,ComponentManager::GetComponentByKey(1,"body")),"ancestor pose edit replaced descendant body");check_authored();
    const auto *pose=ComponentManager::GetComponentByKey(1,"transform").cast<Transform *>().value();
    const float expected_x=pose->world_x,expected_y=pose->world_y,expected_rotation=pose->world_rotation;
    Check(engine.ApplySceneEditCommand(Batch({Property(1,"body","width",4.0)})),engine.GetLastSceneEditError());
    engine.StepSimulation();check_authored();
    auto scene=LoadSceneAsset("main");
    for(auto &record:scene.actors) {
        const auto *actor=engine.GetRuntimeActorByUID(record.uid);Check(actor!=nullptr,"missing actor before save");
        record.name=actor->actor_name;record.parent_uid=actor->parent_uid;record.component_specs=actor->component_specs;
    }
    Check(SaveSceneAsset(scene),"could not save linked authored poses");
    Check(engine.LoadSceneAsset(LoadSceneAsset("main")),engine.GetLastSceneLoadError());engine.StepSimulation();
    const auto position=ComponentManager::GetComponentByKey(1,"body").cast<Rigidbody *>().value()->GetPosition();
    pose=ComponentManager::GetComponentByKey(1,"transform").cast<Transform *>().value();
    Check(std::abs(position.x-expected_x)<0.001f&&std::abs(position.y-expected_y)<0.001f&&std::abs(pose->world_rotation-expected_rotation)<0.001f,"saved parented body reloaded at a different world pose");
    Check(engine.ApplySceneEditCommand(Batch({SceneMutation{DeleteActorMutation{2}}})),engine.GetLastSceneEditError());check_authored();
    const float detached_x=pose->x,detached_y=pose->y;
    Check(engine.ApplySceneEditCommand(Batch({Property(1,"body","width",5.0)})),engine.GetLastSceneEditError());engine.StepSimulation();
    const auto detached=ComponentManager::GetComponentByKey(1,"body").cast<Rigidbody *>().value()->GetPosition();
    Check(std::abs(detached.x-detached_x)<0.001f&&std::abs(detached.y-detached_y)<0.001f,"parent deletion left stale descendant body spec");
}
void DeepHierarchyCachesRespectOrderedMutations(){
    Fixture fixture;constexpr int count=1000;
    std::ofstream scene(fixture.root/"scenes/main.scene");scene<<"{\"actors\":[";
    for(int index=1;index<=count;++index) {
        if(index>1)scene<<',';
        scene<<"{\"uid\":"<<index<<",\"name\":\"deep\",\"parent_uid\":"<<(index-1)<<",\"components\":{\"transform\":{\"type\":\"Transform\",\"x\":1}";
        if(index==count)scene<<",\"body\":{\"type\":\"Rigidbody\",\"x\":1000,\"gravity_scale\":0}";
        scene<<"}}";
    }
    scene<<"]}";scene.close();
    Engine engine;engine.InitializeHeadless();engine.StepSimulation();
    const auto body=ComponentManager::GetComponentByKey(count,"body");
    const auto leaf=ComponentManager::GetComponentByKey(count,"transform");
    Check(engine.ApplySceneEditCommand(Batch({Property(1,"transform","x",2.0),SceneMutation{SetActorParentMutation{count,1}},Property(1,"transform","x",3.0),Name(count,"deep-renamed")})),engine.GetLastSceneEditError());
    Check(AreSameLuaRef(body,ComponentManager::GetComponentByKey(count,"body")),"deep hierarchy pose edit replaced body");
    Check(std::abs(leaf.cast<Transform *>().value()->world_x-1002)<0.001f&&AuthoredNumber(*engine.GetRuntimeActorByUID(count),"body","x")==1002,"hierarchy memo retained a previous mutation's pose");
    Check(!engine.ApplySceneEditCommand(Batch({Name(count,"partial"),SceneMutation{SetActorParentMutation{1,count}}})),"deep hierarchy cycle accepted");
    Check(!engine.RequiresSceneReset()&&engine.GetRuntimeActorByUID(count)->actor_name=="deep-renamed"&&std::abs(leaf.cast<Transform *>().value()->world_x-1002)<0.001f,"deep cycle rejection changed old state");
    Check(engine.ApplySceneEditCommand(Batch({Property(1,"transform","rotation",90.0)})),engine.GetLastSceneEditError());
    Check(std::abs(leaf.cast<Transform *>().value()->world_x-3)<0.001f&&std::abs(leaf.cast<Transform *>().value()->world_y+999)<0.001f,"hierarchy memo survived across commands");
}
void PendingParentDoesNotBlockIndependentReparent(){
    for(bool during_destroy:{false,true}) {
        Fixture fixture;
        Put(fixture.root/"scenes/main.scene",R"({"actors":[{"uid":1,"name":"dyingParent","components":{"logic":{"type":"Dying"},"transform":{"type":"Transform","x":10}}},{"uid":2,"name":"survivor","parent_uid":1,"components":{"transform":{"type":"Transform","x":2}}},{"uid":3,"name":"otherParent","components":{"transform":{"type":"Transform","x":20}}},{"uid":4,"name":"otherChild","parent_uid":3,"components":{"transform":{"type":"Transform","x":4}}}]})");
        Put(fixture.root/"component_types/Dying.lua",R"(Dying={OnDestroy=function(self) DestroyCalls=DestroyCalls+1;if ReparentDuringDestroy then ReparentSucceeded=Actor.Find('otherChild'):SetParent(nil) end end})");
        Engine engine;engine.InitializeHeadless();engine.StepSimulation();
        Lua(during_destroy?"DestroyCalls=0;ReparentDuringDestroy=true;ReparentSucceeded=false":"DestroyCalls=0;ReparentDuringDestroy=false;ReparentSucceeded=false");
        auto *survivor=engine.GetRuntimeActorByUID(2);const auto survivor_specs=survivor->component_specs;
        const auto other=ComponentManager::GetComponentByKey(4,"transform");
        Lua("Actor.Destroy(Actor.Find('dyingParent'))");
        Check(!engine.ApplySceneEditCommand(Batch({SceneMutation{SetActorParentMutation{4,1}}})),"new link to a pending-destroy parent was accepted");
        Check(engine.GetRuntimeActorByUID(4)->parent_uid==3&&!engine.RequiresSceneReset(),"invalid new parent link changed old state");
        if(!during_destroy) {
            Lua("ReparentSucceeded=Actor.Find('otherChild'):SetParent(nil)");
            Lua("assert(ReparentSucceeded and DestroyCalls==0)");
            Check(Runtime().pending_destroy_actor_uids.contains(1),"independent reparent prematurely finalized pending destruction");
        }
        engine.StepSimulation();
        Lua("assert(ReparentSucceeded and DestroyCalls==1)");
        Check(!engine.RequiresSceneReset()&&ComponentManager::GetRuntimeErrorCount()==0,"ordinary OnDestroy reparent failed");
        Check(engine.GetRuntimeActorByUID(2)==survivor&&survivor->parent_uid==1,"virtual missing-parent normalization published an unrelated parent edit");
        Check(survivor->component_specs.size()==survivor_specs.size(),"virtual normalization changed unrelated specs");
        for(std::size_t index=0;index<survivor_specs.size();++index) {
            const auto &before=survivor_specs[index];const auto &after=survivor->component_specs[index];
            Check(before.key==after.key&&before.type==after.type&&before.overrides.size()==after.overrides.size(),"virtual normalization changed unrelated spec shape");
            for(std::size_t field=0;field<before.overrides.size();++field)Check(before.overrides[field].name==after.overrides[field].name&&before.overrides[field].value==after.overrides[field].value,"virtual normalization changed unrelated authored field");
        }
        Check(engine.GetRuntimeActorByUID(4)->parent_uid==0&&AreSameLuaRef(other,ComponentManager::GetComponentByKey(4,"transform"))&&std::abs(other.cast<Transform *>().value()->world_x-24)<0.001f,"independent reparent lost pose or identity");
    }
}
void RenamingPreservesEventSubscriptions(){
    for(bool swap:{false,true}) {
        Fixture fixture;Engine engine;engine.InitializeHeadless();engine.StepSimulation();
        Lua(R"(
            event_a=Actor.Find('source'):GetComponentByKey('logic');event_b=Actor.Find('source'):GetComponentByKey('other')
            event_a.hits=0;event_b.hits=0
            function named_a(self,event)self.hits=self.hits+1 end
            function named_b(self,event)self.hits=self.hits+100 end
            local a_delta,b_delta=10,1000
            anon_a=function(self,event)self.hits=self.hits+a_delta end
            anon_b=function(self,event)self.hits=self.hits+b_delta end
            Event.Subscribe('active',event_a,named_a);Event.Subscribe('active',event_a,anon_a)
            Event.Subscribe('active',event_b,named_b);Event.Subscribe('active',event_b,anon_b)
        )");
        EventBus::ApplyPendingOperations();
        Lua(R"(
            Event.Subscribe('pending',event_a,named_a);Event.Subscribe('pending',event_a,anon_a)
            Event.Subscribe('pending',event_b,named_b);Event.Subscribe('pending',event_b,anon_b)
        )");
        const auto command=swap?Batch({SceneMutation{RenameComponentMutation{1,"logic","temporary"}},SceneMutation{RenameComponentMutation{1,"other","logic"}},SceneMutation{RenameComponentMutation{1,"temporary","other"}}}):Batch({SceneMutation{RenameComponentMutation{1,"logic","renamed"}}});
        Check(engine.ApplySceneEditCommand(command),engine.GetLastSceneEditError());
        Lua("Event.Publish('active',{});Event.Publish('pending',{});assert(event_a.hits==11 and event_b.hits==1100,'rename lost active named/anonymous callbacks or activated pending callbacks')");
        EventBus::ApplyPendingOperations();
        Lua("Event.Publish('pending',{});assert(event_a.hits==22 and event_b.hits==2200,'rename lost pending named/anonymous callbacks');Event.Unsubscribe('active',event_a,anon_a);Event.Unsubscribe('active',event_b,anon_b)");
        EventBus::ApplyPendingOperations();
        Lua("Event.Publish('active',{});assert(event_a.hits==23 and event_b.hits==2300,'renamed subscription identity no longer unsubscribes')");
    }
}
Engine *event_rename_engine=nullptr;
bool RenameDuringPublish(){return event_rename_engine->ApplySceneEditCommand(Batch({SceneMutation{RenameComponentMutation{1,"logic","during_event"}}}));}
void RenameKeepsTeardownOperationsAndRejectsDispatch(){
    {
        Fixture fixture;Engine engine;engine.InitializeHeadless();engine.StepSimulation();
        Lua(R"(
            survivor=Actor.Find('source'):GetComponentByKey('logic');survivor.hits=0
            function named(self,event)self.hits=self.hits+1 end
            anonymous=function(self,event)self.hits=self.hits+10 end
            late=function(self,event)self.hits=self.hits+100 end
            Event.Subscribe('active',survivor,named);Event.Subscribe('active',survivor,anonymous)
            Actor.Find('source'):GetComponentByKey('other').OnDestroy=function(self)
                Event.Subscribe('late',survivor,late);Event.Unsubscribe('active',survivor,named)
            end
        )");
        EventBus::ApplyPendingOperations();
        Check(engine.ApplySceneEditCommand(Batch({SceneMutation{DeleteComponentMutation{1,"other"}},SceneMutation{RenameComponentMutation{1,"logic","renamed"}}})),engine.GetLastSceneEditError());
        Lua("Event.Publish('active',{});assert(survivor.hits==11)");EventBus::ApplyPendingOperations();
        Lua("Event.Publish('active',{});Event.Publish('late',{});assert(survivor.hits==121,'rename discarded OnDestroy queued operations')");
    }
    {
        Fixture fixture;Engine engine;engine.InitializeHeadless();engine.StepSimulation();event_rename_engine=&engine;
        luabridge::getGlobalNamespace(Runtime().lua_state).addFunction("AttemptEventRename",&RenameDuringPublish);
        Lua(R"(
            target=Actor.Find('source'):GetComponentByKey('logic');target.hits=0
            function callback(self,event)assert(not AttemptEventRename());self.hits=self.hits+1 end
            Event.Subscribe('active',target,callback)
        )");
        EventBus::ApplyPendingOperations();Lua("Event.Publish('active',{});Event.Publish('active',{});assert(target.key=='logic' and target.hits==2)");
        Check(!engine.RequiresSceneReset()&&ComponentManager::GetRuntimeErrorCount()==0,"dispatch-time rename rejection mutated the world");
        event_rename_engine=nullptr;
    }
}
void FailedCommitRequiresReset(){
    Fixture fixture;Engine engine;engine.InitializeHeadless();engine.StepSimulation();
    Lua("setmetatable(Actor.Find('source'):GetComponentByKey('logic'),{__newindex=function()error('setter commit failed')end})");
    Check(!engine.ApplySceneEditCommand(Batch({Property(1,"logic","new_field",3)})),"throwing custom setter unexpectedly succeeded");
    Check(engine.RequiresSceneReset()&&!engine.GetLastSceneEditError().empty(),"post-publication callback failure was reported as recoverable");
}
}
int main(){try{RejectionsHaveNoResidue();OrderedSuccessKeepsIdentity();HierarchyAndFinalNativeValidation();LinkedPoseSurvivesBodyRebuild();DeepHierarchyCachesRespectOrderedMutations();PendingParentDoesNotBlockIndependentReparent();RenamingPreservesEventSubscriptions();RenameKeepsTeardownOperationsAndRejectsDispatch();FailedCommitRequiresReset();std::cout<<"scene_edit_runtime_test passed\n";return 0;}catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}}
