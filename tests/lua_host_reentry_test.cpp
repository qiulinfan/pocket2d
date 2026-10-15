#include "core/Engine.h"
#include "core/FrameClock.h"
#include "scripting/ComponentManager.h"
#include "scripting/LuaBoundary.h"
#include "shared/resources/ResourcePath.h"
#include "../src/engine/scripting/manager/Internal.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const std::string &message) { if (!condition) throw std::runtime_error(message); }
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("pocket-lua-host-reentry-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture(const std::string &probe, const std::string &extra_components = "", const std::string &extra_actors = "") {
        for (const char *directory : {"scenes", "component_types"}) std::filesystem::create_directories(root/directory);
        std::ofstream(root/"game.config") << R"({"initial_scene":"test"})";
        std::ofstream(root/"scenes/test.scene") << R"({"actors":[{"uid":1,"name":"owner","components":{"a":{"type":"Probe"},"z":{"type":"Victim"})"
            << extra_components << R"(}},{"uid":2,"name":"stats","components":{"stats":{"type":"Counters"}}})" << extra_actors << "]}";
        std::ofstream(root/"scenes/empty.scene") << R"({"actors":[]})";
        std::ofstream(root/"component_types/Probe.lua") << probe;
        std::ofstream(root/"component_types/Counters.lua") << R"(
Counters={started=0,updates=0,destroyed=0,spawn_destroyed=0,victim_destroyed=0,bad=0,
          contacts=0,second_contacts=0,after_event=0,good_event=0,
          OnStart=function(self) test_counts=self end}
function Counts()
    if test_counts then return test_counts end
    return Actor.Find("stats"):GetComponentByKey("stats")
end
)";
        std::ofstream(root/"component_types/Spawn.lua") << R"(
Spawn={OnStart=function(self) local c=Counts();c.started=c.started+1 end,
       OnUpdate=function(self) local c=Counts();c.updates=c.updates+1 end,
       OnDestroy=function(self) local c=Counts();c.spawn_destroyed=c.spawn_destroyed+1 end}
)";
        std::ofstream(root/"component_types/Victim.lua") << R"(
