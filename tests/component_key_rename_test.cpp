#include "core/Engine.h"
#include "scene/ActorHandle.h"
#include "scripting/ComponentManager.h"
#include "scripting/EventBus.h"
#include "shared/resources/ResourcePath.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void Lua(lua_State *state, const char *source) {
    const int top = lua_gettop(state);
    const int status = luaL_dostring(state, source);
    const std::string error = status == LUA_OK ? "" : lua_tostring(state, -1);
    lua_settop(state, top);
    if (status != LUA_OK) throw std::runtime_error(error);
}
struct Fixture {
    std::filesystem::path previous = ResourcePath::ResourcesRootPath();
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("pocket-component-key-rename-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() {
        std::filesystem::create_directories(root / "scenes");
        std::filesystem::create_directories(root / "component_types");
        std::filesystem::create_directories(root / "actor_templates");
        std::ofstream(root / "game.config") << R"({"initial_scene":"test"})";
        std::ofstream(root / "scenes/test.scene") << R"({"actors":[
            {"uid":1,"name":"one","components":{"logic":{"type":"RenameProbe"}}},
            {"uid":2,"name":"two","components":{"logic":{"type":"RenameProbe"}}}]})";
        std::ofstream(root / "component_types/RenameProbe.lua") << R"(
            TRACE={}
            RenameProbe={starts=0,updates=0,late_updates=0,start_key='',named=0,anonymous=0,pending_named=0,pending_anonymous=0,
                removed=0,cancelled=0,dispatches=0,later=0,
                OnStart=function(self) self.starts=self.starts+1;self.start_key=self.key end,
                OnUpdate=function(self) self.updates=self.updates+1 end,
                OnLateUpdate=function(self) self.late_updates=self.late_updates+1 end,
                Named=function(self,event)
                    self.named=self.named+event.delta
                    table.insert(TRACE,self.actor:GetName()..':named:'..self.key)
                end,
                Pending=function(self,event) self.pending_named=self.pending_named+event.delta end,
                Removed=function(self,event) self.removed=self.removed+event.delta end,
                Cancelled=function(self,event) self.cancelled=self.cancelled+event.delta end,
                DispatchRename=function(self,event)
                    local before=self.key
                    assert(not RenameTest.Rename(1,before,'during_dispatch'))
                    assert(self.key==before)
                    assert(self.actor:GetComponentByKey(before)==self)
                    assert(self.actor:GetComponentByKey('during_dispatch')==nil)
                    self.dispatches=self.dispatches+1
                end}
        )";
        std::ofstream(root / "actor_templates/runtime.template") << R"({"name":"spawned","components":{"logic":{"type":"RenameProbe"}}})";
        ResourcePath::SetResourcesRootPath(root);
    }
    ~Fixture() { ResourcePath::SetResourcesRootPath(previous); std::error_code error; std::filesystem::remove_all(root, error); }
};
void Run() {
    Fixture fixture; Engine engine; engine.InitializeHeadless();
    auto reference = ComponentManager::GetComponentByKey(1, "logic");
    auto other = ComponentManager::GetComponentByKey(2, "logic");
    auto *state = reference.state(); const int top = lua_gettop(state);
    const ActorHandle actor(engine.GetRuntimeActorByUID(1));
    luabridge::getGlobalNamespace(state).beginNamespace("RenameTest")
        .addFunction("Rename", &ComponentManager::RenameComponentKey).endNamespace();
    Lua(state, R"(
        a=Actor.Find('one'):GetComponentByKey('logic')
        b=Actor.Find('two'):GetComponentByKey('logic')
        assert(a.starts==0 and b.starts==0)
        anonymous=function(self,event)
            self.anonymous=self.anonymous+event.delta
            table.insert(TRACE,self.actor:GetName()..':anonymous:'..self.key)
        end
        pending_anonymous=function(self,event) self.pending_anonymous=self.pending_anonymous+event.delta end
        Event.Subscribe('active',a,a.Named)
        Event.Subscribe('active',a,anonymous)
        Event.Subscribe('active',b,b.Named)
        Event.Subscribe('removed',a,a.Removed)
    )");
    EventBus::ApplyPendingOperations();
    Lua(state, R"(
        Event.Subscribe('pending',a,a.Pending)
        Event.Subscribe('pending',a,pending_anonymous)
        Event.Unsubscribe('removed',a,a.Removed)
        Event.Subscribe('cancelled',a,a.Cancelled)
        Event.Unsubscribe('cancelled',a,a.Cancelled)
        Event.Publish('active',{delta=1})
        assert(a.named==1 and a.anonymous==1 and b.named==1)
        Event.Publish('pending',{delta=1})
        assert(a.pending_named==0 and a.pending_anonymous==0)
    )");
    Check(ComponentManager::RenameComponentKey(1, "logic", "renamed"), "direct manager rename failed");
    Check(ComponentManager::GetComponentByKey(1, "logic").isNil() &&
          reference.rawequal(ComponentManager::GetComponentByKey(1, "renamed")) &&
          other.rawequal(ComponentManager::GetComponentByKey(2, "logic")) && actor.IsValid(),
          "rename replaced component/actor identity or affected another owner");
    Lua(state, R"(
        assert(a.key=='renamed' and a.starts==0)
        TRACE={}
        Event.Publish('active',{delta=2})
        assert(a.named==3 and a.anonymous==3 and b.named==3)
        assert(table.concat(TRACE,',')=='one:named:renamed,one:anonymous:renamed,two:named:logic')
        Event.Publish('pending',{delta=2})
        assert(a.pending_named==0 and a.pending_anonymous==0)
        Event.Publish('removed',{delta=1});assert(a.removed==1)
    )");
    EventBus::ApplyPendingOperations();
    Lua(state, R"(
        Event.Publish('pending',{delta=4})
        assert(a.pending_named==4 and a.pending_anonymous==4)
        Event.Publish('removed',{delta=5});assert(a.removed==1)
        Event.Publish('cancelled',{delta=5});assert(a.cancelled==0)
        Event.Unsubscribe('active',a,a.Named)
        Event.Unsubscribe('active',a,anonymous)
    )");
    EventBus::ApplyPendingOperations();
    Lua(state, R"(
        Event.Publish('active',{delta=10})
        assert(a.named==3 and a.anonymous==3 and b.named==13)
    )");
    // The initial queued start must resolve the renamed key exactly once.
    engine.StepSimulation();
    Lua(state, R"(
        assert(a.starts==1 and a.start_key=='renamed' and b.starts==1 and b.start_key=='logic')
        Event.Subscribe('dispatch',a,a.DispatchRename)
        Event.Subscribe('dispatch',a,function(self,event) self.later=self.later+1 end)
    )");
    EventBus::ApplyPendingOperations();
    Lua(state, "Event.Publish('dispatch',{})");
    Check(!EventBus::IsDispatching() && lua_gettop(state) == top, "event dispatch leaked depth or Lua stack");
    Check(reference.rawequal(ComponentManager::GetComponentByKey(1, "renamed")) &&
          ComponentManager::GetComponentByKey(1, "during_dispatch").isNil(),
          "dispatch-time rename mutated registry identity before rejecting");
    Lua(state, "assert(a.key=='renamed' and a.dispatches==1 and a.later==1)");
    Check(ComponentManager::RenameComponentKey(1, "renamed", "final"), "dispatch rejection prevented later idle rename");
    Lua(state, R"(
        Event.Publish('pending',{delta=1})
        assert(a.key=='final' and a.pending_named==5 and a.pending_anonymous==5 and a.starts==1)
    )");
    engine.StepSimulation();
    Lua(state, "assert(a.starts==1 and b.starts==1)");
    // Instantiate is queryable immediately but must join lifecycle lists only
    // once when its deferred activation is finalized, even after a key rename.
    auto spawned = ComponentManager::InstantiateActor("runtime");
    Check(!spawned.isNil(), "deferred runtime actor did not instantiate");
    const auto spawned_actor = spawned.cast<ActorHandle>().value();
    auto spawned_logic = ComponentManager::GetComponentByKey(spawned_actor.uid, "logic");
    Check(ComponentManager::RenameComponentKey(spawned_actor.uid, "logic", "spawned_logic"), "deferred component rename failed");
    Check(spawned_logic["starts"].cast<int>().value() == 0 && spawned_logic["updates"].cast<int>().value() == 0 &&
          spawned_logic["late_updates"].cast<int>().value() == 0, "rename activated deferred lifecycle early");
    ComponentManager::FinalizeFrameMutations();
    Check(spawned_logic["starts"].cast<int>().value() == 0 && spawned_logic["updates"].cast<int>().value() == 0,
          "finalization executed gameplay callbacks");
    engine.StepSimulation();
    const int starts = spawned_logic["starts"].cast<int>().value();
    const int updates = spawned_logic["updates"].cast<int>().value();
    const int late = spawned_logic["late_updates"].cast<int>().value();
    if (starts != 1 || updates != 1 || late != 1)
        throw std::runtime_error("deferred rename lifecycle must run once: starts=" + std::to_string(starts) +
            " updates=" + std::to_string(updates) + " late=" + std::to_string(late));
    Check(spawned_logic["start_key"].cast<std::string>().value() == "spawned_logic" &&
          spawned_logic.rawequal(ComponentManager::GetComponentByKey(spawned_actor.uid, "spawned_logic")),
          "deferred start lost renamed key or component identity");
    Check(ComponentManager::GetRuntimeErrorCount() == 0 && lua_gettop(state) == top,
          "rename lifecycle/event callbacks reported errors or leaked stack");
}
}
int main() {
    try { Run(); std::cout << "component_key_rename_test: active/pending callbacks, pending start and dispatch rejection passed\n"; return 0; }
    catch (const std::exception &error) { std::cerr << "component_key_rename_test: " << error.what() << '\n'; return 1; }
}
