#include "core/Engine.h"
#include "scripting/ComponentManager.h"
#include "shared/resources/ResourcePath.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}
void RunLua(lua_State *state, const char *source) {
    if (luaL_dostring(state, source) != LUA_OK) {
        const std::string message = lua_tostring(state, -1);
        lua_pop(state, 1);
        throw std::runtime_error(message);
    }
}
struct Fixture {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("pocket2d-isolation-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() {
        for (const char *directory : {"scenes", "component_types", "actor_templates"})
            std::filesystem::create_directories(path / directory);
        std::ofstream(path / "game.config") << R"({"initial_scene":"isolation"})";
        std::ofstream(path / "scenes/isolation.scene") << R"({"actors":[
            {"uid":1,"name":"one","components":{"logic":{"type":"Isolation"}}},
            {"uid":2,"name":"two","components":{"logic":{"type":"Isolation"}}},
            {"uid":3,"name":"override","components":{"logic":{
                "type":"Isolation","configured":123,"values":[7,8]}}},
            {"uid":4,"name":"derived_one","components":{"logic":{"type":"DerivedDefaults"}}},
            {"uid":5,"name":"derived_two","components":{"logic":{"type":"DerivedDefaults"}}}
        ]})";
        std::ofstream(path / "component_types/Isolation.lua") << R"(
            local alias = {nested={count=0}}
            local cycle = {}; cycle.self = cycle
            local table_key = {}
            local vector = Vector2(1, 2)
            local deep = {}; local tail = deep
            for i=1,5000 do tail.next = {}; tail=tail.next end
            tail.value = 0
            local shared_upvalue = 0
            ExplicitGlobal = ExplicitGlobal or {count=0}
            Isolation = {
                values={}, seen=-1, configured=5, alias_a=alias, alias_b=alias,
                cycle=cycle, table_key=table_key, keyed={[table_key]="bound"},
                special=setmetatable({}, {__index={score=5,
                    Read=function(self) return self.score end}}),
                vector=vector, vector_alias=vector,
                cursor=Input.GetMousePosition(), deep=deep,
                TouchShared=function(self) shared_upvalue=shared_upvalue+1; return shared_upvalue end,
                TouchGlobal=function(self) ExplicitGlobal.count=ExplicitGlobal.count+1; return ExplicitGlobal.count end,
                OnStart=function(self)
                    assert(self.self == self)
                    self.seen = #self.values
                    table.insert(self.values, self.actor:GetUID())
                    assert(self.alias_a == self.alias_b)
                    self.alias_a.nested.count = self.alias_a.nested.count + 1
                    assert(self.cycle.self == self.cycle)
                    assert(self.keyed[self.table_key] == "bound")
                    local defaults = getmetatable(self.special).__index
                    defaults.score = defaults.score + 1
                    assert(self.special:Read() == 6)
                    assert(self.vector == self.vector_alias)
                    self.vector.x = self.vector.x + 1
                    self.cursor.x = self.cursor.x + 1
                    local tail = self.deep
                    for i=1,5000 do tail=tail.next end
                    tail.value = tail.value + 1
                    assert(tail.value == 1)
                end
            }
            Isolation.self = Isolation
        )";
        std::ofstream(path / "component_types/UnknownDefaults.lua") <<
            "UnknownDefaults = {nested={resource=Rigidbody()}}";
        std::ofstream(path / "component_types/DerivedDefaults.lua") << R"(
            local cycle = {}; cycle.self = cycle
            DefaultsBase = {items={}, nested={count=0}, cycle=cycle,
                method_name={wrong="must not shadow derived function"}, setting=7}
            DerivedDefaults = setmetatable({
                alias=DefaultsBase.nested, setting=9,
                method_name=function(self) return "derived" end,
                OnStart=function(self)
                    assert(#self.items == 0)
                    table.insert(self.items, self.actor:GetUID())
                    assert(self.nested == self.alias)
                    self.nested.count = self.nested.count + 1
                    assert(self.nested.count == 1)
                    assert(self.cycle.self == self.cycle)
                    assert(self:method_name() == "derived")
                    assert(self.setting == 9)
                end
            }, {__index=DefaultsBase})
        )";
        std::ofstream(path / "component_types/ThreadDefaults.lua") <<
            "ThreadDefaults = {worker=coroutine.create(function() end)}";
        std::ofstream(path / "actor_templates/Isolated.template") <<
            R"({"name":"spawned","components":{"logic":{"type":"Isolation"}}})";
        std::ofstream(path / "actor_templates/Bad.template") <<
            R"({"name":"bad","components":{"logic":{"type":"UnknownDefaults"}}})";
        ResourcePath::SetResourcesRootPath(path);
    }
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void TestEpisode() {
    Engine engine;
    Check(engine.GetLastSceneLoadError().empty(), engine.GetLastSceneLoadError());
    engine.InitializeHeadless();
    engine.StepSimulation();
    auto first = ComponentManager::GetComponentByKey(1, "logic");
    lua_State *state = first.state();
    Check(ComponentManager::GetRuntimeErrorCount() == 0,
          ComponentManager::GetLastRuntimeError());
    RunLua(state, R"(
        one = Actor.Find("one"):GetComponent("Isolation")
        two = Actor.Find("two"):GetComponent("Isolation")
        override = Actor.Find("override"):GetComponent("Isolation")
        assert(one.seen == 0 and two.seen == 0)
        assert(#one.values == 1 and #two.values == 1)
        assert(one.values ~= two.values and one.values ~= Isolation.values)
        assert(#Isolation.values == 0)
        assert(one.alias_a ~= two.alias_a)
        assert(one.alias_a.nested.count == 1 and two.alias_a.nested.count == 1)
        assert(Isolation.alias_a.nested.count == 0)
        assert(one.vector.x == 2 and two.vector.x == 2 and Isolation.vector.x == 1)
        assert(one.cursor.x == 1 and two.cursor.x == 1 and Isolation.cursor.x == 0)
        assert(one.special:Read() == 6 and two.special:Read() == 6)
        assert(Isolation.special:Read() == 5)
        assert(override.configured == 123 and one.configured == 5)
        assert(override.seen == 2 and override.values[1] == 7 and override.values[2] == 8)
        assert(rawget(one, "OnStart") == nil and one.OnStart == Isolation.OnStart)
        assert(one:TouchShared() == 1 and two:TouchShared() == 2)
        assert(one:TouchGlobal() == 1 and two:TouchGlobal() == 2)
        local derived_one = Actor.Find("derived_one"):GetComponent("DerivedDefaults")
        local derived_two = Actor.Find("derived_two"):GetComponent("DerivedDefaults")
        assert(derived_one.items ~= derived_two.items)
        assert(#derived_one.items == 1 and #derived_two.items == 1)
        assert(#DefaultsBase.items == 0 and DefaultsBase.nested.count == 0)
        assert(derived_one.alias == derived_one.nested)
        assert(derived_one.alias ~= derived_two.alias)
        assert(rawget(derived_one, "method_name") == nil)
        assert(derived_one:method_name() == "derived")

        Isolation.default_actor = Actor.Find("one")
        added = Actor.Find("one"):AddComponent("Isolation")
        spawned_actor = Actor.Instantiate("Isolated")
        spawned = spawned_actor:GetComponent("Isolation")
        assert(added.default_actor == Actor.Find("one"))
        assert(spawned.default_actor == Actor.Find("one"))
        assert(added.values ~= one.values and spawned.values ~= added.values)
    )");
    engine.StepSimulation();
    Check(ComponentManager::GetRuntimeErrorCount() == 0,
          ComponentManager::GetLastRuntimeError());
    RunLua(state, R"(
        assert(added.seen == 0 and spawned.seen == 0)
        assert(#added.values == 1 and #spawned.values == 1)
        assert(added.alias_a.nested.count == 1 and spawned.alias_a.nested.count == 1)
        assert(added.vector.x == 2 and spawned.vector.x == 2)
        local ok, err = pcall(function() Actor.Find("one"):AddComponent("UnknownDefaults") end)
        assert(not ok and string.find(err, "OnStart"))
        ok, err = pcall(function() Actor.Find("one"):AddComponent("ThreadDefaults") end)
        assert(not ok and string.find(err, "OnStart"))
        ok, err = pcall(function() Actor.Instantiate("Bad") end)
        assert(not ok and string.find(err, "OnStart"))
        assert(Actor.Find("bad") == nil)
    )");
    engine.StepSimulation();
    Check(engine.GetRuntimeActors().size() == 6,
          "rejected mutable defaults must not retain partial spawned actors");
}
} // namespace

int main() {
    try {
        Fixture fixture;
        TestEpisode();
        TestEpisode();
        std::cout << "PASS component defaults: instance isolation, graph/value copies, overrides, spawn/add, reset\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL component isolation: " << error.what() << '\n';
        return 1;
    }
}
