#ifndef POCKET2D_LUA_BOUNDARY_H
#define POCKET2D_LUA_BOUNDARY_H

#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

// Owner-thread Lua boundaries. LuaBridge stays in result mode: enabling its
// registry exception policy lets argument checking throw across Lua pcall.
// Allocation failure in C++ is not converted into an ordinary script error;
// unprotected Lua panic retains the host panic policy.
namespace LuaBoundary {
class Error : public std::runtime_error {
public:
    explicit Error(std::string_view message);
};

template<class Result>
void Check(const Result &result, const char *context) {
    if (!result) throw Error(std::string(context) + ": " + result.message());
}

template<class T>
T Cast(const luabridge::LuaRef &value) {
    auto *state=value.state();
    if(!lua_checkstack(state,4))throw Error("Lua conversion could not reserve stack space");
    const int top=lua_gettop(state);
    try {
        auto result=value.template cast<T>();
        Check(result,"Lua value conversion");
        if constexpr(std::is_reference_v<T>)return result.value().get();
        else return std::move(*result);
    } catch(const luabridge::LuaException &failure) {
        lua_settop(state,top);
        throw Error(failure.what());
    }
}

// Component policy: only the actual Lua boolean false disables. A nil or
// other value retains the existing default-enabled behavior. No metamethod runs.
bool IsEnabled(const luabridge::LuaRef &value);

// Stack before: [unchanged base..., function, nargs arguments]. Success removes
// function/arguments and leaves nresults; error restores base and throws Error
// containing a bounded UTF-8 diagnostic and traceback where Lua provides one.
void ProtectedCall(lua_State *state,int nargs,int nresults);

template<class... Args>
void CallDiscard(const luabridge::LuaRef &function,Args&&... arguments) {
    auto *state=function.state();
    const int top=lua_gettop(state);
    struct Restore {lua_State *state;int top;~Restore(){lua_settop(state,top);}} restore{state,top};
    if(!lua_checkstack(state,static_cast<int>(sizeof...(Args))+4))
        throw Error("Lua callback could not reserve stack space");
    try {
        function.push(state);
        (Check(luabridge::Stack<std::decay_t<Args>>::push(state,std::forward<Args>(arguments)),"Lua callback argument"),...);
        ProtectedCall(state,static_cast<int>(sizeof...(Args)),0);
    } catch(const luabridge::LuaException &failure) {
        throw Error(failure.what());
    }
}
}
#endif
