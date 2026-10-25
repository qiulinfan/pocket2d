#include "core/Engine.h"
#include "scripting/ComponentManager.h"
#include "shared/config/GameConfig.h"
#include "shared/resources/ResourcePath.h"
#include "SDL3/SDL.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const std::string &message) { if (!value) throw std::runtime_error(message); }
void Lua(const char *code) {
    lua_State *state = ComponentManager::GetComponentByKey(0, "missing").state();
    if (luaL_dostring(state, code) != LUA_OK) {
        const std::string error = lua_tostring(state, -1); lua_pop(state, 1);
        throw std::runtime_error(error);
    }
}
struct Fixture {
    std::filesystem::path previous = ResourcePath::ResourcesRootPath();
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("pocket-save-lua-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string previous_env;
    bool had_env = false;
    Fixture() {
        if (const char *value = SDL_getenv("POCKET_SAVE_ROOT")) { had_env = true; previous_env = value; }
        std::filesystem::create_directories(root / "project");
        ResourcePath::SetResourcesRootPath(root / "project");
        SDL_setenv_unsafe("POCKET_SAVE_ROOT", (root / "saves").string().c_str(), 1);
    }
    void Config(const std::string &name) {
        std::ofstream(root / "project/game.config") << "{\"game_title\":\"save test\",\"save_namespace\":\"" << name << "\"}";
    }
    ~Fixture() {
        ResourcePath::SetResourcesRootPath(previous);
        if (had_env) SDL_setenv_unsafe("POCKET_SAVE_ROOT", previous_env.c_str(), 1);
        else SDL_unsetenv_unsafe("POCKET_SAVE_ROOT");
        std::error_code error; std::filesystem::remove_all(root, error);
    }
};
void RoundTrip(Fixture &fixture) {
    fixture.Config("alpha"); Engine engine; engine.InitializeHeadless();
    Lua(R"lua(
      local calls=0
      local shared={score=19}
      local data=setmetatable({truth=true, zero=0, fraction=1.25, low=math.mininteger,
          high=math.maxinteger, text="hello 世界", binary="a\0b", list={1,2,3},
          nested={empty={}, aliases={shared,shared}}}, {
          __pairs=function() calls=calls+1;error("pairs called") end,
          __index=function() calls=calls+1;error("index called") end})
      assert(Save.Write("main",3,data).ok)
      local result=Save.Read("main");assert(result.ok and result.schema_version==3)
      local d=result.data
      assert(d.truth and d.zero==0 and d.fraction==1.25 and d.low==math.mininteger and d.high==math.maxinteger)
      assert(math.type(d.high)=="integer" and math.type(d.fraction)=="float")
      assert(d.text=="hello 世界" and d.binary=="a\0b" and d.list[3]==3 and next(d.nested.empty)==nil)
      assert(d.nested.aliases[1].score==19 and d.nested.aliases[2].score==19)
      assert(d.nested.aliases[1]~=d.nested.aliases[2] and calls==0)
      local function rejected(value)
          local bad=Save.Write("main",3,value);assert(not bad.ok and bad.code=="invalid_data",bad.error)
          assert(Save.Read("main").data.high==math.maxinteger,"old slot was replaced")
      end
      local cycle={};cycle.self=cycle;rejected(cycle)
      rejected({[1]=1,[3]=3});rejected({[1]=1,name="mixed"});rejected({[0]=1})
      rejected({value=function()end});rejected({value=coroutine.create(function()end)})
      rejected({value=Vector2(1,2)});rejected({value=0/0});rejected({value=1/0})
      rejected({value=string.char(255)})
      local deep={};local cursor=deep;for i=1,33 do cursor.next={};cursor=cursor.next end;rejected(deep)
      local large={};for i=1,100001 do large[i]=i end;rejected(large)
      rejected({value=string.rep("x",4*1024*1024+1)})
      assert(not Save.Write("../escape",1,{}).ok)
      assert(not Save.Write("a/b",1,{}).ok and not Save.Write("bad\0slot",1,{}).ok)
      assert(not Save.Write("main",0,{}).ok and not Save.Write("main",1.5,{}).ok)
      assert(not Save.Write("main",1,"bad").ok and not Save.Read({}).ok)
      local absent=Save.Read("absent");assert(not absent.ok and absent.code=="not_found")
      assert(Save.Read("main").ok)
    )lua");
    auto config = GameConfig::Read(); Check(config.save_namespace == "alpha", "namespace config lost");
    Check(GameConfig::Write(config), "config write failed");
    Check(GameConfig::Read().save_namespace == "alpha", "namespace roundtrip failed");
    config.save_namespace = "../invalid";Check(!GameConfig::Write(config), "invalid namespace config saved");
}
void Isolation(Fixture &fixture) {
    fixture.Config("beta");
    { Engine engine;engine.InitializeHeadless();Lua("assert(Save.Read('main').code=='not_found');assert(Save.Write('main',1,{owner='beta'}).ok)"); }
    fixture.Config("alpha");
    { Engine engine;engine.InitializeHeadless();Lua("assert(Save.Read('main').schema_version==3);assert(Save.Read('main').data.high==math.maxinteger)"); }
    fixture.Config("");
    { Engine engine;engine.InitializeHeadless();Lua("assert(Save.Read('main').code=='not_configured');assert(Save.Write('main',1,{}).code=='not_configured')"); }
    fixture.Config("link");std::filesystem::create_directory_symlink(fixture.root / "saves/alpha",fixture.root / "saves/link");
    { Engine engine;engine.InitializeHeadless();Lua("assert(not Save.Read('main').ok);assert(not Save.Write('main',1,{}).ok)"); }
}
}
int main() {
    try { Fixture fixture;RoundTrip(fixture);Isolation(fixture);std::cout<<"PASS Lua save conversion, limits, config and project isolation\n";return 0; }
    catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
