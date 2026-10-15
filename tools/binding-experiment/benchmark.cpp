#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include "scripting/ScriptBudget.h"
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using Clock = std::chrono::steady_clock;
#if POCKET_BINDING_CANDIDATE
constexpr const char *Library = "LuaBridge3-rc12";
#else
constexpr const char *Library = "LuaBridge2.7";
#endif
void Check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
struct Value {
    double value = 0;
    void Add(double amount) { value += amount; }
};
struct VM {
    lua_State *state;
    VM() {
        ScriptBudget::Configure({});
        state = ScriptBudget::NewState();
        Check(state != nullptr, "cannot allocate Lua VM");
        luaL_openlibs(state);
#if POCKET_BINDING_CANDIDATE
        const auto project_panic = lua_atpanic(state, nullptr);
        lua_atpanic(state, project_panic);
        luabridge::enableExceptions(state);
        lua_atpanic(state, project_panic); // Keep the allocator owner's fatal policy.
#endif
    }
    ~VM() { ScriptBudget::CloseState(state); }
};
void Load(lua_State *state, const char *code) {
    if (luaL_loadbuffer(state, code, std::char_traits<char>::length(code), "@binding_benchmark.lua") != LUA_OK ||
        lua_pcall(state, 0, 0, 0) != LUA_OK)
        throw std::runtime_error(lua_tostring(state, -1));
}
void Invoke(const luabridge::LuaRef &function, int argument) {
#if POCKET_BINDING_CANDIDATE
    const auto result = function(argument);
    Check(static_cast<bool>(result), "candidate callback failed");
#else
    function(argument);
#endif
}
void HostDiscardCall(lua_State *state, const luabridge::LuaRef &function) {
    function.push(state);
    lua_pushinteger(state, 1);
    if (lua_pcall(state, 1, 0, 0) != LUA_OK)
        throw std::runtime_error(lua_tostring(state, -1));
}
int IntegerArg(const char *text, int minimum, int maximum) {
    std::size_t consumed = 0;
    const auto value = std::stoll(text, &consumed);
    Check(consumed == std::char_traits<char>::length(text) && value >= minimum && value <= maximum,
          "argument outside supported range");
    return static_cast<int>(value);
}
void Run(int iterations, int samples) {
    VM vm;
    auto *state = vm.state;
    Value native;
    auto registration = luabridge::getGlobalNamespace(state).beginClass<Value>("Value");
#if POCKET_BINDING_CANDIDATE
    registration.addPropertyReadWrite("value", &Value::value);
#else
    registration.addProperty("value", &Value::value);
#endif
    registration.addFunction("Add", &Value::Add).endClass();
    luabridge::setGlobal(state, &native, "native");
    Load(state, R"(
        counter=0
        function Property(n) for i=1,n do native.value=native.value+1 end end
        function Host(n) for i=1,n do native:Add(1) end end
        function Tick(n) counter=counter+n end
    )");
    auto property = luabridge::getGlobal(state, "Property");
    auto host = luabridge::getGlobal(state, "Host");
    auto tick = luabridge::getGlobal(state, "Tick");
    const int stack_top = lua_gettop(state);
    std::cout << "library,workload,iterations,sample,ns_per_operation\n";
    for (const std::string workload : {"lua_property_read_write", "lua_native_method", "callback_library_void", "callback_host_discard"}) {
        for (int sample=-3; sample<samples; ++sample) {
            native.value=0;
            lua_pushinteger(state, 0); lua_setglobal(state, "counter");
            lua_gc(state, LUA_GCCOLLECT);
            const auto start=Clock::now();
            if (workload=="lua_property_read_write") Invoke(property, iterations);
            else if (workload=="lua_native_method") Invoke(host, iterations);
            else if (workload=="callback_library_void") for(int i=0;i<iterations;++i) Invoke(tick,1);
            else for(int i=0;i<iterations;++i) HostDiscardCall(state,tick);
            const double elapsed=std::chrono::duration<double,std::nano>(Clock::now()-start).count();
            Check(lua_gettop(state)==stack_top, "benchmark leaked Lua stack");
            if(workload.starts_with("lua_")) Check(native.value==iterations, "native workload result mismatch");
            else {
                lua_getglobal(state,"counter");
                Check(lua_isinteger(state,-1)&&lua_tointeger(state,-1)==iterations,"callback workload result mismatch");
                lua_pop(state,1);
            }
            if(sample>=0) std::cout<<Library<<','<<workload<<','<<iterations<<','<<sample<<','
                <<std::fixed<<std::setprecision(3)<<elapsed/iterations<<'\n';
        }
    }
}
}
int main(int argc,char **argv) {
    try {
        Check(argc<=3,"usage: binding_benchmark_[baseline|candidate] [ITERATIONS] [SAMPLES]");
        Run(argc>1?IntegerArg(argv[1],1000,10000000):200000, argc>2?IntegerArg(argv[2],3,100):7);
        Check(ScriptBudget::HeapBytesUsed()==0,"Lua allocations survived shutdown");
        return 0;
    } catch(const std::exception &error) {
        std::cerr<<error.what()<<'\n';return 1;
    }
}
