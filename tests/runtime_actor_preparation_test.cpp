#include "core/Engine.h"
#include "core/FrameClock.h"
#include "scripting/RuntimeActorPlan.h"
#include "scripting/ComponentManager.h"
#include "physics/RayCast.h"
#include "scene/Transform.h"
#include "shared/resources/ResourcePath.h"
#include "../src/engine/scripting/manager/Internal.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace ManagerDetail;
namespace fs=std::filesystem;
void Check(bool value,const std::string &message) { if(!value) throw std::runtime_error(message); }
void Lua(const char *code) {
    auto *state=Runtime().lua_state;const int top=lua_gettop(state);
    const int result=luaL_dostring(state,code);
    const auto error=result==LUA_OK?std::string():std::string(lua_tostring(state,-1));
    lua_settop(state,top);Check(result==LUA_OK,error);
}
void Put(const fs::path &path,const std::string &text) { fs::create_directories(path.parent_path());std::ofstream(path)<<text; }
struct Fixture {
    fs::path previous=ResourcePath::ResourcesRootPath();
    fs::path root=fs::temp_directory_path()/("pocket-runtime-actor-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() {
        Put(root/"game.config",R"({"initial_scene":"main"})");
        Put(root/"scenes/main.scene",R"({"actors":[{"uid":1,"name":"source","components":{"a_good":{"type":"Good"},"z_late":{"type":"Late"}}}]})");
        Put(root/"component_types/Good.lua",R"(
AUDIT={starts=0,destroys=0}
Good={health=100,starts=0,updates=0,late_updates=0,
 OnStart=function(self) self.starts=self.starts+1;AUDIT.starts=AUDIT.starts+1 end,
 OnUpdate=function(self) self.updates=self.updates+1 end,
 OnLateUpdate=function(self) self.late_updates=self.late_updates+1 end,
 OnDestroy=function(self)
  AUDIT.destroys=AUDIT.destroys+1
  local source=Actor.Find("source")
  if source then local good=source:GetComponent("Good");if good then good.health=good.health-1 end end
 end}
)");
        Put(root/"component_types/Late.lua","Late={value=3}");
        Put(root/"actor_templates/Broken.template",R"({"name":"broken","components":{"a_good":{"type":"Good"},"z_body":{"type":"Rigidbody","width":0}}})");
        Put(root/"actor_templates/Valid.template",R"({"name":"valid","components":{"logic":{"type":"Good"}}})");
        ResourcePath::SetResourcesRootPath(root);
    }
    ~Fixture() { ResourcePath::SetResourcesRootPath(previous);std::error_code ignored;fs::remove_all(root,ignored); }
};
Actor Candidate(Actor::UID uid,const std::string &name="candidate") {
    Actor actor;actor.uid=uid;actor.actor_name=name;
    actor.component_specs={{"a_good","Good",{}}};return actor;
}
struct Snapshot {
    std::size_t actors,actor_index,records,starts,activation,destroy,updates,late;
    std::uint64_t add_counter;
    bool operator==(const Snapshot &) const=default;
};
Snapshot State(const Engine &engine) {
    return {engine.GetRuntimeActors().size(),Runtime().actor_by_uid.size(),Runtime().actor_components.size(),
        Runtime().pending_on_start.size(),Runtime().pending_actor_uids_to_activate.size(),Runtime().pending_destroy_actor_uids.size(),
        Runtime().on_update_components.size(),Runtime().on_late_update_components.size(),Runtime().runtime_add_component_counter};
}
void PreparationRejectsWithoutPublication() {
    Fixture fixture;Engine engine;engine.InitializeHeadless();engine.StepSimulation();
    const auto before=State(engine);
    const auto source=ComponentManager::FindActorByName("source");
    const auto generation=source.cast<ActorHandle>().value().generation;
    const auto *physics=RayCast::GetPhysicsWorld();
    const auto reject=[&](Actor actor,const std::string &fragment) {
        std::string error;
        auto token=PrepareRuntimeActor(std::move(actor),error);
        Check(!token&&!error.empty()&&error.size()<=4096,error);
        Check(fragment.empty()||error.find(fragment)!=std::string::npos,"unexpected preparation diagnostic: "+error);
        Check(State(engine)==before,"preparation failure changed actor/index/lifecycle state");
        Check(RayCast::GetPhysicsWorld()==physics,"preparation failure created or replaced the physics world");
        Check(ComponentManager::FindActorByName("candidate").isNil(),"rejected actor is visible by name");
        Check(ComponentManager::FindActorByName("source").cast<ActorHandle>().value().generation==generation,"old actor generation changed");
        lua_gc(Runtime().lua_state,LUA_GCCOLLECT);
        Lua("assert(AUDIT.starts==1 and AUDIT.destroys==0 and Actor.Find('source'):GetComponent('Good').health==100)");
    };
    auto actor=Candidate(10000);actor.component_specs.push_back({"z_body","Rigidbody",{{"width",0.0}}});reject(actor,"dimensions");
    actor=Candidate(10000);actor.component_specs.push_back({"z_missing","Missing",{}});reject(actor,"not registered");
    actor=Candidate(10000);actor.component_specs.push_back({"a_good","Late",{}});reject(actor,"duplicate");
    actor=Candidate(10000);actor.parent_uid=1;reject(actor,"root");
    actor=Candidate(10000);actor.runtime_generation=1;reject(actor,"generation");
    actor=Candidate(1);reject(actor,"reserved");
    actor=Candidate(10000);actor.scene_backed=true;reject(actor,"provenance");
    actor=Candidate(10000);actor.component_specs.push_back({"z_transform","Transform",{{"x",std::numeric_limits<double>::infinity()}}});reject(actor,"finite");
    actor=Candidate(10000);actor.component_specs.push_back({"body","Rigidbody",{}});actor.component_specs.push_back({"transform","Transform",{{"x",1e9}}});reject(actor,"position");
    Lua("Late.owned=Rigidbody()");
    actor=Candidate(10000);actor.component_specs.push_back({"z_late","Late",{}});reject(actor,"userdata");
    Lua("Late.owned=nil");
    std::string error;
    auto discarded=PrepareRuntimeActor(Candidate(10000),error);
    Check(discarded&&!discarded->CommitStarted()&&!lua_gc(Runtime().lua_state,LUA_GCISRUNNING),error);
    Check(State(engine)==before,"successful preparation published an actor");
    {PreparedRuntimeActor moved(std::move(*discarded));discarded.reset();Check(!moved.CommitStarted()&&!lua_gc(Runtime().lua_state,LUA_GCISRUNNING),"moving token lost ownership");}
    Check(lua_gc(Runtime().lua_state,LUA_GCISRUNNING),"discarded token did not restore GC mode");
    lua_gc(Runtime().lua_state,LUA_GCCOLLECT);engine.StepSimulation();
    Check(State(engine)==before,"discarded token left pending lifecycle work");
    Lua("assert(AUDIT.starts==1 and AUDIT.destroys==0 and Actor.Find('source'):GetComponent('Good').health==100)");
}
void PublicationAndLifecycle() {
    Fixture fixture;Engine engine;engine.InitializeHeadless();engine.StepSimulation();
    auto *state=Runtime().lua_state;std::string error;
    auto deferred=PrepareRuntimeActor(Candidate(10000,"deferred"),error);Check(deferred!=nullptr,error);
    auto *spawned=PublishRuntimeActor(*deferred,*Runtime().scene_actors,true);
    engine.NotifyRuntimeActorPublished(*spawned);
    Check(deferred->CommitStarted()&&spawned->runtime_generation!=0,"publication did not enter commit/generation boundary");
    Check(ComponentManager::FindActorByName("deferred").cast<ActorHandle>().value().IsValid(),"published deferred actor is not findable");
    auto reference=ComponentManager::GetComponentByKey(10000,"a_good");
    Check(reference["starts"].cast<int>().value()==0&&reference["updates"].cast<int>().value()==0,"publication ran gameplay callbacks");
    Check(std::find(Runtime().actor_uids_sorted.begin(),Runtime().actor_uids_sorted.end(),10000)==Runtime().actor_uids_sorted.end(),"deferred actor was activated early");
    bool repeated=false;try{PublishRuntimeActor(*deferred,*Runtime().scene_actors,true);}catch(const std::exception&){repeated=true;}
    Check(repeated,"published token was reusable");deferred.reset();
    engine.StepSimulation();
    Check(reference["starts"].cast<int>().value()==1&&reference["updates"].cast<int>().value()==0,"idle deferred spawn timeline changed");
    engine.StepSimulation();Check(reference["updates"].cast<int>().value()==1,"deferred actor never entered updates");
    auto immediate=PrepareRuntimeActor(Candidate(10001,"immediate"),error);Check(immediate!=nullptr,error);
    const auto pending_before=Runtime().pending_on_start.size();
    auto *copy=PublishRuntimeActor(*immediate,*Runtime().scene_actors,false);engine.NotifyRuntimeActorPublished(*copy);
    Check(Runtime().pending_on_start.size()==pending_before+1&&Runtime().pending_actor_uids_to_activate.empty(),"immediate publication changed unrelated activation queues");
    auto immediate_ref=ComponentManager::GetComponentByKey(copy->uid,"a_good");
    Check(immediate_ref["starts"].cast<int>().value()==0&&immediate_ref["updates"].cast<int>().value()==0,"immediate publication invoked callbacks");
    immediate.reset();engine.StepSimulation();
    Check(immediate_ref["starts"].cast<int>().value()==1&&immediate_ref["updates"].cast<int>().value()==1,"duplicate-style immediate lifecycle timing changed");
    auto duplicate=PrepareRuntimeActor(Candidate(10001),error);Check(!duplicate&&error.find("registered")!=std::string::npos,"live UID collision accepted");
    // Removing an earlier actor must not let a later publication reuse its
    // physical list position as an order value already held by a live actor.
    Check(engine.DeleteRuntimeActorByUID(10000),"could not create actor-order gap");
    std::size_t largest_order=0;
    for(const auto &[uid,order]:Runtime().actor_order_by_uid) largest_order=std::max(largest_order,order);
    auto older=PrepareRuntimeActor(Candidate(10002,"deferred_older"),error);Check(older!=nullptr,error);
    auto *a=PublishRuntimeActor(*older,*Runtime().scene_actors,true);engine.NotifyRuntimeActorPublished(*a);older.reset();
    Check(Runtime().actor_order_by_uid.at(a->uid)>largest_order,"publication reused a pruned actor-order position");
    const auto activation_before=Runtime().pending_actor_uids_to_activate;
    auto newer=PrepareRuntimeActor(Candidate(10003,"immediate_newer"),error);Check(newer!=nullptr,error);
    auto *b=PublishRuntimeActor(*newer,*Runtime().scene_actors,false);engine.NotifyRuntimeActorPublished(*b);newer.reset();
    Check(Runtime().actor_order_by_uid.at(a->uid)<Runtime().actor_order_by_uid.at(b->uid)&&
          Runtime().pending_actor_uids_to_activate==activation_before,"immediate publication disturbed pending older actor");
    auto a_ref=ComponentManager::GetComponentByKey(a->uid,"a_good");
    auto b_ref=ComponentManager::GetComponentByKey(b->uid,"a_good");
    engine.StepSimulation();
    Check(a_ref["starts"].cast<int>().value()==1&&a_ref["updates"].cast<int>().value()==0&&b_ref["updates"].cast<int>().value()==1,
          "mixed deferred/immediate publication changed first-tick lifecycle timing");
    for(const auto *list:{&Runtime().on_update_components,&Runtime().on_late_update_components}) {
        std::vector<Actor::UID> order;
        for(const auto &entry:*list) if(entry.actor_uid==a->uid||entry.actor_uid==b->uid) order.push_back(entry.actor_uid);
        Check(order==std::vector<Actor::UID>{a->uid,b->uid},"late deferred activation reordered callbacks after immediate publication");
    }
    const auto a_position=std::find(Runtime().actor_uids_sorted.begin(),Runtime().actor_uids_sorted.end(),a->uid);
    const auto b_position=std::find(Runtime().actor_uids_sorted.begin(),Runtime().actor_uids_sorted.end(),b->uid);
    Check(a_position!=Runtime().actor_uids_sorted.end() && b_position!=Runtime().actor_uids_sorted.end() && a_position<b_position,
          "mixed activation reordered animation/physics traversal");
    engine.StepSimulation();
    Check(a_ref["updates"].cast<int>().value()==1&&a_ref["late_updates"].cast<int>().value()==1&&b_ref["updates"].cast<int>().value()==2,
          "mixed-mode actors did not activate exactly once");
    Check(lua_gettop(state)==0,"actor preparation/publication leaked Lua stack");
    Check(ComponentManager::GetRuntimeErrorCount()==0,ComponentManager::GetLastRuntimeError());
}
void PublicSpawnFailuresStayAtomic() {
    Fixture fixture;Engine engine;engine.InitializeHeadless();engine.StepSimulation();
    const auto before=State(engine);
    Lua("ok,error_message=pcall(function() Actor.Instantiate('Broken') end);assert(not ok and Actor.Find('broken')==nil)");
    Check(State(engine)==before,"failed public Instantiate left actor or pending records");
    engine.StepSimulation();
    Lua("assert(AUDIT.starts==1 and AUDIT.destroys==0 and Actor.Find('source'):GetComponent('Good').health==100)");
    Lua("Late.owned=Rigidbody()");
    Actor::UID uid=0;
    Check(!engine.DuplicateRuntimeActorByUID(1,&uid)&&uid==0,"failed duplicate returned success/UID");
    Check(State(engine)==before&&ComponentManager::FindActorByName("source (copy)").isNil(),"failed public Duplicate left a partial actor");
    engine.StepSimulation();
    Lua("assert(AUDIT.starts==1 and AUDIT.destroys==0 and Actor.Find('source'):GetComponent('Good').health==100);Late.owned=nil");
    Check(engine.DuplicateRuntimeActorByUID(1,&uid)&&uid>=Actor::kRuntimeGeneratedUIDStart,"valid duplicate failed after rejected attempt");
    Check(ComponentManager::FindActorByName("source (copy)").cast<ActorHandle>().value().IsValid(),"valid duplicate is not visible");
    engine.StepSimulation();Lua("assert(AUDIT.starts==2 and AUDIT.destroys==0)");
    Check(ComponentManager::GetRuntimeErrorCount()==0,ComponentManager::GetLastRuntimeError());
}
void FatalPublicationNotificationStopsFrame() {
    Fixture fixture; Engine engine; engine.InitializeHeadless(); engine.StepSimulation();
    Actor::UID duplicate=0; Check(engine.DuplicateRuntimeActorByUID(1,&duplicate),"could not set up fatal-boundary observer");
    engine.StepSimulation();
    auto *state=Runtime().lua_state;
    lua_pushlightuserdata(state,&engine);
    lua_pushcclosure(state,[](lua_State *vm)->int {
        auto *host=static_cast<Engine *>(lua_touserdata(vm,lua_upvalueindex(1)));
        host->ReportRuntimeActorPublicationFailure("simulated actor publication failure");
        return 0;
    },1); lua_setglobal(state,"fault_marker");
    Lua(R"(
      source_ref=Actor.Find('source'):GetComponent('Good')
      copy_ref=Actor.Find('source (copy)'):GetComponent('Good')
      before_copy_updates=copy_ref.updates;before_late=source_ref.late_updates
      source_ref.OnUpdate=function(self) self.updates=self.updates+1;fault_marker() end
    )");
    const int frame=FrameClock::GetFrameNumber();
    engine.StepSimulation();
    Check(engine.RequiresSceneReset()&&!engine.IsRunning()&&FrameClock::GetFrameNumber()==frame,
          "fatal publication notification did not stop the frame");
    Check(engine.GetLastRuntimeActorError().find("simulated actor publication failure")!=std::string::npos,
          "fatal actor diagnostic was lost");
    Lua("assert(copy_ref.updates==before_copy_updates and source_ref.late_updates==before_late)");
    engine.StepSimulation();
    Check(FrameClock::GetFrameNumber()==frame,"stopped runtime resumed simulation");
}
}
int main(){try{
    PreparationRejectsWithoutPublication();PublicationAndLifecycle();PublicSpawnFailuresStayAtomic();FatalPublicationNotificationStopsFrame();
    std::cout<<"RuntimeActorPlan: unpublished failure cleanup, UID/generation boundary and lifecycle publication passed\n";return 0;
}catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}}