Victim={updates=0,OnUpdate=function(self) self.updates=self.updates+1 end,
        OnDestroy=function(self) local c=Counts();c.victim_destroyed=c.victim_destroyed+1 end,
        OnCollisionEnter=function(self) local c=Counts();c.second_contacts=c.second_contacts+1 end,
        OnTriggerEnter=function(self) local c=Counts();c.second_contacts=c.second_contacts+1 end}
)";
        ResourcePath::SetResourcesRootPath(root);
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root,error); }
};
luabridge::LuaRef Counters() { return ComponentManager::GetComponentByKey(2,"stats"); }
int Count(const char *name) { return Counters()[name].cast<int>().value(); }
void Start(Engine &engine) {
    Check(engine.GetLastSceneLoadError().empty(),engine.GetLastSceneLoadError());
    engine.InitializeHeadless();
}
void DestructionMutation() {
    Fixture fixture(R"(
Probe={OnStart=function(self) self.actor:RemoveComponent(self) end,
       OnDestroy=function(self)
          local c=Counts();c.destroyed=c.destroyed+1
          for i=1,64 do self.actor:AddComponent("Spawn") end
          self.actor:RemoveComponent(self.actor:GetComponentByKey("z"))
          self.actor:RemoveComponent(self)
       end}
)");
    Engine engine; Start(engine);
    auto counters=Counters();
    engine.StepSimulation();
    Check(Count("destroyed")==1 && Count("victim_destroyed")==1,"chained removals must destroy exactly once");
    Check(Count("started")==0,"OnDestroy additions must wait until next tick");
    engine.StepSimulation();
    Check(Count("started")==64 && Count("updates")==64,"OnDestroy additions lost pending OnStart/Update");
    engine.StepSimulation();
    Check(Count("started")==64 && Count("updates")==128,"added lifecycle duplicated or skipped");
    engine.RequestSceneLoad("empty"); engine.StepSimulation();
    Check(counters["spawn_destroyed"].cast<int>().value()==64,"scene teardown must destroy all new components");
    Check(ComponentManager::GetRuntimeErrorCount()==0,"destruction mutation script error");
}
void GetterErrors() {
    for (const std::string field : {"enabled", "OnUpdate", "OnLateUpdate"}) {
        const auto script=std::string(R"(
Probe={OnStart=function(self)
    rawset(self,"FIELD",nil)
    setmetatable(self,{__index=function(_,key)
        if key=="FIELD" then error("FIELD getter sentinel") end
        return Probe[key]
    end})
end,OnUpdate=function(self) end,OnLateUpdate=function(self) end}
)");
        auto configured=script;
        for (auto position=configured.find("FIELD");position!=std::string::npos;position=configured.find("FIELD",position+field.size()))
            configured.replace(position,5,field);
        Fixture fixture(configured);
        Engine engine; Start(engine);
        auto counters=Counters();
        lua_State *state=counters.state();
        engine.StepSimulation(); engine.StepSimulation();
        Check(FrameClock::GetFrameNumber()==2,"getter error interrupted a simulation tick");
        Check(ComponentManager::GetRuntimeErrorCount()>=2 &&
              ComponentManager::GetLastRuntimeError().find(field+" getter sentinel")!=std::string::npos,
              "getter errors must use normal runtime diagnostics");
        Check(ComponentManager::GetComponentByKey(1,"z")["updates"].cast<int>().value()==2,"getter failure skipped unrelated callback");
        Check(lua_gettop(state)==0,"getter error corrupted host stack");
    }
}
void GetterRemoval() {
    Fixture fixture(R"(
Probe={OnStart=function(self)
    setmetatable(self,{__index=function(_,key)
        if key=="OnUpdate" then
            for i=1,64 do self.actor:AddComponent("Spawn") end
            self.actor:RemoveComponent(self)
            return function() local c=Counts();c.bad=c.bad+1 end
        end
        return Probe[key]
    end})
end,OnUpdate=function(self) end,
OnDestroy=function(self) local c=Counts();c.destroyed=c.destroyed+1 end}
)");
    Engine engine; Start(engine);
    engine.StepSimulation();
    Check(Count("bad")==0 && Count("destroyed")==1,"getter-removed component callback still ran");
    engine.StepSimulation();
    Check(Count("started")==64 && Count("updates")==64,"getter additions did not start on next tick");
    Check(ComponentManager::GetRuntimeErrorCount()==0,"getter removal script errors");
}
void ContactMutation(bool trigger) {
    const std::string method=trigger ? "OnTriggerEnter" : "OnCollisionEnter";
    Fixture fixture("Probe={"+method+R"(=function(self,collision)
        local c=Counts()
        if c.contacts~=0 then return end
        c.contacts=c.contacts+1
        for i=1,64 do self.actor:AddComponent("Spawn") end
    end})",
        std::string(",\"body\":{\"type\":\"Rigidbody\",\"gravity_scale\":0,\"has_collider\":") +
        (trigger?"false":"true") + ",\"has_trigger\":" + (trigger?"true":"false") + "}",
        std::string(R"(,{"uid":3,"name":"wall","components":{"body":{"type":"Rigidbody","body_type":"static","x":0.4,"gravity_scale":0,"has_trigger":)") +
        (trigger ? "true" : "false") + "}}}");
    Engine engine; Start(engine);
    engine.StepSimulation();
    Check(Count("contacts")==1 && Count("second_contacts")>=1,
          method+" real Box2D dispatch lost recipient: "+std::to_string(Count("contacts"))+","+std::to_string(Count("second_contacts")));
    Check(Count("started")==0,"contact additions started in same tick");
    engine.StepSimulation();
    Check(Count("started")==64,"contact additions lost OnStart");
    Check(ComponentManager::GetRuntimeErrorCount()==0,"contact mutation script errors");
}
void MetadataErrorsAndMutation() {
    Fixture fixture(R"(
Probe={score=10,mode="error",OnStart=function(self)
    self.score=nil
    setmetatable(self,{__index=function(_,key)
        if key=="score" then
            if self.mode=="error" then error("metadata getter sentinel") end
            for i=1,64 do self.actor:AddComponent("Spawn") end
            self.actor:RemoveComponent(self)
            return 42
        end
        return Probe[key]
    end})
end}
)");
    Engine engine; Start(engine);
    engine.StepSimulation();
    auto probe=ComponentManager::GetComponentByKey(1,"a");
    lua_State *state=probe.state();
    lua_pushinteger(state,123456);
    bool rejected=false;
    try { ComponentManager::GetRuntimeComponentProperties(1,"a"); }
    catch(const std::exception &error) { rejected=std::string(error.what()).find("metadata getter sentinel")!=std::string::npos; }
    Check(rejected,"metadata getter failure became silent success");
    Check(lua_gettop(state)==1 && lua_tointeger(state,1)==123456,"metadata failure erased enclosing Lua stack");
    lua_pop(state,1);
    probe["mode"]=std::string("remove");
    rejected=false;
    try { ComponentManager::GetRuntimeComponentProperties(1,"a"); }
    catch(const std::exception &error) { rejected=std::string(error.what()).find("removed during property lookup")!=std::string::npos; }
    Check(rejected,"metadata used a component removed by its getter");
    engine.StepSimulation();
    Check(Count("started")==64,"metadata callback additions lost OnStart");
    Check(ComponentManager::GetRuntimeErrorCount()==0 && lua_gettop(state)==0,"metadata recovery corrupted runtime");
}
void NestedEventsAndIdentity() {
    Fixture fixture(R"(
Probe={OnStart=function(self)
    Event.Subscribe("outer",self,self.Bad)
    Event.Subscribe("outer",self,self.Good)
    Event.Subscribe("inner",self,self.Bad)
    -- A forged component key must neither remove nor subscribe as the victim.
    self.actor:RemoveComponent({key="z"})
    Event.Subscribe("outer",{key="z",actor=self.actor},function() error("forged subscriber") end)
end,
Bad=function(self,event) error("nested event sentinel") end,
Good=function(self,event)
    assert(event.value==17 and self.actor:IsValid())
    Event.Publish("inner",event)
    assert(event.value==17 and self.actor:IsValid())
    local c=Counts();c.good_event=c.good_event+1
end,
OnUpdate=function(self)
    if Application.GetFrame()==1 then
        local keep={value=17}
        Event.Publish("outer",keep)
        assert(keep.value==17 and self.actor:IsValid())
        local c=Counts();c.after_event=c.after_event+1
    end
end}
)");
    Engine engine; Start(engine);
    engine.StepSimulation(); engine.StepSimulation();
    Check(!ComponentManager::GetComponentByKey(1,"z").isNil(),"forged key removed a registered component");
    Check(Count("good_event")==1 && Count("after_event")==1,"nested Event.Publish error damaged caller stack");
    Check(ComponentManager::GetRuntimeErrorCount()==2 && ComponentManager::GetLastRuntimeError().find("nested event sentinel")!=std::string::npos,
          "event errors were lost or forged subscriber was called");
    Check(lua_gettop(Counters().state())==0,"nested Event.Publish leaked stack values");
}
void ProtectedPrototypeChains() {
    Fixture fixture("Probe={}");
    Engine engine; Start(engine);
    auto component = ComponentManager::GetComponentByKey(1, "a");
    lua_State *state = component.state();
    const char *script = R"(
        chain_base = {value=42, false_value=false, callback=function(self) return self end}
        chain_middle = setmetatable({}, {__index=chain_base})
        chain_leaf = setmetatable({}, {__index=chain_middle})
        setmetatable(getmetatable(chain_leaf), {__index=function() error("meta metadata must be raw") end})
        deep_chain = chain_base
        for i=1,80 do deep_chain=setmetatable({}, {__index=deep_chain}) end
        cycle_chain = {}; setmetatable(cycle_chain,{__index=cycle_chain})
        vector_chain = setmetatable({}, {__index=Vector2(7,8)})
        fallback_calls = 0
        function_base = setmetatable({}, {__index=function(receiver,key)
            assert(receiver==function_base, "fallback received wrong chain node")
            fallback_calls=fallback_calls+1
            if key=="error" then error("chain function sentinel") end
            if key=="remove" then
                local owner=Actor.Find("owner")
                owner:RemoveComponent(owner:GetComponentByKey("a"))
                return 123
            end
            return 77
        end})
        function_chain = setmetatable({}, {__index=setmetatable({}, {__index=function_base})})
    )";
    Check(luaL_dostring(state, script)==LUA_OK, "create prototype chain fixture");
    auto leaf = luabridge::getGlobal(state, "chain_leaf");
    auto fallback = luabridge::getGlobal(state, "function_chain");
    lua_pushinteger(state, 987654);
    const auto balanced = [&] {
        Check(lua_gettop(state)==1 && lua_tointeger(state,1)==987654,
              "prototype lookup changed enclosing stack");
    };
    Check(ManagerDetail::ProtectedField(leaf,"value").cast<int>().value()==42,"nested table prototype lookup"); balanced();
    Check(ManagerDetail::ProtectedField(leaf,"false_value").isBool() &&
          !ManagerDetail::ProtectedField(leaf,"false_value").cast<bool>().value(), "false value must not fall through"); balanced();
    Check(ManagerDetail::ProtectedField(leaf,"callback").isFunction(),"inherited callback lookup"); balanced();
    Check(ManagerDetail::ProtectedField(leaf,"missing").isNil(),"missing table field must remain nil"); balanced();
    Check(luaL_dostring(state,"chain_middle.value=17")==LUA_OK,"mutate prototype");
    Check(ManagerDetail::ProtectedField(leaf,"value").cast<int>().value()==17,"prototype mutation must be visible without a cache"); balanced();
    Check(ManagerDetail::ProtectedField(luabridge::getGlobal(state,"deep_chain"),"value").cast<int>().value()==42,
          "deep valid chain must fall back to Lua rather than truncate"); balanced();
    Check(ManagerDetail::ProtectedField(luabridge::getGlobal(state,"vector_chain"),"x").cast<float>().value()==7,
          "userdata __index fallback must preserve registered property getters"); balanced();
    Check(ManagerDetail::ProtectedField(fallback,"value").cast<int>().value()==77 &&
          luabridge::getGlobal(state,"fallback_calls").cast<int>().value()==1,"function fallback must execute exactly once"); balanced();
    for (const auto &entry : {std::make_pair(fallback,"error"),
                              std::make_pair(luabridge::getGlobal(state,"cycle_chain"),"missing")}) {
        bool rejected=false;
        try { ManagerDetail::ProtectedField(entry.first,entry.second); }
        catch(const LuaBoundary::Error &) { rejected=true; }
        Check(rejected,"function error/cyclic __index must be reported by protected lookup"); balanced();
    }
    Check(luabridge::getGlobal(state,"fallback_calls").cast<int>().value()==2,"function fallback was replayed on failure");
    Check(ManagerDetail::ProtectedField(fallback,"remove").cast<int>().value()==123 &&
          ComponentManager::GetComponentByKey(1,"a").isNil(),"function fallback must preserve host mutation"); balanced();
    lua_pop(state,1);
    engine.StepSimulation();
    Check(FrameClock::GetFrameNumber()==1 && lua_gettop(state)==0,"runtime did not recover after chain errors/mutation");
}

}
int main() {
    try {
        DestructionMutation(); GetterErrors(); GetterRemoval();
        ContactMutation(false); ContactMutation(true);
        MetadataErrorsAndMutation(); NestedEventsAndIdentity(); ProtectedPrototypeChains();
        std::cout << "PASS Lua host reentry: teardown/getters/contacts/metadata/nested events\n";
        return 0;
    } catch(const std::exception &error) { std::cerr << "FAIL Lua host reentry: " << error.what() << '\n'; return 1; }
}
