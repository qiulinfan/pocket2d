#include "core/Engine.h"
#include "scene/ActorHandle.h"
#include "scripting/ComponentManager.h"
#include "scripting/EventBus.h"
#include "shared/resources/ResourcePath.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include "../src/engine/scripting/manager/Internal.h"
#include <chrono>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value,const std::string &message){if(!value)throw std::runtime_error(message);}
struct Fixture {
    std::filesystem::path previous=ResourcePath::ResourcesRootPath();
    std::filesystem::path root=std::filesystem::temp_directory_path()/
        ("pocket-reload-runtime-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture(){
        std::filesystem::create_directories(root/"component_types");std::filesystem::create_directories(root/"scenes");
        std::ofstream(root/"game.config")<<R"({"initial_scene":"test"})";
        std::ofstream(root/"scenes/test.scene")<<R"({"actors":[
          {"uid":1,"name":"a","components":{"logic":{"type":"A"}}},
          {"uid":2,"name":"b","components":{"logic":{"type":"A"}}},
          {"uid":3,"name":"other","components":{"logic":{"type":"B"}}}]})";
        Source("A",R"(A={count=1,starts=0,pulses=0,nested={value=3},OnStart=function(self)
          self.starts=self.starts+1;self.alias=self.nested;self.self_reference=self;self.vector=Vector2(2,4)
          Event.Subscribe("pulse",self,self.OnPulse)
        end,OnUpdate=function(self)self.count=self.count+1 end,
        OnPulse=function(self,e)self.pulses=self.pulses+1 end})");
        Source("B","B={count=0,OnUpdate=function(self)self.count=self.count+1 end}");
        ResourcePath::SetResourcesRootPath(root);
    }
    void Source(const char *type,const std::string &code){std::ofstream(root/"component_types"/(std::string(type)+".lua"))<<code;}
    ~Fixture(){ResourcePath::SetResourcesRootPath(previous);std::error_code error;std::filesystem::remove_all(root,error);}
};
void Lua(lua_State *state,const char *code){
    const int top=lua_gettop(state);const int status=luaL_dostring(state,code);
    const std::string message=status==LUA_OK?"":lua_tostring(state,-1);lua_settop(state,top);Check(status==LUA_OK,message);
}
std::string Error(const ScriptReloadResult &result){return result.diagnostics.empty()?"no diagnostic":result.diagnostics.front().message;}
}
void ExistingReloadCases(){
    Fixture fixture;Engine engine;engine.InitializeHeadless();engine.StepSimulation();engine.StepSimulation();
    auto *state=ComponentManager::GetComponentByKey(1,"logic").state();
    Lua(state,R"(old_a=Actor.Find("a"):GetComponent("A");old_b=Actor.Find("b"):GetComponent("A")
      old_vector=old_a.vector;old_nested=old_a.nested;old_a.nested.value=17
      assert(old_a.count==3 and old_b.count==3 and old_a.starts==1)
      Event.Publish("pulse",{});assert(old_a.pulses==1 and old_b.pulses==1))");
    fixture.Source("A",R"(local calls=0
      A={count=999,starts=0,pulses=0,nested={value=-1},added={value=7},
      OnStart=function(self)self.starts=self.starts+100 end,
      OnUpdate=function(self)calls=calls+1;self.count=self.count+10;self.module_calls=calls end,
      OnPulse=function(self,e)self.pulses=self.pulses+20 end})");
    fixture.Source("B","B={count=0,OnUpdate=function(self)self.count=self.count+2 end}");
    auto result=ComponentManager::ReloadScripts({"B","A"});Check(result.ok,Error(result));Check(result.instances_updated==3,"all instances updated");
    Lua(state,R"(assert(Actor.Find("a"):GetComponent("A")==old_a and old_a.vector==old_vector)
      assert(old_a.nested==old_nested and old_a.alias==old_nested and old_a.self_reference==old_a)
      assert(old_a.nested.value==17 and old_a.count==3 and old_a.starts==1)
      assert(old_a.added.value==7 and old_a.added~=old_b.added)
      Event.Publish("pulse",{});assert(old_a.pulses==21 and old_b.pulses==21))");
    engine.StepSimulation();
    Lua(state,"assert(old_a.count==13 and old_b.count==13 and old_a.module_calls==1 and old_b.module_calls==2 and old_a.starts==1)");
    fixture.Source("A","A={OnUpdate=function(self)self.count=-100 end}");fixture.Source("B","B={ syntax failure");
    result=ComponentManager::ReloadScripts({"A","B"});Check(!result.ok&&!result.diagnostics.empty(),"late batch compile must reject");
    engine.StepSimulation();Lua(state,"assert(old_a.count==23 and old_b.count==23)");
    fixture.Source("A","Actor.Destroy(Actor.Find('a'));A={}");
    result=ComponentManager::ReloadScripts({"A"});Check(!result.ok,"top-level host effect accepted");
    Lua(state,"assert(Actor.Find('a')==old_a.actor and old_a.actor:IsValid())");
    fixture.Source("A","A={count=1,added={value=7}}");
    result=ComponentManager::ReloadScripts({"A"});Check(result.ok,Error(result));
    engine.StepSimulation();Lua(state,"Event.Publish('pulse',{});assert(old_a.count==23 and old_a.pulses==21)");
    // Instance-local code is an explicit migration boundary, not a hidden old method.
    Lua(state,"old_a.cached=function()return 7 end");
    fixture.Source("A","A={OnUpdate=function(self)self.count=self.count+50 end}");
    result=ComponentManager::ReloadScripts({"A"});Check(!result.ok,"instance closure silently retained across reload");
    Lua(state,"old_a.cached=nil");result=ComponentManager::ReloadScripts({"A"});Check(result.ok,Error(result));
    engine.StepSimulation();Lua(state,"assert(old_a.count==73 and old_b.count==73)");
    // Anonymous subscriptions cannot be identified by a prototype method name.
    Lua(state,"anonymous=function(self,e) self.count=-999 end;Event.Subscribe('anon',old_a,anonymous)");
    EventBus::ApplyPendingOperations();
    fixture.Source("A","A={OnUpdate=function(self)self.count=self.count+100 end}");
    result=ComponentManager::ReloadScripts({"A"});Check(!result.ok,"anonymous subscription reload accepted");
    Lua(state,"Event.Unsubscribe('anon',old_a,anonymous)");EventBus::ApplyPendingOperations();
    result=ComponentManager::ReloadScripts({"A"});Check(result.ok,Error(result));
    lua_pushcfunction(state,[](lua_State *L)->int{
        const auto nested=ComponentManager::ReloadScripts({"A"});lua_pushboolean(L,!nested.ok&&!nested.diagnostics.empty()&&nested.diagnostics.front().stage=="busy");return 1;
    });lua_setglobal(state,"reload_inside_callback");
    Lua(state,"assert(reload_inside_callback())");
    for(const auto &types:std::vector<std::vector<std::string>>{{},{"A","A"},{"Transform"},{"missing"}})
        Check(!ComponentManager::ReloadScripts(types).ok,"invalid reload target accepted");
    Check(lua_gettop(state)==0,"reload leaked Lua stack");
    Check(ComponentManager::GetRuntimeErrorCount()==0,ComponentManager::GetLastRuntimeError());
}


