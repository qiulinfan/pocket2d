#ifndef POCKET2D_LUA_JSON_H
#define POCKET2D_LUA_JSON_H

#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <string>

namespace LuaJson {
// A Lua value as JSON, read without metamethods: at most 4 levels, 32 entries
// per table and 256 bytes per string. Arrays are tables with a border > 0.
// Functions and unknown userdata become "<function>"/"<userdata>", deeper
// tables "<table>", Vector2 {x,y}, actors {actor_uid}, non-finite numbers null.
std::string Bounded(const luabridge::LuaRef &value);
} // namespace LuaJson

#endif
