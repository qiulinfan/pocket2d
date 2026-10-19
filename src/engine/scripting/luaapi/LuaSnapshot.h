#ifndef POCKET2D_LUA_SNAPSHOT_H
#define POCKET2D_LUA_SNAPSHOT_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

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
// Marks the table at `index` as this machine's own (settings, which side is
// local): rolled back like the rest, but a resync from the peer leaves it as it is.
void MarkLocal(lua_State *L, int index);
bool HasRoots();

void Save(int slot);
void Load(int slot);
// An order-independent hash of a saved slot's scalars (strings, numbers,
// booleans; tables and functions count by kind), equal for equal states.
std::uint32_t Hash(int slot);
// Saved tables and functions in a slot (diagnostics).
int Objects(int slot);

// Desync recovery: the live state in a portable form, for another process
// running the same scripts. Objects are named by the path they were first
// reached on (root, key, metatable, upvalue); Import matches each to the
// object on the same path when it is of the same kind (a closure: of the same
// function), creates tables and closures that have no match, and then writes
// every table's contents and closure's upvalues. Static and local tables are
// never written, only found by their path; so are C functions and userdata.
struct ImportStats {
    int objects = 0;    // tables and closures written
    int created = 0;    // of those, new (no object on the same path)
    int unresolved = 0; // static / local tables and opaque values not found by their path (read as nil)
};
// The state's functions travel as hashes of their code: Export also fills
// `dumps` (hash -> code) for the code an importer does not have (MissingCode).
using Dumps = std::unordered_map<std::uint64_t, std::string>;
std::string Export(Dumps *dumps = nullptr);
std::vector<std::uint64_t> MissingCode(const std::string &data);
bool Import(const std::string &data, const Dumps *dumps, std::string &error, ImportStats *stats = nullptr);

namespace detail { // for LuaSnapshotPortable.cpp
lua_State *Lua();
int PushRoots(); // pushes the roots array, returns their count
bool IsStatic(const void *p);
bool IsLocal(const void *p);
} // namespace detail

} // namespace LuaSnapshot

#endif
