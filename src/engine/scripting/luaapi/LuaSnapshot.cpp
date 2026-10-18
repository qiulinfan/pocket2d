#include "LuaSnapshot.h"

#include "RegistrationDetail.h"
#include "core/World.h"
#include "lua.hpp"

#include <cstring>
#include <unordered_set>
#include <vector>

// A slot is one flat Lua array in the registry, reused between saves:
//   table record:    T, metatable or false, n, k1, v1, ... kn, vn
//   function record: F, n, upvalue1 ... upvaluen
// Records are told apart by the type of their first value.
namespace LuaSnapshot {
namespace {

struct Slot {
    int ref = LUA_NOREF;
    int length = 0;   // used entries
    int objects = 0;
};

struct State {
    lua_State *L = nullptr;
    int roots_ref = LUA_NOREF;
    int roots = 0;
    int static_ref = LUA_NOREF; // keeps static tables alive (their addresses stay theirs)
    std::unordered_set<const void *> statics;
    std::vector<Slot> slots;
    std::unordered_set<const void *> visited;
    int work_ref = LUA_NOREF;
};

State &Current() {
    State &s = World::Current().State<State>();
    lua_State *L = APIRegistrationDetail::LuaState();
    if (s.L != L) s = State{}, s.L = L; // a new Lua state starts afresh
    return s;
}

int RegistryTable(lua_State *L, int &ref) {
    if (ref == LUA_NOREF) {
        lua_newtable(L);
        ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    return lua_gettop(L);
}

bool Walkable(lua_State *L, int index) {
    const int t = lua_type(L, index);
    return t == LUA_TTABLE || (t == LUA_TFUNCTION && !lua_iscfunction(L, index));
}

std::uint32_t Mix(std::uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352dU;
    h ^= h >> 15;
    h *= 0x846ca68bU;
    h ^= h >> 16;
    return h;
}

std::uint32_t ValueHash(lua_State *L, int index) {
    switch (lua_type(L, index)) {
    case LUA_TNIL: return 0x1u;
    case LUA_TBOOLEAN: return lua_toboolean(L, index) ? 0x2u : 0x3u;
    case LUA_TNUMBER: {
        if (lua_isinteger(L, index)) {
            const auto v = static_cast<std::uint64_t>(lua_tointeger(L, index));
            return Mix(static_cast<std::uint32_t>(v) ^ Mix(static_cast<std::uint32_t>(v >> 32) ^ 0x51u));
        }
        double d = lua_tonumber(L, index);
        if (d == 0.0) d = 0.0; // -0 == 0
        std::uint64_t bits;
        std::memcpy(&bits, &d, sizeof bits);
        return Mix(static_cast<std::uint32_t>(bits) ^ Mix(static_cast<std::uint32_t>(bits >> 32) ^ 0x52u));
    }
    case LUA_TSTRING: {
        std::size_t n = 0;
        const char *p = lua_tolstring(L, index, &n);
        std::uint32_t h = 2166136261u;
        for (std::size_t i = 0; i < n; ++i) h = (h ^ static_cast<unsigned char>(p[i])) * 16777619u;
        return Mix(h ^ 0x53u);
    }
    case LUA_TTABLE: return 0x54u;
    case LUA_TFUNCTION: return 0x55u;
    default: return 0x56u;
    }
}

} // namespace

void Track(lua_State *L, int first, int last) {
    State &s = Current();
    if (s.roots_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, s.roots_ref);
    lua_createtable(L, last - first + 1, 0);
    int n = 0;
    for (int i = first; i <= last; ++i) {
        if (!Walkable(L, i)) continue;
        lua_pushvalue(L, i);
        lua_rawseti(L, -2, ++n);
    }
    s.roots = n;
    s.roots_ref = luaL_ref(L, LUA_REGISTRYINDEX);
}

void MarkStatic(lua_State *L, int index) {
    if (lua_type(L, index) != LUA_TTABLE) return;
    State &s = Current();
    index = lua_absindex(L, index);
    const int keep = RegistryTable(L, s.static_ref);
    lua_pushvalue(L, index);
    lua_pushboolean(L, 1);
    lua_rawset(L, keep);
    lua_pop(L, 1);
    s.statics.insert(lua_topointer(L, index));
}

bool HasRoots() { return Current().roots > 0; }

void Save(int slot) {
    State &s = Current();
    lua_State *L = s.L;
    if (slot < 0) return;
    if (static_cast<int>(s.slots.size()) <= slot) s.slots.resize(slot + 1);
    Slot &out = s.slots[slot];
    lua_checkstack(L, 16);
    const int top = lua_gettop(L);
    const int S = RegistryTable(L, out.ref);
    const int W = RegistryTable(L, s.work_ref);
    int w = 0;   // work stack size
    int i = 0;   // slot entries
    int objects = 0;
    s.visited.clear();
    auto queue = [&](int index) {
        if (!Walkable(L, index)) return;
        const void *p = lua_topointer(L, index);
        if (s.statics.count(p) || !s.visited.insert(p).second) return;
        lua_pushvalue(L, index);
        lua_rawseti(L, W, ++w);
    };
    auto put = [&](int index) {
        lua_pushvalue(L, index);
        lua_rawseti(L, S, ++i);
    };
    if (s.roots_ref != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, s.roots_ref);
        for (int r = 1; r <= s.roots; ++r) {
            lua_rawgeti(L, -1, r);
            queue(-1);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    while (w > 0) {
        lua_rawgeti(L, W, w);
        lua_pushnil(L);
        lua_rawseti(L, W, w--);
        const int obj = lua_gettop(L);
        ++objects;
        put(obj);
        if (lua_type(L, obj) == LUA_TTABLE) {
            if (lua_getmetatable(L, obj)) {
                queue(-1);
                put(-1);
                lua_pop(L, 1);
            } else {
                lua_pushboolean(L, 0);
                lua_rawseti(L, S, ++i);
            }
            const int count_at = ++i;
            lua_Integer pairs = 0;
            lua_pushnil(L);
            while (lua_next(L, obj)) {
                queue(-2);
                queue(-1);
                put(-2);
                put(-1);
                ++pairs;
                lua_pop(L, 1);
            }
            lua_pushinteger(L, pairs);
            lua_rawseti(L, S, count_at);
        } else {
            const int count_at = ++i;
            lua_Integer n = 0;
            while (lua_getupvalue(L, obj, static_cast<int>(n) + 1)) {
                queue(-1);
                put(-1);
                lua_pop(L, 1);
                ++n;
            }
            lua_pushinteger(L, n);
            lua_rawseti(L, S, count_at);
        }
        lua_pop(L, 1);
    }
    // Release what the previous save of this slot referred to beyond the new end.
    for (int k = i + 1; k <= out.length; ++k) {
        lua_pushnil(L);
        lua_rawseti(L, S, k);
    }
    out.length = i;
    out.objects = objects;
    lua_settop(L, top);
}

void Load(int slot) {
    State &s = Current();
    lua_State *L = s.L;
    if (slot < 0 || slot >= static_cast<int>(s.slots.size()) || s.slots[slot].ref == LUA_NOREF) return;
    const Slot &in = s.slots[slot];
    lua_checkstack(L, 16);
    const int top = lua_gettop(L);
    const int S = RegistryTable(L, s.slots[slot].ref);
    for (int i = 1; i <= in.length;) {
        lua_rawgeti(L, S, i++);
        const int obj = lua_gettop(L);
        if (lua_type(L, obj) == LUA_TTABLE) {
            // Clear, refill, and restore the metatable.
            lua_newtable(L);
            const int keys = lua_gettop(L);
            int nkeys = 0;
            lua_pushnil(L);
            while (lua_next(L, obj)) {
                lua_pop(L, 1);
                lua_pushvalue(L, -1);
                lua_rawseti(L, keys, ++nkeys);
            }
            for (int k = 1; k <= nkeys; ++k) {
                lua_rawgeti(L, keys, k);
                lua_pushnil(L);
                lua_rawset(L, obj);
            }
            lua_pop(L, 1);
            lua_rawgeti(L, S, i++);
            if (lua_istable(L, -1)) lua_setmetatable(L, obj);
            else { lua_pop(L, 1); lua_pushnil(L); lua_setmetatable(L, obj); }
            lua_rawgeti(L, S, i++);
            const lua_Integer pairs = lua_tointeger(L, -1);
            lua_pop(L, 1);
            for (lua_Integer p = 0; p < pairs; ++p) {
                lua_rawgeti(L, S, i++);
                lua_rawgeti(L, S, i++);
                lua_rawset(L, obj);
            }
        } else {
            lua_rawgeti(L, S, i++);
            const lua_Integer n = lua_tointeger(L, -1);
            lua_pop(L, 1);
            for (lua_Integer u = 1; u <= n; ++u) {
                lua_rawgeti(L, S, i++);
                if (!lua_setupvalue(L, obj, static_cast<int>(u))) lua_pop(L, 1);
            }
        }
        lua_settop(L, obj - 1);
    }
    lua_settop(L, top);
}

std::uint32_t Hash(int slot) {
    State &s = Current();
    lua_State *L = s.L;
    if (slot < 0 || slot >= static_cast<int>(s.slots.size()) || s.slots[slot].ref == LUA_NOREF) return 0;
    const Slot &in = s.slots[slot];
    lua_checkstack(L, 16);
    const int top = lua_gettop(L);
    const int S = RegistryTable(L, s.slots[slot].ref);
    std::uint32_t total = 0;
    for (int i = 1; i <= in.length;) {
        lua_rawgeti(L, S, i++);
        std::uint32_t record = 0;
        if (lua_type(L, -1) == LUA_TTABLE) {
            lua_rawgeti(L, S, i++);
            record = lua_istable(L, -1) ? 0x61u : 0x62u;
            lua_pop(L, 1);
            lua_rawgeti(L, S, i++);
            const lua_Integer pairs = lua_tointeger(L, -1);
            lua_pop(L, 1);
            std::uint32_t sum = 0;
            for (lua_Integer p = 0; p < pairs; ++p) {
                lua_rawgeti(L, S, i++);
                lua_rawgeti(L, S, i++);
                sum += Mix(ValueHash(L, -2) * 31u + ValueHash(L, -1));
                lua_pop(L, 2);
            }
            record = Mix(record ^ sum);
        } else {
            lua_rawgeti(L, S, i++);
            const lua_Integer n = lua_tointeger(L, -1);
            lua_pop(L, 1);
            record = 0x63u;
            for (lua_Integer u = 1; u <= n; ++u) {
                lua_rawgeti(L, S, i++);
                record = Mix(record * 31u + ValueHash(L, -1) + static_cast<std::uint32_t>(u));
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        total += record;
    }
    lua_settop(L, top);
    return Mix(total);
}

int Objects(int slot) {
    State &s = Current();
    return slot >= 0 && slot < static_cast<int>(s.slots.size()) ? s.slots[slot].objects : 0;
}

} // namespace LuaSnapshot