void DefaultGraphRetention() {
    Fixture fixture;
    fixture.Source("A",R"(
      A={count=0,starts=0,nested={value=3,child={value=4}},
         OnStart=function(self) self.starts=self.starts+1;self.vector=Vector2(2,4) end,
         OnUpdate=function(self) self.count=self.count+1 end}
    )");
    Engine engine;engine.InitializeHeadless();engine.StepSimulation();
    auto *state=ComponentManager::GetComponentByKey(1,"logic").state();
    Lua(state,R"(
      graph_a=Actor.Find("a"):GetComponent("A");graph_b=Actor.Find("b"):GetComponent("A")
      kept_nested=graph_a.nested;kept_child=graph_a.nested.child;kept_vector=graph_a.vector
      graph_a.nested.value=17;graph_a.nested.child.value=29
      assert(graph_a.nested~=graph_b.nested and graph_a.nested.child~=graph_b.nested.child)
    )");
    fixture.Source("A",R"(
      local nested={value=-1,child={value=-2}}
      A={count=99,starts=99,nested=nested,new_alias=nested,child_alias=nested.child}
      A.new_self=A
      A.owned_cycle={owner=A};A.owned_cycle.self=A.owned_cycle
      A.OnUpdate=function(self)
        assert(self.new_self==self and self.new_alias==self.nested and self.child_alias==self.nested.child)
        assert(self.owned_cycle.owner==self and self.owned_cycle.self==self.owned_cycle)
        self.count=self.count+10
      end
    )");
    const auto result=ComponentManager::ReloadScripts({"A"});Check(result.ok,Error(result));
    Lua(state,R"(
      assert(Actor.Find("a"):GetComponent("A")==graph_a and graph_a.count==1 and graph_a.starts==1)
      assert(graph_a.nested==kept_nested and graph_a.nested.child==kept_child and graph_a.vector==kept_vector)
      assert(graph_a.nested.value==17 and graph_a.nested.child.value==29)
      assert(graph_a.new_self==graph_a and graph_b.new_self==graph_b)
      assert(graph_a.new_alias==kept_nested and graph_a.child_alias==kept_child)
      assert(graph_b.new_alias==graph_b.nested and graph_b.child_alias==graph_b.nested.child)
      assert(graph_a.owned_cycle~=graph_b.owned_cycle and graph_a.owned_cycle.owner==graph_a and graph_b.owned_cycle.owner==graph_b)
      graph_a.child_alias.value=31;assert(kept_child.value==31 and graph_b.child_alias.value==4)
    )");
    engine.StepSimulation();
    Lua(state,"assert(graph_a.count==11 and graph_b.count==11 and graph_a.starts==1)");
    Check(ComponentManager::GetRuntimeErrorCount()==0,ComponentManager::GetLastRuntimeError());
    Check(lua_gettop(state)==0,"default graph reload leaked stack");
}
void ConflictingAliasesRejectWholeBatch() {
    Fixture fixture;
    fixture.Source("A",R"(
      A={count=0,left={value=1},right={value=2},OnUpdate=function(self)self.count=self.count+1 end}
    )");
    Engine engine;engine.InitializeHeadless();engine.StepSimulation();
    auto *state=ComponentManager::GetComponentByKey(1,"logic").state();
    Lua(state,R"(
      conflict_a=Actor.Find("a"):GetComponent("A");conflict_b=Actor.Find("b"):GetComponent("A")
      conflict_other=Actor.Find("other"):GetComponent("B")
      conflict_a.left=conflict_a.right
      conflict_left=conflict_b.left;conflict_right=conflict_b.right
      before_A=A;before_B=B
    )");
    fixture.Source("A",R"(
      local shared={value=99}
      A={left=shared,right=shared,added={value=123},OnUpdate=function(self)self.count=self.count+100 end}
    )");
    fixture.Source("B","B={added=17,OnUpdate=function(self)self.count=self.count+1000 end}");
    const auto result=ComponentManager::ReloadScripts({"B","A"});
    Check(!result.ok && !result.diagnostics.empty(),"conflicting candidate aliases must reject the entire batch");
    Check(Error(result).find("alias")!=std::string::npos,"alias failure needs a useful diagnostic");
    Lua(state,R"(
      assert(A==before_A and B==before_B)
      assert(conflict_b.left==conflict_left and conflict_b.right==conflict_right and conflict_left~=conflict_right)
      assert(rawget(conflict_a,"added")==nil and rawget(conflict_b,"added")==nil and rawget(conflict_other,"added")==nil)
      assert(conflict_a.count==1 and conflict_b.count==1 and conflict_other.count==1)
    )");
    engine.StepSimulation();
    Lua(state,"assert(conflict_a.count==2 and conflict_b.count==2 and conflict_other.count==2)");
    Check(ComponentManager::GetRuntimeErrorCount()==0,ComponentManager::GetLastRuntimeError());
    Check(lua_gettop(state)==0,"rejected alias batch leaked stack");
}
void KnownTableKeyDefaultsPreserveIdentity() {
    Fixture fixture;
    fixture.Source("A",R"(
      local lookup_key={tag=1}
      A={count=0,lookup_key=lookup_key,map={[lookup_key]={value=3}},
         OnUpdate=function(self)self.count=self.count+1 end}
    )");
    Engine engine;engine.InitializeHeadless();engine.StepSimulation();
    auto *state=ComponentManager::GetComponentByKey(1,"logic").state();
    Lua(state,R"(
      keyed=Actor.Find("a"):GetComponent("A");keyed_other=Actor.Find("b"):GetComponent("A")
      saved_key=keyed.lookup_key;saved_map=keyed.map;saved_value=keyed.map[saved_key]
      saved_key.tag=11;saved_value.value=31
      assert(keyed.lookup_key~=keyed_other.lookup_key)
    )");
    fixture.Source("A",R"(
      local lookup_key={tag=99}
      local map={[lookup_key]={value=999}}
      A={lookup_key=lookup_key,map=map,key_alias=lookup_key,value_alias=map[lookup_key],
         OnUpdate=function(self)
           assert(self.key_alias==self.lookup_key and self.value_alias==self.map[self.lookup_key])
           self.count=self.count+10
         end}
    )");
    const auto result=ComponentManager::ReloadScripts({"A"});Check(result.ok,Error(result));
    Lua(state,R"(
      assert(keyed.lookup_key==saved_key and keyed.map==saved_map and keyed.map[saved_key]==saved_value)
      assert(keyed.key_alias==saved_key and keyed.value_alias==saved_value)
      assert(saved_key.tag==11 and saved_value.value==31)
      assert(keyed_other.key_alias==keyed_other.lookup_key and keyed_other.value_alias==keyed_other.map[keyed_other.lookup_key])
      assert(keyed_other.value_alias~=keyed.value_alias and keyed_other.value_alias.value==3)
    )");
    engine.StepSimulation();Lua(state,"assert(keyed.count==11 and keyed_other.count==11)");
    Check(ComponentManager::GetRuntimeErrorCount()==0,ComponentManager::GetLastRuntimeError());
    Check(lua_gettop(state)==0,"known table-key reload leaked stack");
}
void UnknownTableKeyDefaultsReject() {
    Fixture fixture;
    fixture.Source("A",R"(
      local private_key={tag=1}
      A={count=0,map={[private_key]={value=3}},OnUpdate=function(self)self.count=self.count+1 end}
    )");
    Engine engine;engine.InitializeHeadless();engine.StepSimulation();
    auto *state=ComponentManager::GetComponentByKey(1,"logic").state();
    Lua(state,R"(
      opaque_keyed=Actor.Find("a"):GetComponent("A")
      old_key=next(opaque_keyed.map);old_map=opaque_keyed.map;old_value=old_map[old_key];old_prototype=A
      old_value.value=37
    )");
    fixture.Source("A",R"(
      local private_key={tag=1}
      local map={[private_key]={value=999}}
      A={map=map,value_alias=map[private_key],OnUpdate=function(self)self.count=self.count+100 end}
    )");
    const auto result=ComponentManager::ReloadScripts({"A"});
    Check(!result.ok && !result.diagnostics.empty(),"unidentifiable table key must reject instead of guessing correspondence");
    Check(Error(result).find("key")!=std::string::npos,"unknown key failure needs a useful diagnostic");
    Lua(state,R"(
      assert(A==old_prototype and opaque_keyed.map==old_map and old_map[old_key]==old_value)
      assert(old_value.value==37 and rawget(opaque_keyed,"value_alias")==nil)
    )");
    engine.StepSimulation();Lua(state,"assert(opaque_keyed.count==2)");
    Check(ComponentManager::GetRuntimeErrorCount()==0,ComponentManager::GetLastRuntimeError());
    Check(lua_gettop(state)==0,"unknown table-key rejection leaked stack");
}
void RegisteredSourceConfinement() {
    Fixture fixture;Engine engine;engine.InitializeHeadless();engine.StepSimulation();
    auto *state=ComponentManager::GetComponentByKey(1,"logic").state();
    Lua(state,"confined=Actor.Find('a'):GetComponent('A');before_source=A;before_count=confined.count");
    const auto source=fixture.root/"component_types/A.lua";
    std::filesystem::remove(source);
    auto result=ComponentManager::ReloadScripts({"A"});
    Check(!result.ok && !result.diagnostics.empty() && result.diagnostics.front().stage=="read",
          "missing registered source must reject before changing live state");
    Lua(state,"assert(A==before_source and confined.count==before_count)");
    const auto outside=fixture.root/"outside.lua";
    std::ofstream(outside)<<"A={OnUpdate=function(self)self.count=-100 end}";
    std::error_code symlink_error;std::filesystem::create_symlink(outside,source,symlink_error);
    if(!symlink_error) {
        result=ComponentManager::ReloadScripts({"A"});
        Check(!result.ok && !result.diagnostics.empty() && result.diagnostics.front().stage=="read",
              "registered source symlink escaped component directory");
        Lua(state,"assert(A==before_source and confined.count==before_count)");
    } else std::cout<<"SKIP source-symlink variant: "<<symlink_error.message()<<'\n';
    engine.StepSimulation();Lua(state,"assert(confined.count==before_count+1)");
    Check(ComponentManager::GetRuntimeErrorCount()==0,ComponentManager::GetLastRuntimeError());
}
void RegisteredCanonicalRedirectRejects() {
    Fixture fixture;Engine engine;engine.InitializeHeadless();engine.StepSimulation();
    auto *state=ComponentManager::GetComponentByKey(1,"logic").state();
    Lua(state,R"(
      redirect_actor=Actor.Find("a");redirect_instance=redirect_actor:GetComponent("A")
      redirect_prototype=A;redirect_count=redirect_instance.count;redirect_starts=redirect_instance.starts
    )");
    const auto registered=ComponentManager::GetComponentScriptSource("A");
    const auto cache_before=ManagerDetail::g_runtime.component_source_cache;
    const auto sources_before=ManagerDetail::g_runtime.component_type_sources;
    const auto owners_before=ManagerDetail::g_runtime.component_source_owners;
    const auto source=fixture.root/"component_types/A.lua";
    const auto target=fixture.root/"component_types/redirect/A.lua";
    std::filesystem::create_directories(target.parent_path());
    std::ofstream(target)<<"A={OnUpdate=function(self)self.count=self.count+100 end}";
    std::filesystem::remove(source);
    std::error_code symlink_error;std::filesystem::create_symlink(target,source,symlink_error);
    if(symlink_error) {
#ifdef _WIN32
        std::cout<<"SKIP canonical redirect regression: "<<symlink_error.message()<<'\n';return;
#else
        Check(false,"could not create canonical redirect fixture: "+symlink_error.message());
#endif
    }
    const auto failed=ComponentManager::ReloadScripts({"A"});
    Check(!failed.ok&&!failed.diagnostics.empty()&&failed.diagnostics.front().stage=="read"&&
          Error(failed).find("redirected")!=std::string::npos,"in-root canonical redirect was not rejected before staging");
    Check(ManagerDetail::g_runtime.component_source_cache.size()==cache_before.size()&&
          ManagerDetail::g_runtime.component_type_sources==sources_before&&
          ManagerDetail::g_runtime.component_source_owners==owners_before,
          "rejected redirect changed source/cache ownership metadata");
    for(const auto &[path,prototype]:cache_before)
        Check(ManagerDetail::AreSameLuaRef(prototype,ManagerDetail::g_runtime.component_source_cache.at(path)),
              "rejected redirect replaced cached prototype identity");
    Check(ManagerDetail::AreSameLuaRef(cache_before.at(registered),ManagerDetail::g_runtime.component_type_tables.at("A")),
          "rejected redirect replaced active prototype registry");
    Lua(state,R"(
      assert(A==redirect_prototype and Actor.Find("a")==redirect_actor and redirect_actor:IsValid())
      assert(redirect_actor:GetComponent("A")==redirect_instance and redirect_instance.count==redirect_count)
      assert(redirect_instance.starts==redirect_starts)
    )");
    engine.StepSimulation();Lua(state,"assert(redirect_instance.count==redirect_count+1)");
    // Restoring the original canonical path permits an ordinary same-source reload.
    std::filesystem::remove(source);
    fixture.Source("A","A={OnUpdate=function(self)self.count=self.count+10 end}");
    const auto restored=ComponentManager::ReloadScripts({"A"});Check(restored.ok,Error(restored));
    Check(ComponentManager::GetComponentScriptSource("A")==registered&&
          ManagerDetail::g_runtime.component_source_owners==owners_before,
          "repair changed stable registered source/owner identity");
    Check(ManagerDetail::AreSameLuaRef(ManagerDetail::g_runtime.component_source_cache.at(registered),
              ManagerDetail::g_runtime.component_type_tables.at("A"))&&
          !ManagerDetail::AreSameLuaRef(cache_before.at(registered),ManagerDetail::g_runtime.component_source_cache.at(registered)),
          "successful repair did not update the matching canonical cache entry");
    engine.StepSimulation();Lua(state,"assert(redirect_instance.count==redirect_count+11 and redirect_instance.starts==redirect_starts)");
    Check(ComponentManager::GetRuntimeErrorCount()==0,ComponentManager::GetLastRuntimeError());
    Check(lua_gettop(state)==0,"redirect rejection/repair leaked the Lua stack");
}
std::array<int,3> PendingActorTimeline(bool reload) {
    Fixture fixture;
    fixture.Source("A","A={count=0,OnUpdate=function(self)self.count=self.count+1 end}");
    fixture.Source("PendingProbe",R"(PendingProbe={starts=0,updates=0,
      OnStart=function(self)self.starts=self.starts+1 end,
      OnUpdate=function(self)self.updates=self.updates+1 end})");
    std::filesystem::create_directories(fixture.root/"actor_templates");
    std::ofstream(fixture.root/"actor_templates/Pending.template") <<
      R"({"name":"pending","components":{"logic":{"type":"PendingProbe"}}})";
    Engine engine;engine.InitializeHeadless();engine.StepSimulation();
    const auto actor=ComponentManager::InstantiateActor("Pending");
    Check(!actor.isNil(),"create pending actor at idle boundary");
    const auto handle=actor.cast<ActorHandle>().value();
    const auto probe=ComponentManager::GetComponentByKey(handle.uid,"logic");
    Check(probe["starts"].cast<int>().value()==0 && probe["updates"].cast<int>().value()==0,"pending actor ran before stepping");
    if(reload) {
        fixture.Source("A","A={count=0,OnUpdate=function(self)self.count=self.count+10 end}");
        const auto result=ComponentManager::ReloadScripts({"A"});Check(result.ok,Error(result));
    }
    engine.StepSimulation();
    const int starts=probe["starts"].cast<int>().value(),first_updates=probe["updates"].cast<int>().value();
    engine.StepSimulation();
    Check(ComponentManager::GetRuntimeErrorCount()==0,ComponentManager::GetLastRuntimeError());
    return {starts,first_updates,probe["updates"].cast<int>().value()};
}
int main(){try{
    ExistingReloadCases();DefaultGraphRetention();ConflictingAliasesRejectWholeBatch();
    KnownTableKeyDefaultsPreserveIdentity();UnknownTableKeyDefaultsReject();RegisteredSourceConfinement();RegisteredCanonicalRedirectRejects();
    const auto baseline=PendingActorTimeline(false),reloaded=PendingActorTimeline(true);
    Check(baseline==std::array<int,3>{1,0,1},"pending actor baseline changed unexpectedly");
    Check(reloaded==baseline,"reload changed pending actor activation/update timing");
    std::cout<<"PASS live reload identity/state graph/atomic failures/lifecycle/event binding/pending actor timing\n";
    return 0;
}catch(const std::exception &error){std::cerr<<"FAIL live reload: "<<error.what()<<'\n';return 1;}}
