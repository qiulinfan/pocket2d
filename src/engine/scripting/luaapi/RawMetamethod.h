#ifndef POCKET2D_RAW_METAMETHOD_H
#define POCKET2D_RAW_METAMETHOD_H

#include "lua.hpp"
#include <cstdio>
#include <exception>
#include <cstdlib>
#include <new>

namespace APIRegistrationDetail {
// These four Actor/Vector2 metamethods are installed with lua_pushcfunction,
// outside LuaBridge's registered-call exception wrapper. Never call lua_error
// while an exception/string/result owning C++ resources still needs unwinding.
template <lua_CFunction Function>
int CheckedRawMetamethod(lua_State *state) {
    const int top = lua_gettop(state);
    char diagnostic[1024]{};
    try { return Function(state); }
    catch (const std::bad_alloc &) {
        // An exhausted native allocator is a fatal/reset boundary, not a
        // recoverable script input error. This path must not allocate.
        std::fputs("fatal: native allocation exhausted in Lua metamethod\n", stderr);
        std::abort();
    } catch (const std::exception &error) {
        std::snprintf(diagnostic, sizeof(diagnostic), "%s", error.what());
    } catch (...) {
        std::snprintf(diagnostic, sizeof(diagnostic), "%s", "unknown native metamethod failure");
    }
    lua_settop(state, top);
    lua_pushstring(state, diagnostic);
    return lua_error(state);
}
}

#endif
