#ifndef POCKET2D_LUA_SNAPSHOT_H
#define POCKET2D_LUA_SNAPSHOT_H

#include <cstdint>

struct lua_State;

// Rollback state for netplay: the Lua object graph reachable from the tracked
// roots, saved into numbered slots and restored in place. Every reachable
// table keeps its identity (closures and other tables still refer to it): its
// contents and metatable are saved and written back, and Lua closures' upvalues
// are saved and set again. Tables marked static are neither walked nor
// restored. Userdata, C functions, strings and numbers are kept as values.
namespace LuaSnapshot {

// Replaces the roots with the values at stack indices [first, last].
void Track(lua_State *L, int first, int last);
// Marks the table at `index` as never changing.
void MarkStatic(lua_State *L, int index);
bool HasRoots();

void Save(int slot);
void Load(int slot);
// An order-independent hash of a saved slot's scalars (strings, numbers,
// booleans; tables and functions count by kind), equal for equal states.
std::uint32_t Hash(int slot);
// Saved tables and functions in a slot (diagnostics).
int Objects(int slot);

} // namespace LuaSnapshot

#endif
