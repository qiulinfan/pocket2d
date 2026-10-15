#include "../src/engine/scripting/manager/Internal.h"
#include "scripting/ComponentManager.h"
#include "shared/resources/ResourcePath.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
namespace fs = std::filesystem;
using namespace ManagerDetail;
void Check(bool condition, const std::string &message) { if (!condition) throw std::runtime_error(message); }
void Put(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary); output.write(text.data(), static_cast<std::streamsize>(text.size()));
    Check(static_cast<bool>(output), "cannot write scene script fixture");
}
void Lua(const char *code) {
    auto *state = g_runtime.lua_state; const int top = lua_gettop(state);
    const int status = luaL_dostring(state, code);
    const auto message = status == LUA_OK ? std::string() : std::string(lua_tostring(state, -1));
    lua_settop(state, top); Check(status == LUA_OK, message);
}
Actor Incoming(const std::string &type) {
    Actor actor; actor.uid = 10; actor.actor_name = "incoming";
    actor.component_specs.push_back({"component", type, {}}); return actor;
}
struct Fixture {
    fs::path previous = ResourcePath::ResourcesRootPath();
    fs::path root = fs::temp_directory_path() / ("pocket-scene-scripts-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path scripts = root / "component_types";
    Fixture() { fs::create_directories(scripts); ResourcePath::SetResourcesRootPath(root); }
    ~Fixture() { ComponentManager::Shutdown(); ResourcePath::SetResourcesRootPath(previous); std::error_code ignored; fs::remove_all(root, ignored); }
};
void Run() {
    Fixture fixture;
    Put(fixture.scripts / "Shared.lua", "bootstrap_runs=(bootstrap_runs or 0)+1;Shared={value=1}");
    Put(fixture.scripts / "a/Shared.lua", "bootstrap_runs=(bootstrap_runs or 0)+1;Shared={value=2}");
    Put(fixture.scripts / "b/Shared.lua", "Shared={value=3}");
    Put(fixture.scripts / "Broken.lua", "Broken={fallback=true}");
    Put(fixture.scripts / "a/Broken.lua", "Broken={invalid syntax");
    Put(fixture.scripts / "SpriteRenderer.lua", "error('native shadow must not execute');SpriteRenderer={}");
    const auto io_path = (fixture.root / "bootstrap-side-effect.txt").generic_string();
    Put(fixture.scripts / "Trusted.lua", "local f=assert(io.open([=[" + io_path + "]=],'w'));f:write('trusted');f:close();Trusted={roll=math.random(1,100),vector=Vector2(3,4)}");
    ComponentManager::SetRandomSeed(123);
    ComponentManager::Initialize();
    auto *state = g_runtime.lua_state;
    Check(state != nullptr && !g_runtime.component_types_bootstrapped && g_runtime.component_type_tables.empty(), "Initialize still executed project code");
    Check(!fs::exists(fixture.root / "bootstrap-side-effect.txt"), "Initialize executed trusted file I/O");
    Lua("assert(type(SpriteRenderer)=='table' and Trusted==nil and Shared==nil)");
    ComponentManager::BootstrapComponentTypes("a");
    Check(g_runtime.component_types_bootstrapped && ComponentManager::IsRegisteredComponentType("Shared"), "bootstrap did not register winner");
    Check(!ComponentManager::IsRegisteredComponentType("Broken") && ComponentManager::GetComponentTypeWarningCount() == 1, "bad preferred script fell back to another source");
    Lua("assert(bootstrap_runs==1 and Shared.value==2 and type(Trusted.vector)=='userdata' and Trusted.roll>=1 and Trusted.roll<=100);assert(type(SpriteRenderer)=='table')");
    Check(fs::exists(fixture.root / "bootstrap-side-effect.txt"), "trusted bootstrap capabilities changed");
    Check(ComponentManager::GetComponentScriptSource("SpriteRenderer").empty(), "native shadow leaked into script source metadata");
    const auto selected_path = fs::canonical(fixture.scripts / "a/Shared.lua").generic_string();
    Check(ComponentManager::GetComponentScriptSource("Shared") == selected_path, "bootstrap source was not canonical preferred path");
    ComponentManager::BootstrapComponentTypes("b");
    Lua("assert(bootstrap_runs==1 and Shared.value==2)");
    fs::remove(fixture.scripts / "Broken.lua"); fs::remove(fixture.scripts / "a/Broken.lua");

    std::string error;
    auto plan = ComponentManager::PrepareSceneScripts("a", {Incoming("Shared"), Incoming("Trusted")}, {}, error);
    Check(plan && error.empty() && !lua_gc(state, LUA_GCISRUNNING), "prepare did not own GC pause");
    Check(AreSameLuaRef(ComponentManager::GetPreparedSceneType(*plan, "Shared"), g_runtime.component_type_tables.at("Shared")), "same canonical source did not reuse prototype identity");
    Check(ComponentManager::GetPreparedSceneType(*plan, "Trusted")["vector"].isUserdata(), "cached native defaults were restaged as declarative Lua");
    Check(ComponentManager::GetPreparedSceneType(*plan, "Missing").isNil(), "missing prepared type must be nil");
    auto nested = ComponentManager::PrepareSceneScripts("a", {}, {}, error);
    Check(nested && !lua_gc(state, LUA_GCISRUNNING), "nested plan preparation resumed collection");
    PauseScenePreparationGC(state); // Same ownership used by detached component tokens.
    plan.reset(); Check(!lua_gc(state, LUA_GCISRUNNING), "outer plan prematurely resumed nested GC guard");
    nested.reset(); Check(!lua_gc(state, LUA_GCISRUNNING), "last plan resumed a remaining detached-token GC guard");
    ResumeScenePreparationGC(state); Check(lua_gc(state, LUA_GCISRUNNING), "last shared guard did not restore GC mode");
    lua_gc(state, LUA_GCSTOP);
    { auto stopped = ComponentManager::PrepareSceneScripts("a", {}, {}, error); Check(stopped != nullptr, error); }
    Check(!lua_gc(state, LUA_GCISRUNNING), "plan restarted previously stopped GC"); lua_gc(state, LUA_GCRESTART);
    { auto original = ComponentManager::PrepareSceneScripts("a", {}, {}, error); Check(original != nullptr, error);
      SceneScriptPlan moved(std::move(*original)); original.reset(); Check(!lua_gc(state, LUA_GCISRUNNING), "moving plan lost GC ownership"); }
    Check(lua_gc(state, LUA_GCISRUNNING), "moved plan did not restore GC");

    // Same-path source edits stay in memory until explicit reload/project reset.
    Put(fixture.scripts / "a/Shared.lua", "invalid syntax;Actor.Destroy('bad')");
    plan = ComponentManager::PrepareSceneScripts("a", {Incoming("Shared")}, {}, error);
    Check(plan && ComponentManager::GetPreparedSceneType(*plan, "Shared")["value"].cast<int>().value() == 2, "same-path preparation reread/reexecuted code");
    plan.reset();
    fs::remove(fixture.scripts / "Trusted.lua");
    plan = ComponentManager::PrepareSceneScripts("a", {Incoming("Trusted")}, {}, error);
    Check(plan && ComponentManager::GetPreparedSceneType(*plan, "Trusted")["vector"].isUserdata(), "deleted source evicted live module"); plan.reset();

    Put(fixture.scripts / "NewUnused.lua", "NewUnused={};Actor.Destroy('bad')");
    plan = ComponentManager::PrepareSceneScripts("a", {Incoming("Absent")}, {}, error);
    Check(!plan && error.find("missing Lua component") != std::string::npos, "missing incoming type did not fail before candidate evaluation");
    plan = ComponentManager::PrepareSceneScripts("a", {Incoming("Shared")}, {}, error);
    Check(!plan && error.find("unavailable") != std::string::npos && lua_gc(state, LUA_GCISRUNNING), "unused chosen source was not staged/rolled back");
    Lua("assert(NewUnused==nil and Shared.value==2 and bootstrap_runs==1)");
    Check(!ComponentManager::IsRegisteredComponentType("NewUnused") && ComponentManager::GetComponentScriptSource("NewUnused").empty(), "failed plan published registry/source changes");
    Put(fixture.scripts / "NewUnused.lua", "NewUnused={value=4}");
    const auto new_path = fs::canonical(fixture.scripts / "NewUnused.lua").generic_string();
    plan = ComponentManager::PrepareSceneScripts("a", {Incoming("NewUnused")}, {}, error);
    Check(plan && ComponentManager::GetPreparedSceneType(*plan, "NewUnused")["value"].cast<int>().value() == 4, error);
    Lua("assert(NewUnused==nil)");
    Check(!g_runtime.component_source_cache.contains(new_path), "uncommitted source cache became visible");
    plan.reset();
    Check(!ComponentManager::IsRegisteredComponentType("NewUnused"), "discarded plan registered candidate");
    Check(!g_runtime.component_source_cache.contains(new_path), "discarded plan published source cache");
    plan = ComponentManager::PrepareSceneScripts("a", {}, {}, error); Check(plan != nullptr, error);
    ComponentManager::CommitSceneScripts(*plan);
    Check(ComponentManager::IsRegisteredComponentType("NewUnused") && ComponentManager::GetPreparedSceneType(*plan, "NewUnused")["value"].cast<int>().value() == 4,
          "commit did not publish prepared registry");
    Lua("assert(NewUnused.value==4 and Shared.value==2)"); plan.reset();
    Check(!ComponentManager::GetComponentScriptSource("NewUnused").empty(), "commit did not publish canonical source");

    std::list<Actor> actors;
    actors.push_back(Incoming("Shared")); actors.back().AssignRuntimeGeneration();
    ComponentManager::BindActorsForScene(actors);
    ComponentManager::InstantiateComponentForActor(10, actors.back().component_specs[0]);
    plan = ComponentManager::PrepareSceneScripts("b", {}, {10}, error);
    Check(!plan && error.find("persistent actor") != std::string::npos, "persistent instance allowed selected prototype replacement");
    Lua("assert(Shared.value==2)");
    Check(ComponentManager::GetComponentScriptSource("Shared") == selected_path, "persistent rejection changed source metadata");
    ComponentManager::ClearActorComponents();
    plan = ComponentManager::PrepareSceneScripts("b", {Incoming("Shared")}, {}, error);
    Check(plan && ComponentManager::GetPreparedSceneType(*plan, "Shared")["value"].cast<int>().value() == 3, error);
    Lua("assert(Shared.value==2)"); ComponentManager::CommitSceneScripts(*plan); plan.reset();
    Lua("assert(Shared.value==3 and bootstrap_runs==1)");

    // Preferred bad winner must not fall through to a valid root duplicate.
    Put(fixture.scripts / "c/Shared.lua", "Shared={not valid Lua}");
    plan = ComponentManager::PrepareSceneScripts("c", {}, {}, error);
    Check(!plan && !error.empty(), "failed preferred path fell back during prepare"); Lua("assert(Shared.value==3)");
    fs::remove(fixture.scripts / "c/Shared.lua");
    Put(fixture.scripts / "Binary.lua", std::string("Binary={value='\xff'}"));
    plan = ComponentManager::PrepareSceneScripts("b", {}, {}, error);
    Check(!plan && error.find("UTF-8") != std::string::npos, "invalid UTF8 source accepted"); fs::remove(fixture.scripts / "Binary.lua");
    Put(fixture.scripts / "Large.lua", std::string(1024 * 1024 + 1, ' '));
    plan = ComponentManager::PrepareSceneScripts("b", {}, {}, error);
    Check(!plan && error.find("1 MiB") != std::string::npos, "oversized source accepted"); fs::remove(fixture.scripts / "Large.lua");
    Put(fixture.scripts / "Verbose.lua", "error(string.rep('x',10000))");
    plan = ComponentManager::PrepareSceneScripts("b", {}, {}, error);
    Check(!plan && error.size() <= 4096 && error.find("truncated") != std::string::npos, "scene diagnostics are not bounded"); fs::remove(fixture.scripts / "Verbose.lua");
    Put(fixture.root / "Escape.lua", "Escape={}");
    std::error_code symlink_error;
    fs::create_symlink(fixture.root / "Escape.lua", fixture.scripts / "Escape.lua", symlink_error);
    if (!symlink_error) {
        plan = ComponentManager::PrepareSceneScripts("b", {}, {}, error);
        Check(!plan && error.find("escapes") != std::string::npos, "outside source symlink accepted");
        fs::remove(fixture.scripts / "Escape.lua");
        const auto cache_size = g_runtime.component_source_cache.size();
        fs::create_symlink(fixture.scripts / "b/Shared.lua", fixture.scripts / "Alias.lua");
        plan = ComponentManager::PrepareSceneScripts("b", {}, {}, error);
        Check(!plan && error.find("one canonical component source") != std::string::npos,
              "different type names sharing one canonical source were accepted");
        Check(g_runtime.component_source_cache.size() == cache_size && !ComponentManager::IsRegisteredComponentType("Alias"),
              "rejected canonical alias changed the registry/cache");
        Lua("assert(Shared.value==3 and Alias==nil)");
        fs::remove(fixture.scripts / "Alias.lua");
    } else {
#ifdef _WIN32
        std::cout << "source symlink check skipped: " << symlink_error.message() << '\n';
#else
        Check(false, "could not create source symlink fixture");
#endif
    }
    Check(lua_gettop(state) == 0 && lua_gc(state, LUA_GCISRUNNING), "scene preparation left stack/GC state changed");
    ComponentManager::Shutdown();
    Check(!g_runtime.component_types_bootstrapped, "project shutdown did not reset bootstrap flag");
    Check(g_runtime.component_source_cache.empty(), "project shutdown retained source cache LuaRefs");
    Check(g_runtime.component_source_owners.empty(), "project shutdown retained canonical source owners");
    Check(g_runtime.reserved_component_type_names.empty(), "project shutdown retained host reserved-name snapshot");
    // A new project lifetime bootstraps code again and drops deleted modules.
    Put(fixture.scripts / "a/Shared.lua", "Shared={value=8}");
    ComponentManager::Initialize(); ComponentManager::BootstrapComponentTypes("a");
    Lua("assert(Shared.value==8 and Trusted==nil)");
    Check(!ComponentManager::IsRegisteredComponentType("Trusted"), "project reset retained a deleted module");
}
void SourceVariantCache() {
    Fixture fixture;
    Put(fixture.scripts / "Variant.lua", R"(
        LOADS=(LOADS or 0)+1
        local private_count=0
        Variant={name="root",Next=function() private_count=private_count+1;return private_count end}
    )");
    Put(fixture.scripts / "zone/Variant.lua", "Variant={name='zone',Next=function() return 200 end}");
    ComponentManager::Initialize(); ComponentManager::BootstrapComponentTypes({});
    Lua("assert(LOADS==1 and Variant.name=='root' and Variant.Next()==1)");
    const auto root_path = fs::canonical(fixture.scripts / "Variant.lua").generic_string();
    const auto zone_path = fs::canonical(fixture.scripts / "zone/Variant.lua").generic_string();
    Check(g_runtime.component_source_cache.size() == 1 && g_runtime.component_source_cache.contains(root_path), "bootstrap did not cache its canonical module");
    std::string error;
    auto zone = ComponentManager::PrepareSceneScripts("zone", {Incoming("Variant")}, {}, error);
    Check(zone && !g_runtime.component_source_cache.contains(zone_path), "candidate source cache published before commit");
    ComponentManager::CommitSceneScripts(*zone); zone.reset();
    Lua("assert(LOADS==1 and Variant.name=='zone' and Variant.Next()==200)");
    Check(g_runtime.component_source_cache.size() == 2, "variant switch dropped the old source cache");
    Put(fixture.scripts / "Variant.lua", "error('cached root must not execute'); Variant={bad syntax");

    // A cached path remains a source-definition change for persistent actors.
    std::list<Actor> actors{Incoming("Variant")};
    ComponentManager::BindActorsForScene(actors);
    ComponentManager::InstantiateComponentForActor(10, actors.front().component_specs.front());
    auto root = ComponentManager::PrepareSceneScripts({}, {}, {10}, error);
    Check(!root && error.find("persistent actor") != std::string::npos, "source-cache hit bypassed persistent-type conflict");
    Lua("assert(Variant.name=='zone' and LOADS==1)");
    ComponentManager::ClearActorComponents();
    root = ComponentManager::PrepareSceneScripts({}, {Incoming("Variant")}, {}, error);
    Check(root && ComponentManager::GetPreparedSceneType(*root, "Variant")["name"].cast<std::string>().value() == "root", error);
    Check(AreSameLuaRef(ComponentManager::GetPreparedSceneType(*root, "Variant"), g_runtime.component_source_cache.at(root_path)), "return to root cloned/reexecuted its module");
    ComponentManager::CommitSceneScripts(*root); root.reset();
    Lua("assert(Variant.name=='root' and LOADS==1 and Variant.Next()==2)");

    Put(fixture.scripts / "NewA.lua", "NewA={value=1}");
    Put(fixture.scripts / "NewZ.lua", "NewZ={broken syntax");
    const auto count = g_runtime.component_source_cache.size();
    auto failed = ComponentManager::PrepareSceneScripts({}, {}, {}, error);
    Check(!failed && g_runtime.component_source_cache.size() == count && !ComponentManager::IsRegisteredComponentType("NewA"), "failed batch published a new source cache entry");
    Lua("assert(NewA==nil and NewZ==nil and LOADS==1 and Variant.name=='root')");
    fs::remove(fixture.scripts / "NewA.lua"); fs::remove(fixture.scripts / "NewZ.lua");
    fs::remove(fixture.scripts / "zone/Variant.lua");
    Check(g_runtime.component_source_cache.contains(zone_path), "deleting an unselected source evicted its module");
}
void BootstrapAliasRejection() {
    Fixture fixture;
    Put(fixture.scripts / "Original.lua", "alias_executions=(alias_executions or 0)+1;Original={};Alias={}");
    std::error_code error;
    fs::create_symlink(fixture.scripts / "Original.lua", fixture.scripts / "Alias.lua", error);
    if (error) {
#ifdef _WIN32
        std::cout << "bootstrap source-alias check skipped: " << error.message() << '\n'; return;
#else
        Check(false, "could not create bootstrap alias fixture");
#endif
    }
    ComponentManager::Initialize();
    bool rejected = false;
    try { ComponentManager::BootstrapComponentTypes({}); }
    catch (const std::exception &failure) { rejected = std::string(failure.what()).find("one canonical component source") != std::string::npos; }
    Check(rejected && !g_runtime.component_types_bootstrapped && g_runtime.component_type_tables.empty() && g_runtime.component_source_cache.empty(),
          "bootstrap canonical alias was executed or cached before rejection");
    Lua("assert(alias_executions==nil and Original==nil and Alias==nil)");
}
void ReservedHostNames() {
    for (const char *name : {"Actor", "Input", "Music", "vec2", "math"}) {
        Fixture fixture;
        Put(fixture.scripts / "AAA.lua", "unrelated_bootstrap_runs=(unrelated_bootstrap_runs or 0)+1;AAA={value=1}");
        const auto conflicting = fixture.scripts / (std::string(name) + ".lua");
        Put(conflicting, std::string(name) + "={shadow=true}");
        ComponentManager::Initialize();
        {
            auto *state = g_runtime.lua_state;
            auto original = luabridge::getGlobal(state, name);
            Check(original.isTable() && g_runtime.reserved_component_type_names.contains(name),
                  std::string("actual host global missing from initialization snapshot: ") + name);
            bool rejected = false;
            try { ComponentManager::BootstrapComponentTypes({}); }
            catch (const std::exception &failure) { rejected = std::string(failure.what()).find("reserved host") != std::string::npos; }
            Check(rejected && !g_runtime.component_types_bootstrapped && g_runtime.component_type_tables.empty() && g_runtime.component_source_cache.empty(),
                  "reserved filename reached trusted bootstrap evaluation");
            Check(AreSameLuaRef(original, luabridge::getGlobal(state, name)), "reserved filename changed original host identity");
            Lua("assert(unrelated_bootstrap_runs==nil and AAA==nil)");
            fs::remove(conflicting);
            ComponentManager::BootstrapComponentTypes({});
            Lua("assert(unrelated_bootstrap_runs==1 and AAA.value==1)");
            Put(conflicting, std::string(name) + "={shadow=true}");
            const auto cache_size = g_runtime.component_source_cache.size();
            std::string error;
            auto plan = ComponentManager::PrepareSceneScripts({}, {}, {}, error);
            Check(!plan && error.find("reserved host") != std::string::npos, "new reserved filename accepted during scene preparation");
            Check(AreSameLuaRef(original, luabridge::getGlobal(state, name)) && g_runtime.component_source_cache.size() == cache_size,
                  "reserved preparation changed host identity/cache");
            Lua("assert(unrelated_bootstrap_runs==1 and AAA.value==1)");
        }
        ComponentManager::Shutdown();
        Check(g_runtime.reserved_component_type_names.empty(), "host reserved-name snapshot survived shutdown");
    }
}
void HistoricalSourceOwnership() {
    Fixture fixture;
    const auto physical = fixture.scripts / "data/A.lua";
    Put(physical, "A={value=1}");
    std::error_code link_error;
    fs::create_symlink(physical, fixture.scripts / "A.lua", link_error);
    if (link_error) {
#ifdef _WIN32
        std::cout << "historical source-owner check skipped: " << link_error.message() << '\n'; return;
#else
        Check(false, "could not create historical source fixture");
#endif
    }
    ComponentManager::Initialize(); ComponentManager::BootstrapComponentTypes({});
    const auto physical_key = fs::canonical(physical).generic_string();
    Check(g_runtime.component_source_owners.at(physical_key) == "A", "legal Lua symlink did not record its type owner");
    Lua("assert(A.value==1)");
    Put(fixture.scripts / "zone/A.lua", "A={value=2}");
    std::string error;
    auto zone = ComponentManager::PrepareSceneScripts("zone", {Incoming("A")}, {}, error);
    Check(zone != nullptr, error); ComponentManager::CommitSceneScripts(*zone); zone.reset();
    Check(g_runtime.component_type_sources.at("A") != physical_key && g_runtime.component_source_owners.at(physical_key) == "A",
          "switching variants lost historical type ownership");
    fs::remove(fixture.scripts / "A.lua");
    fs::create_symlink(physical, fixture.scripts / "B.lua");
    const auto owners = g_runtime.component_source_owners;
    const auto cache_size = g_runtime.component_source_cache.size();
    auto renamed = ComponentManager::PrepareSceneScripts("zone", {Incoming("B")}, {}, error);
    Check(!renamed && error.find("already belongs to type 'A'") != std::string::npos,
          "historically cached A source was silently registered as B");
    Check(g_runtime.component_source_owners == owners && g_runtime.component_source_cache.size() == cache_size &&
          !g_runtime.component_type_sources.contains("B"), "historical owner rejection published metadata");
    Lua("assert(A.value==2 and B==nil)");
    fs::remove(fixture.scripts / "B.lua");
    Put(fixture.scripts / "Fresh.lua", "Fresh={value=3}");
    auto fresh = ComponentManager::PrepareSceneScripts("zone", {}, {}, error);
    Check(fresh != nullptr, error);
    const auto fresh_key = fs::canonical(fixture.scripts / "Fresh.lua").generic_string();
    Check(!g_runtime.component_source_owners.contains(fresh_key), "prepared candidate owner published before commit");
    fresh.reset();
    Check(!g_runtime.component_source_owners.contains(fresh_key), "discarded candidate owner leaked");
    fs::remove(fixture.scripts / "Fresh.lua");
    Put(fixture.scripts / "zone/A.lua", "A={value=4}");
    const auto reloaded = ComponentManager::ReloadScripts({"A"});
    Check(reloaded.ok && g_runtime.component_source_owners == owners, "same-source explicit reload changed canonical owners");
    Lua("assert(A.value==4)");
}
void NonLuaCanonicalTarget() {
    Fixture fixture;
    Put(fixture.scripts / "data/source.txt", "Alias={value=1}");
    Put(fixture.scripts / "AAA.lua", "unexpected_source_execution=true;AAA={}");
    std::error_code error;
    fs::create_symlink(fixture.scripts / "data/source.txt", fixture.scripts / "Alias.lua", error);
    if (error) {
#ifdef _WIN32
        std::cout << "non-Lua canonical target check skipped: " << error.message() << '\n'; return;
#else
        Check(false, "could not create non-Lua canonical target fixture");
#endif
    }
    ComponentManager::Initialize();
    bool rejected = false;
    try { ComponentManager::BootstrapComponentTypes({}); }
    catch (const std::exception &failure) { rejected = std::string(failure.what()).find("canonical component source must end in .lua") != std::string::npos; }
    Check(rejected && !g_runtime.component_types_bootstrapped && g_runtime.component_source_cache.empty() && g_runtime.component_source_owners.empty(),
          "non-Lua canonical target was executed/cached during bootstrap");
    Lua("assert(unexpected_source_execution==nil and Alias==nil and AAA==nil)");
    fs::remove(fixture.scripts / "Alias.lua");
    ComponentManager::BootstrapComponentTypes({});
    fs::create_symlink(fixture.scripts / "data/source.txt", fixture.scripts / "Alias.lua");
    std::string message;
    auto plan = ComponentManager::PrepareSceneScripts({}, {}, {}, message);
    Check(!plan && message.find("canonical component source must end in .lua") != std::string::npos && !g_runtime.component_type_sources.contains("Alias"),
          "non-Lua canonical target accepted by later preparation");
}
} // namespace
int main() {
    try { Run(); SourceVariantCache(); BootstrapAliasRejection(); ReservedHostNames(); HistoricalSourceOwnership(); NonLuaCanonicalTarget(); std::cout << "SceneScripts: trusted bootstrap, isolated selection, cache lifetime, GC guards and atomic registry preparation passed\n"; return 0; }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
