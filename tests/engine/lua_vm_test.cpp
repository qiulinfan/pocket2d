#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void Check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

class LuaState {
public:
    lua_State *state = luaL_newstate();
    LuaState() {
        Check(state != nullptr, "Lua VM allocation failed");
        luaL_openlibs(state);
    }
    ~LuaState() { lua_close(state); }
    void Run(const char *source) {
        if (luaL_dostring(state, source) != LUA_OK) {
            const std::string error = lua_tostring(state, -1);
            lua_settop(state, 0);
            throw std::runtime_error(error);
        }
        Check(lua_gettop(state) == 0, "successful script must leave a clean stack");
    }
};

void TestStateEqualityAndCoroutineReferences() {
    LuaState first, second;
    auto *a = lua_newthread(first.state);
    auto *b = lua_newthread(first.state);
    Check(luabridge::equalstates(first.state, first.state) && luabridge::equalstates(a, a),
          "same live Lua thread must share its registry");
    Check(luabridge::equalstates(first.state, a) && luabridge::equalstates(a, b) &&
          luabridge::equalstates(b, first.state), "different coroutines in one VM must share registry");
    Check(!luabridge::equalstates(first.state, second.state) && !luabridge::equalstates(a, second.state) &&
          !luabridge::equalstates(second.state, b), "independent Lua VMs must remain distinct");
    {
        auto value = luabridge::newTable(first.state); value["answer"] = 42;
        lua_pushinteger(a, 17); value.push(a);
        auto alias = luabridge::LuaRef::fromStack(a, -1);
        Check(alias.rawequal(value) && alias["answer"].cast<int>().value() == 42,
              "coroutine push/reference changed table identity");
        alias["answer"] = 43;
        Check(value["answer"].cast<int>().value() == 43, "cross-coroutine alias did not retain shared value");
        lua_pop(a, 1);
        Check(lua_gettop(a) == 1 && lua_tointeger(a, 1) == 17 && lua_gettop(b) == 0 &&
              lua_gettop(first.state) == 2 && lua_gettop(second.state) == 0,
              "state comparison/reference push corrupted Lua stack");
        lua_pop(a, 1);
    }
    lua_pop(first.state, 2);
}

int hook_lua_calls = 0;
int hook_yields = 0;
void YieldingHook(lua_State *state, lua_Debug *event) {
    if (event->event == LUA_HOOKCALL) {
        lua_getinfo(state, "S", event);
        if (std::strcmp(event->what, "Lua") == 0) ++hook_lua_calls;
    } else if (event->event == LUA_HOOKCOUNT) {
        ++hook_yields;
        lua_yield(state, 0);
    }
}

void TestHooksAndCoroutineResume() {
    LuaState vm;
    lua_State *thread = lua_newthread(vm.state); // Retained on the parent's stack.
    Check(luaL_loadstring(thread, "local result = (function(value) return value * 3 end)(7); return result") == LUA_OK,
          "coroutine load failed");
    hook_lua_calls = 0;
    hook_yields = 0;
    lua_sethook(thread, YieldingHook, LUA_MASKCALL | LUA_MASKCOUNT, 1);
    int results = 0;
    int status = LUA_YIELD;
    int resumes = 0;
    while (status == LUA_YIELD && resumes++ < 1000)
        status = lua_resume(thread, nullptr, 0, &results);
    Check(status == LUA_OK && results == 1 && lua_tointeger(thread, -1) == 21,
          "count-hook yield/resume must preserve the continuation and result");
    Check(hook_yields > 0, "test must exercise yielding count hooks");
    Check(hook_lua_calls == 1, "yielding count hooks must not duplicate the Lua call hook");
    lua_sethook(thread, nullptr, 0, 0);
    lua_pop(vm.state, 1);
}

int finalized = 0;
int FinalizeTracked(lua_State *) {
    ++finalized;
    return 0;
}

int NewTracked(lua_State *state) {
    lua_newuserdatauv(state, 1, 0);
    if (luaL_newmetatable(state, "pocket2d.vm.test.tracked")) {
        lua_pushcfunction(state, FinalizeTracked);
        lua_setfield(state, -2, "__gc");
    }
    lua_setmetatable(state, -2);
    return 1;
}

void TestGCAndWeakReferences() {
    finalized = 0;
    {
        LuaState vm;
        lua_pushcfunction(vm.state, NewTracked);
        lua_setglobal(vm.state, "NewTracked");
        vm.Run(R"lua(
            local weak = setmetatable({}, {__mode = "v"})
            do
                local object = NewTracked()
                weak[1] = object
                assert(weak[1] ~= nil)
            end
            collectgarbage("collect")
            collectgarbage("collect")
            assert(weak[1] == nil, "weak refs must not retain native userdata")
            held_until_shutdown = NewTracked()
            collectgarbage("collect")
        )lua");
        Check(finalized == 1, "unreachable native userdata must finalize once");
    }
    Check(finalized == 2, "VM shutdown must finalize still-referenced userdata once");
}

struct NumberBox {
    int value = 0;
    void Add(int amount) { value += amount; }
};

void TestErrorsAndExistingBinding() {
    LuaState vm;
    Check(luaL_loadstring(vm.state, "local broken = )") == LUA_ERRSYNTAX,
          "invalid source must be a syntax error");
    Check(lua_isstring(vm.state, -1), "syntax error must carry a diagnostic");
    lua_settop(vm.state, 0);
    Check(luaL_loadstring(vm.state, "error('intentional-runtime-error')") == LUA_OK,
          "runtime error fixture must compile successfully");
    Check(lua_pcall(vm.state, 0, 0, 0) == LUA_ERRRUN,
          "protected execution must return a runtime error");
    Check(std::string(lua_tostring(vm.state, -1)).find("intentional-runtime-error") != std::string::npos,
          "runtime diagnostic must preserve the original message");
    lua_settop(vm.state, 0);
    luabridge::getGlobalNamespace(vm.state)
        .beginClass<NumberBox>("NumberBox")
        .addConstructor<void (*)()>()
        .addPropertyReadWrite("value", &NumberBox::value)
        .addFunction("Add", &NumberBox::Add)
        .endClass();
    vm.Run(R"lua(
        local box = NumberBox()
        box:Add(4)
        assert(box.value == 4)
        assert(not pcall(function() box:Add({}) end), "invalid binding argument must fail")
        box:Add(2)
        assert(box.value == 6, "binding remains usable after an error")
        local predicate_key = {[true] = "ok"}
        assert(predicate_key[1 < 2] == "ok", "comparison expression must work as a table key")
        assert(utf8.len(string.char(255, 143, 143, 143, 143, 143, 143, 143)) == nil)
    )lua");
}
} // namespace

int main() {
    try {
        Check(LUA_VERSION_NUM == 504, "maintenance update must retain the Lua 5.4 API");
        TestStateEqualityAndCoroutineReferences();
        TestHooksAndCoroutineResume();
        TestGCAndWeakReferences();
        TestErrorsAndExistingBinding();
        std::cout << LUA_RELEASE << " VM and LuaBridge regressions passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
