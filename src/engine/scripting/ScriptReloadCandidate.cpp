#include "scripting/ScriptReloadCandidate.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace {
constexpr std::size_t kHeap = 32 * 1024 * 1024;
constexpr std::size_t kGraphBytes = 8 * 1024 * 1024;
constexpr std::size_t kGraphNodes = 100000;
constexpr unsigned kGraphDepth = 64;
constexpr std::size_t kDiagnosticBytes = 4096;
struct Context {
    std::size_t used = 0;
    unsigned remaining = 1000000;
    bool heap_failed = false;
    bool instruction_failed = false;
    int proxy_map = LUA_NOREF, denied = LUA_NOREF, allowed = LUA_NOREF, backing = LUA_NOREF, environment = LUA_NOREF;
    lua_CFunction original_rawget = nullptr, original_rawset = nullptr;
    lua_CFunction original_next = nullptr, original_setmetatable = nullptr;
};
Context *Current(lua_State *state) { return *static_cast<Context **>(lua_getextraspace(state)); }
void *Allocate(void *userdata, void *pointer, std::size_t old_size, std::size_t size) {
    auto &context = *static_cast<Context *>(userdata);
    if (!pointer) old_size = 0;
    if (!size) { std::free(pointer); context.used -= old_size; return nullptr; }
    if (size > old_size && size - old_size > kHeap - context.used) {
        context.heap_failed = true;
        return nullptr;
    }
    void *result = std::realloc(pointer, size);
    if (result) context.used = context.used - old_size + size;
    return result;
}
void Hook(lua_State *state, lua_Debug *) {
    auto &context = *Current(state);
    if (context.remaining) --context.remaining;
    else context.instruction_failed = true;
    if (context.instruction_failed || context.heap_failed)
        luaL_error(state, "%s", context.heap_failed ? "candidate heap limit exceeded" : "candidate instruction limit exceeded");
}
// All callbacks below use only trivial C++ locals across Lua error/longjmp.
bool PushBacking(lua_State *state, int index) {
    index = lua_absindex(state, index);
    lua_rawgeti(state, LUA_REGISTRYINDEX, Current(state)->proxy_map);
    lua_pushvalue(state, index); lua_rawget(state, -2); lua_remove(state, -2);
    if (lua_isnil(state, -1)) { lua_pop(state, 1); return false; }
    return true;
}
void CheckAvailable(lua_State *state, int object, int key) {
    object = lua_absindex(state, object); key = lua_absindex(state, key);
    lua_rawgeti(state, LUA_REGISTRYINDEX, Current(state)->denied);
    lua_pushvalue(state, object); lua_rawget(state, -2);
    bool unavailable = false;
    if (lua_istable(state, -1)) {
        lua_pushvalue(state, key); lua_rawget(state, -2);
        unavailable = lua_toboolean(state, -1); lua_pop(state, 1);
    }
    lua_pop(state, 2);
    if (unavailable) luaL_error(state, "candidate API '%s' is unavailable during staging", lua_tostring(state, key));
}
void CheckGlobalNil(lua_State *state, int object, int key) {
    if (!lua_isnil(state, -1)) return;
    object = lua_absindex(state, object); key = lua_absindex(state, key);
    lua_rawgeti(state, LUA_REGISTRYINDEX, Current(state)->environment);
    const bool environment = lua_rawequal(state, object, -1); lua_pop(state, 1);
    if (!environment) return;
    // TSTL bundles capture a missing global require before installing their own
    // local module loader. Preserve that known nil; no native require is exposed.
    std::size_t key_length = 0;
    const char *key_text = lua_type(state, key) == LUA_TSTRING ? lua_tolstring(state, key, &key_length) : nullptr;
    if (key_text && key_length == 7 && std::memcmp(key_text, "require", 7) == 0) return;
    lua_rawgeti(state, LUA_REGISTRYINDEX, Current(state)->allowed);
    lua_pushvalue(state, key); lua_rawget(state, -2);
    const bool selected = lua_toboolean(state, -1); lua_pop(state, 2);
    if (!selected) luaL_error(state, "unknown candidate global is unavailable during staging; select its declaration dependency");
}
int ProxyIndex(lua_State *state) {
    lua_settop(state, 2);
    CheckAvailable(state, 1, 2);
    if (!PushBacking(state, 1)) return luaL_error(state, "invalid candidate environment");
    lua_pushvalue(state, 2); lua_rawget(state, -2); CheckGlobalNil(state, 1, 2); return 1;
}
int ProxyWrite(lua_State *state) {
    lua_settop(state, 3);
    lua_rawgeti(state, LUA_REGISTRYINDEX, Current(state)->environment);
    const bool environment = lua_rawequal(state, 1, -1); lua_pop(state, 1);
    if (!environment) return luaL_error(state, "candidate standard libraries are readonly");
    lua_rawgeti(state, LUA_REGISTRYINDEX, Current(state)->allowed);
    lua_pushvalue(state, 2); lua_rawget(state, -2);
    const bool allowed = lua_toboolean(state, -1); lua_pop(state, 2);
    if (!allowed) return luaL_error(state, "unexpected candidate global export or standard-library replacement");
    lua_rawgeti(state, LUA_REGISTRYINDEX, Current(state)->backing);
    lua_pushvalue(state, 2); lua_pushvalue(state, 3); lua_rawset(state, -3);
    return 0;
}
int SafeRawGet(lua_State *state) {
    luaL_checktype(state, 1, LUA_TTABLE); luaL_checkany(state, 2); lua_settop(state, 2);
    CheckAvailable(state, 1, 2);
    if (PushBacking(state, 1)) { lua_pushvalue(state, 2); lua_rawget(state, -2); CheckGlobalNil(state, 1, 2); }
    else { lua_pushvalue(state, 2); lua_rawget(state, 1); }
    return 1;
}
int SafeRawSet(lua_State *state) {
    luaL_checktype(state, 1, LUA_TTABLE); luaL_checkany(state, 2); luaL_checkany(state, 3); lua_settop(state, 3);
    if (PushBacking(state, 1)) { lua_pop(state, 1); ProxyWrite(state); lua_pushvalue(state, 1); }
    else { lua_pushvalue(state, 2); lua_pushvalue(state, 3); lua_rawset(state, 1); lua_pushvalue(state, 1); }
    return 1;
}
int SafeNext(lua_State *state) {
    luaL_checktype(state, 1, LUA_TTABLE); lua_settop(state, 2);
    if (PushBacking(state, 1)) { lua_replace(state, 1); }
    if (lua_next(state, 1)) return 2;
    lua_pushnil(state); return 1;
}
int ProxyPairs(lua_State *state) {
    lua_pushcfunction(state, SafeNext); lua_pushvalue(state, 1); lua_pushnil(state); return 3;
}
int SafeSetMetatable(lua_State *state) {
    luaL_checktype(state, 1, LUA_TTABLE);
    if (!lua_isnil(state, 2)) luaL_checktype(state, 2, LUA_TTABLE);
    lua_settop(state, 2);
    if (PushBacking(state, 1)) return luaL_error(state, "candidate standard-library metatables are readonly");
    if (lua_getmetatable(state, 1)) {
        lua_pushliteral(state, "__metatable"); lua_rawget(state, -2);
        if (!lua_isnil(state, -1)) return luaL_error(state, "cannot change a protected metatable");
        lua_pop(state, 2);
    }
    if (!lua_isnil(state, 2)) {
        for (const char *field : {"__gc", "__close", "__mode"}) {
            lua_pushstring(state, field); lua_rawget(state, 2);
            if (!lua_isnil(state, -1)) return luaL_error(state, "candidate finalizer/weak metatables are not supported");
            lua_pop(state, 1);
        }
    }
    lua_pushvalue(state, 2); lua_setmetatable(state, 1); lua_pushvalue(state, 1); return 1;
}
void MakeProxy(lua_State *state, int backing) {
    backing = lua_absindex(state, backing);
    lua_newtable(state);
    const int proxy = lua_gettop(state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, Current(state)->proxy_map);
    lua_pushvalue(state, proxy); lua_pushvalue(state, backing); lua_rawset(state, -3); lua_pop(state, 1);
    lua_newtable(state);
    lua_pushcfunction(state, ProxyIndex); lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, ProxyWrite); lua_setfield(state, -2, "__newindex");
    lua_pushcfunction(state, ProxyPairs); lua_setfield(state, -2, "__pairs");
    lua_pushliteral(state, "isolated candidate library"); lua_setfield(state, -2, "__metatable");
    lua_setmetatable(state, proxy);
}
void BlockFields(lua_State *state, int object, std::initializer_list<const char *> names) {
    object = lua_absindex(state, object);
    lua_rawgeti(state, LUA_REGISTRYINDEX, Current(state)->denied);
    lua_pushvalue(state, object); lua_newtable(state);
    for (const char *name : names) { lua_pushboolean(state, 1); lua_setfield(state, -2, name); }
    lua_rawset(state, -3); lua_pop(state, 1);
}
int Initialize(lua_State *state) {
    auto *context = Current(state);
    luaL_requiref(state, "_G", luaopen_base, 1); lua_pop(state, 1);
    luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(state, 1);
    luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(state, 1);
    luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(state, 1);
    luaL_requiref(state, LUA_UTF8LIBNAME, luaopen_utf8, 1); lua_pop(state, 1);
    lua_getglobal(state, "rawget"); context->original_rawget = lua_tocfunction(state, -1); lua_pop(state, 1);
    lua_getglobal(state, "rawset"); context->original_rawset = lua_tocfunction(state, -1); lua_pop(state, 1);
    lua_getglobal(state, "next"); context->original_next = lua_tocfunction(state, -1); lua_pop(state, 1);
    lua_getglobal(state, "setmetatable"); context->original_setmetatable = lua_tocfunction(state, -1); lua_pop(state, 1);
    for (const char *name : {"dofile", "loadfile", "load", "print", "warn", "collectgarbage", "pcall", "xpcall"}) {
        lua_pushnil(state); lua_setglobal(state, name);
    }
    lua_getglobal(state, "math");
    lua_pushnil(state); lua_setfield(state, -2, "random");
    lua_pushnil(state); lua_setfield(state, -2, "randomseed"); lua_pop(state, 1);
    // Lua's native pattern backtracking does not dispatch bytecode hooks.
    // Remove it from the original string table too, so method syntax cannot
    // recover it through the string metatable's __index backing.
    lua_getglobal(state, "string");
    for (const char *name : {"find", "match", "gmatch", "gsub"}) {
        lua_pushnil(state); lua_setfield(state, -2, name);
    }
    lua_pop(state, 1);
    lua_pushcfunction(state, SafeRawGet); lua_setglobal(state, "rawget");
    lua_pushcfunction(state, SafeRawSet); lua_setglobal(state, "rawset");
    lua_pushcfunction(state, SafeNext); lua_setglobal(state, "next");
    lua_pushcfunction(state, SafeSetMetatable); lua_setglobal(state, "setmetatable");
    // Hide string's original library backing from getmetatable("").
    lua_pushliteral(state, "");
    if (lua_getmetatable(state, -1)) {
        lua_pushliteral(state, "isolated string metatable"); lua_setfield(state, -2, "__metatable"); lua_pop(state, 1);
    }
    lua_pop(state, 1);
    lua_newtable(state); context->proxy_map = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_newtable(state); context->denied = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_newtable(state); context->allowed = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_newtable(state); context->backing = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_pushglobaltable(state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, context->backing);
    lua_pushnil(state);
    while (lua_next(state, -3)) {
        if (lua_type(state, -2) == LUA_TSTRING && std::strcmp(lua_tostring(state, -2), "_G") != 0) {
            lua_pushvalue(state, -2); lua_pushvalue(state, -2); lua_rawset(state, -5);
        }
        lua_pop(state, 1);
    }
    // Replace exposed library tables with readonly proxies.
    for (const char *name : {"table", "string", "math", "utf8"}) {
        lua_getfield(state, -1, name); MakeProxy(state, -1);
        if (std::strcmp(name, "math") == 0) BlockFields(state, -1, {"random", "randomseed"});
        if (std::strcmp(name, "string") == 0) {
            BlockFields(state, -1, {"find", "match", "gmatch", "gsub"});
            // String method lookup must use the same unavailable-read checks,
            // not silently cache nil from the original backing table.
            lua_pushliteral(state, ""); lua_getmetatable(state, -1);
            lua_pushvalue(state, -3); lua_setfield(state, -2, "__index"); lua_pop(state, 2);
        }
        lua_setfield(state, -3, name); lua_pop(state, 1);
    }
    MakeProxy(state, -1);
    BlockFields(state, -1, {"io", "os", "package", "debug", "load", "loadfile", "dofile", "print", "warn", "collectgarbage", "coroutine", "pcall", "xpcall",
        "Actor", "Vector2", "Transform", "Rigidbody", "SpriteRenderer", "SpriteAnimation", "Tilemap", "ParticleSystem", "Input", "Save", "Inventory", "Debug", "Physics", "Event", "Audio", "Image", "Text", "Camera", "Application", "Scene", "Collision", "HitResult", "ItemCatalog", "InventoryContainer"});
    lua_pushvalue(state, -1); context->environment = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_setfield(state, -2, "_G");
    lua_settop(state, 0);
    return 0;
}
std::string DiagnosticText(std::string_view input) {
    constexpr std::string_view marker = "...[truncated]";
    constexpr char hex[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(std::min(input.size(), kDiagnosticBytes));
    std::vector<std::size_t> boundaries;
    std::size_t cursor = 0;
    while (cursor < input.size()) {
        const auto byte = static_cast<unsigned char>(input[cursor]);
        std::size_t length = byte < 0x80 ? 1 : byte >= 0xc2 && byte <= 0xdf ? 2 :
            byte >= 0xe0 && byte <= 0xef ? 3 : byte >= 0xf0 && byte <= 0xf4 ? 4 : 0;
        bool valid = length != 0 && length <= input.size() - cursor;
        for (std::size_t i = 1; valid && i < length; ++i)
            valid = (static_cast<unsigned char>(input[cursor + i]) & 0xc0) == 0x80;
        if (valid && length > 1) {
            const auto second = static_cast<unsigned char>(input[cursor + 1]);
            if ((byte == 0xe0 && second < 0xa0) || (byte == 0xed && second >= 0xa0) ||
                (byte == 0xf0 && second < 0x90) || (byte == 0xf4 && second > 0x8f)) valid = false;
        }
        const bool escape = !valid || (byte < 0x20 && byte != '\n' && byte != '\r' && byte != '\t') || byte == 0x7f;
        const std::size_t encoded_size = escape ? 4 : length;
        if (encoded_size > kDiagnosticBytes - output.size()) break;
        boundaries.push_back(output.size());
        if (escape) {
            output += "\\x"; output += hex[byte >> 4]; output += hex[byte & 15]; ++cursor;
        } else { output.append(input.data() + cursor, length); cursor += length; }
    }
    if (cursor < input.size()) {
        while (output.size() > kDiagnosticBytes - marker.size()) { output.resize(boundaries.back()); boundaries.pop_back(); }
        output += marker;
    }
    return output;
}
std::string Error(lua_State *state) {
    if (lua_type(state, -1) != LUA_TSTRING) return "candidate raised a non-string error";
    std::size_t length = 0; const char *text = lua_tolstring(state, -1, &length);
    return DiagnosticText({text, length});
}
int ErrorLine(const std::string &message) {
    for (std::size_t i = 0; i + 2 < message.size(); ++i) {
        if (message[i] != ':' || message[i + 1] < '0' || message[i + 1] > '9') continue;
        int line = 0; auto cursor = i + 1;
        while (cursor < message.size() && message[cursor] >= '0' && message[cursor] <= '9') {
            if (line > 10000000) break;
            line = line * 10 + message[cursor++] - '0';
        }
        if (cursor < message.size() && message[cursor] == ':') return line;
    }
    return 0;
}
bool Name(const std::string &value) {
    if (value.empty() || value.size() > 128) return false;
    const auto letter = [](unsigned char c) { return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    if (!letter(value.front())) return false;
    for (unsigned char c : value) if (!letter(c) && (c < '0' || c > '9')) return false;
    return true;
}
struct Path { std::string library, name; lua_CFunction expected = nullptr; };
void PushLive(lua_State *state, const Path &path) {
    lua_pushglobaltable(state);
    if (!path.library.empty()) {
        lua_pushlstring(state, path.library.data(), path.library.size()); lua_rawget(state, -2); lua_remove(state, -2);
        if (!lua_istable(state, -1)) { lua_pop(state, 1); throw std::runtime_error("live standard library is unavailable: " + path.library); }
    }
    lua_pushlstring(state, path.name.data(), path.name.size()); lua_rawget(state, -2); lua_remove(state, -2);
}
struct Transfer {
    lua_State *stage, *live;
    Context &context;
    std::unordered_map<const void *, Path> cfunctions;
    std::unordered_map<const void *, std::string> libraries;
    std::unordered_set<const void *> visited;
    std::unordered_map<const void *, std::string> bytecode;
    std::unordered_map<const void *, luabridge::LuaRef> copied;
    struct Cell { luabridge::LuaRef function; int index; };
    std::unordered_map<const void *, Cell> cells;
    const void *environment = nullptr;
    std::size_t nodes = 0, bytes = 0;

    void Count(std::size_t size = 0) {
        if (++nodes > kGraphNodes || size > kGraphBytes - bytes) throw std::runtime_error("candidate graph exceeds 100000 nodes or 8 MiB");
        bytes += size;
    }
    void CaptureFunctions(int table, const std::string &library) {
        table = lua_absindex(stage, table);
        lua_pushnil(stage);
        while (lua_next(stage, table)) {
            if (lua_type(stage, -2) == LUA_TSTRING && lua_iscfunction(stage, -1)) {
                auto function = lua_tocfunction(stage, -1);
                auto expected = function;
                if (function == SafeRawGet) expected = context.original_rawget;
                if (function == SafeRawSet) expected = context.original_rawset;
                if (function == SafeNext) expected = context.original_next;
                if (function == SafeSetMetatable) expected = context.original_setmetatable;
                cfunctions.emplace(lua_topointer(stage, -1), Path{library, lua_tostring(stage, -2), expected});
            }
            lua_pop(stage, 1);
        }
    }
    Transfer(lua_State *candidate, lua_State *target, Context &budget) : stage(candidate), live(target), context(budget) {
        lua_rawgeti(stage, LUA_REGISTRYINDEX, context.environment); environment = lua_topointer(stage, -1); lua_pop(stage, 1);
        lua_rawgeti(stage, LUA_REGISTRYINDEX, context.backing); CaptureFunctions(-1, "");
        for (const char *name : {"table", "string", "math", "utf8"}) {
            lua_getfield(stage, -1, name); libraries.emplace(lua_topointer(stage, -1), name);
            PushBacking(stage, -1); CaptureFunctions(-1, name); lua_pop(stage, 2);
        }
        lua_pop(stage, 1);
        // pairs(plain_table) can return the original base next function.
        lua_pushcfunction(stage, context.original_next);
        cfunctions.emplace(lua_topointer(stage, -1), Path{"", "next", context.original_next}); lua_pop(stage, 1);
    }
    struct DumpBuffer { std::string data; bool failed = false; };
    static int Dump(lua_State *, const void *data, std::size_t size, void *userdata) noexcept {
        auto &buffer = *static_cast<DumpBuffer *>(userdata);
        try {
            if (size > kGraphBytes - buffer.data.size()) { buffer.failed = true; return 1; }
            buffer.data.append(static_cast<const char *>(data), size); return 0;
        } catch (...) { buffer.failed = true; return 1; }
    }
    void Validate(int index, unsigned depth = 1) {
        if (depth > kGraphDepth) throw std::runtime_error("candidate graph exceeds depth 64");
        index = lua_absindex(stage, index);
        Count();
        const int type = lua_type(stage, index);
        if (type == LUA_TNIL || type == LUA_TBOOLEAN) return;
        if (type == LUA_TNUMBER) {
            if (!lua_isinteger(stage, index) && !std::isfinite(lua_tonumber(stage, index))) throw std::runtime_error("candidate graph contains a non-finite number");
            return;
        }
        if (type == LUA_TSTRING) { std::size_t length = 0; lua_tolstring(stage, index, &length); Count(length); return; }
        if (type != LUA_TTABLE && type != LUA_TFUNCTION) throw std::runtime_error("candidate graph contains userdata/thread/unsupported value");
        const void *identity = lua_topointer(stage, index);
        if (identity == environment || libraries.contains(identity)) return;
        if (!visited.insert(identity).second) return;
        if (type == LUA_TFUNCTION) {
            if (lua_iscfunction(stage, index)) {
                if (!cfunctions.contains(identity)) throw std::runtime_error("candidate captures an unknown C closure");
                return;
            }
            lua_pushvalue(stage, index); DumpBuffer buffer;
            const int result = lua_dump(stage, Dump, &buffer, 0); lua_pop(stage, 1);
            if (result || buffer.failed) throw std::runtime_error("candidate function bytecode exceeds transfer limits");
            Count(buffer.data.size()); bytecode.emplace(identity, std::move(buffer.data));
            for (int upvalue = 1; lua_getupvalue(stage, index, upvalue); ++upvalue) { Validate(-1, depth + 1); lua_pop(stage, 1); }
            return;
        }
        if (lua_getmetatable(stage, index)) {
            for (const char *field : {"__gc", "__close", "__mode"}) {
                lua_pushstring(stage, field); lua_rawget(stage, -2);
                const bool forbidden = !lua_isnil(stage, -1); lua_pop(stage, 1);
                if (forbidden) throw std::runtime_error("candidate graph has finalizer/weak metatable");
            }
            lua_pushliteral(stage, "__index"); lua_rawget(stage, -2);
            const bool function_index = lua_isfunction(stage, -1); lua_pop(stage, 1);
            if (function_index) throw std::runtime_error("function __index prototypes are not supported during reload");
            Validate(-1, depth + 1); lua_pop(stage, 1);
        }
        lua_pushnil(stage);
        while (lua_next(stage, index)) { Validate(-2, depth + 1); Validate(-1, depth + 1); lua_pop(stage, 1); }
    }
    void Copy(int index) {
        index = lua_absindex(stage, index);
        switch (lua_type(stage, index)) {
        case LUA_TNIL: lua_pushnil(live); return;
        case LUA_TBOOLEAN: lua_pushboolean(live, lua_toboolean(stage, index)); return;
        case LUA_TNUMBER:
            if (lua_isinteger(stage, index)) lua_pushinteger(live, lua_tointeger(stage, index));
            else lua_pushnumber(live, lua_tonumber(stage, index));
            return;
        case LUA_TSTRING: {
            std::size_t length = 0; const char *value = lua_tolstring(stage, index, &length);
            lua_pushlstring(live, value, length); return;
        }
        default: break;
        }
        const void *identity = lua_topointer(stage, index);
        if (identity == environment) { lua_pushglobaltable(live); return; }
        if (const auto found = copied.find(identity); found != copied.end()) { found->second.push(live); return; }
        if (const auto library = libraries.find(identity); library != libraries.end()) {
            // Capture the actual live namespace only if each whitelisted C
            // function still has the standard implementation, never a Lua getter.
            for (const auto &[unused, path] : cfunctions) if (path.library == library->second) {
                PushLive(live, path);
                const bool same = lua_iscfunction(live, -1) && lua_tocfunction(live, -1) == path.expected;
                lua_pop(live, 1);
                if (!same) throw std::runtime_error("live standard-library function identity changed");
            }
            lua_pushglobaltable(live); lua_pushlstring(live, library->second.data(), library->second.size()); lua_rawget(live, -2); lua_remove(live, -2);
            return;
        }
        if (lua_iscfunction(stage, index)) {
            const auto &path = cfunctions.at(identity); PushLive(live, path);
            if (!lua_iscfunction(live, -1) || lua_tocfunction(live, -1) != path.expected)
                throw std::runtime_error("captured C function does not match live standard-library identity");
            return;
        }
        if (lua_isfunction(stage, index)) {
            const auto &code = bytecode.at(identity);
            if (luaL_loadbufferx(live, code.data(), code.size(), "@reload-transfer", "b") != LUA_OK)
                throw std::runtime_error("cannot load transferred Lua function: " + Error(live));
            const auto function = luabridge::LuaRef::fromStack(live, -1);
            copied.emplace(identity, function);
            for (int upvalue = 1; lua_getupvalue(stage, index, upvalue); ++upvalue) {
                const void *cell = lua_upvalueid(stage, index, upvalue);
                if (const auto prior = cells.find(cell); prior != cells.end()) {
                    function.push(live); prior->second.function.push(live);
                    lua_upvaluejoin(live, -2, upvalue, -1, prior->second.index); lua_pop(live, 2);
                } else {
                    cells.emplace(cell, Cell{function, upvalue});
                    function.push(live); Copy(-1);
                    if (!lua_setupvalue(live, -2, upvalue)) throw std::runtime_error("transferred function upvalue mismatch");
                    lua_pop(live, 1);
                }
                lua_pop(stage, 1);
            }
            return;
        }
        lua_newtable(live);
        const auto table = luabridge::LuaRef::fromStack(live, -1); copied.emplace(identity, table);
        lua_pushnil(stage);
        while (lua_next(stage, index)) {
            table.push(live); Copy(-2); Copy(-1); lua_rawset(live, -3); lua_pop(live, 1); lua_pop(stage, 1);
        }
        if (lua_getmetatable(stage, index)) { table.push(live); Copy(-1); lua_setmetatable(live, -2); lua_pop(live, 1); lua_pop(stage, 1); }
    }
};
struct StackRestore { lua_State *state; int top; ~StackRestore() { lua_settop(state, top); } };
struct GCGuard {
    lua_State *state; bool running;
    explicit GCGuard(lua_State *value) : state(value), running(lua_gc(value, LUA_GCISRUNNING) != 0) { lua_gc(state, LUA_GCSTOP); }
    ~GCGuard() { if (running) lua_gc(state, LUA_GCRESTART); }
};
} // namespace

namespace ScriptReloadCandidate {
std::string SanitizeDiagnosticText(std::string_view text) { return DiagnosticText(text); }

Batch Stage(lua_State *live, const std::vector<Source> &sources) {
    Batch batch;
    const Source *current = sources.empty() ? nullptr : &sources.front();
    std::string phase = "validate";
    const auto fail = [&](const std::string &message) {
        batch.exports.clear(); batch.ok = false;
        batch.diagnostics.push_back({DiagnosticText(current ? current->type : ""), DiagnosticText(current ? current->resource : ""),
            phase, DiagnosticText(message), ErrorLine(message)});
    };
    if (!live) { fail("live Lua state is required"); return batch; }
    StackRestore restore{live, lua_gettop(live)};
    Context context;
    std::unique_ptr<lua_State, decltype(&lua_close)> candidate(lua_newstate(Allocate, &context), lua_close);
    if (!candidate) { fail("cannot create bounded candidate Lua state"); return batch; }
    auto *state = candidate.get(); *static_cast<Context **>(lua_getextraspace(state)) = &context;
    try {
        if (sources.empty() || sources.size() > 64) throw std::runtime_error("reload batch requires 1..64 sources");
        lua_pushcfunction(state, Initialize);
        if (lua_pcall(state, 0, 0, 0) != LUA_OK) throw std::runtime_error("cannot initialize candidate VM: " + Error(state));
        if (!lua_checkstack(state, 1024)) throw std::runtime_error("candidate stack allocation failed");
        std::unordered_set<std::string> names;
        static const std::unordered_set<std::string> reserved = {"Actor", "Vector2", "Transform", "Rigidbody", "SpriteRenderer", "SpriteAnimation", "Tilemap", "ParticleSystem", "Input", "Save", "Inventory", "Debug", "Physics", "Event", "Audio", "Image", "Text", "Camera", "Application", "Scene", "Collision", "HitResult", "ItemCatalog", "InventoryContainer", "io", "os", "package", "debug", "require", "load", "loadfile", "dofile", "print", "warn", "collectgarbage", "coroutine", "pcall", "xpcall"};
        for (const auto &source : sources) {
            current = &source;
            if (!Name(source.type) || !names.insert(source.type).second || reserved.contains(source.type)) throw std::runtime_error("invalid, duplicate or reserved component type");
            if (source.code.size() > 1024 * 1024 || source.resource.find('\0') != std::string::npos) throw std::runtime_error("source exceeds 1 MiB or resource contains NUL");
            lua_rawgeti(state, LUA_REGISTRYINDEX, context.backing); lua_getfield(state, -1, source.type.c_str());
            const bool collision = !lua_isnil(state, -1); lua_pop(state, 2);
            if (collision) throw std::runtime_error("component type collides with candidate standard-library global");
            lua_rawgeti(state, LUA_REGISTRYINDEX, context.allowed); lua_pushboolean(state, 1); lua_setfield(state, -2, source.type.c_str()); lua_pop(state, 1);
        }
        // Capture the immutable library identities before evaluating candidate code.
        Transfer transfer(state, live, context);
        lua_sethook(state, Hook, LUA_MASKCOUNT, 1);
        for (const auto &source : sources) {
            current = &source;
            lua_rawgeti(state, LUA_REGISTRYINDEX, context.backing); lua_getfield(state, -1, source.type.c_str());
            const void *previous = lua_topointer(state, -1); lua_pop(state, 2);
            phase = "compile";
            const auto chunk = "@" + source.resource;
            if (luaL_loadbufferx(state, source.code.data(), source.code.size(), chunk.c_str(), "t") != LUA_OK) throw std::runtime_error(Error(state));
            lua_rawgeti(state, LUA_REGISTRYINDEX, context.environment); lua_setupvalue(state, -2, 1);
            phase = "evaluate";
            if (lua_pcall(state, 0, 0, 0) != LUA_OK) throw std::runtime_error(context.heap_failed ? "candidate heap limit exceeded" : Error(state));
            if (context.heap_failed || context.instruction_failed) throw std::runtime_error("candidate execution budget exceeded");
            lua_rawgeti(state, LUA_REGISTRYINDEX, context.backing); lua_getfield(state, -1, source.type.c_str());
            if (!lua_istable(state, -1) || lua_topointer(state, -1) == previous) throw std::runtime_error("source must newly export its same-name component table");
            lua_pop(state, 2);
        }
        phase = "validate";
        for (const auto &source : sources) {
            current = &source;
            lua_rawgeti(state, LUA_REGISTRYINDEX, context.backing); lua_getfield(state, -1, source.type.c_str());
            if (!lua_istable(state, -1) || lua_getmetatable(state, -1)) throw std::runtime_error("component exports must be plain tables");
            lua_pushnil(state);
            while (lua_next(state, -2)) {
                if (lua_type(state, -2) != LUA_TSTRING) throw std::runtime_error("component export keys must be strings");
                std::size_t length = 0; const char *key = lua_tolstring(state, -2, &length);
                if ((length == 5 && std::memcmp(key, "actor", 5) == 0) || (length == 3 && std::memcmp(key, "key", 3) == 0))
                    throw std::runtime_error("component exports cannot override actor/key");
                lua_pop(state, 1);
            }
            transfer.Validate(-1); lua_pop(state, 2);
        }
        phase = "transfer";
        GCGuard gc(live);
        if (!lua_checkstack(live, 1024)) throw std::runtime_error("cannot reserve live stack for bounded graph transfer");
        for (const auto &source : sources) {
            current = &source;
            lua_rawgeti(state, LUA_REGISTRYINDEX, context.backing); lua_getfield(state, -1, source.type.c_str());
            transfer.Copy(-1); batch.exports.emplace(source.type, luabridge::LuaRef::fromStack(live, -1)); lua_pop(live, 1); lua_pop(state, 2);
        }
        batch.ok = true;
    } catch (const std::exception &error) { fail(error.what()); }
    // No stage table can have acquired a __gc/__close finalizer through the
    // restricted setmetatable. Keep its sticky hook active until close anyway.
    return batch;
}
} // namespace ScriptReloadCandidate
