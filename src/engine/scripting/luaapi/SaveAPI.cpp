#include "RegistrationDetail.h"
#include "core/World.h"
#include "scripting/GameSave.h"
#include "shared/persistence/SaveStore.h"
#include "SDL3/SDL.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
struct SaveNamespace { std::string active_namespace; };
std::string &ActiveNamespace() { return World::Current().State<SaveNamespace>().active_namespace; }
using Allocator = rapidjson::Document::AllocatorType;
struct InvalidData : std::runtime_error { using std::runtime_error::runtime_error; };
struct Conversion {
    std::size_t nodes = 0, string_bytes = 0;
    std::unordered_set<const void *> ancestors;
    void Node(std::size_t depth) {
        if (depth > SaveStore::kMaxDepth || ++nodes > SaveStore::kMaxNodes)
            throw InvalidData("save data exceeds depth or node limit");
    }
    void String(std::size_t bytes) {
        if (bytes > SaveStore::kMaxFileBytes - string_bytes)
            throw InvalidData("save data exceeds string byte limit");
        string_bytes += bytes;
    }
};
struct StackScope {
    lua_State *state;
    int top;
    explicit StackScope(lua_State *value) : state(value), top(lua_gettop(value)) {}
    ~StackScope() { lua_settop(state, top); }
};
struct AncestorScope {
    Conversion &context;
    const void *identity;
    ~AncestorScope() { context.ancestors.erase(identity); }
};
rapidjson::Value Encode(lua_State *state, int index, Allocator &allocator,
                        Conversion &context, std::size_t depth) {
    context.Node(depth);
    if (!lua_checkstack(state, 8)) throw InvalidData("insufficient Lua stack for save data");
    StackScope stack(state);
    index = lua_absindex(state, index);
    switch (lua_type(state, index)) {
    case LUA_TBOOLEAN: return rapidjson::Value(lua_toboolean(state, index) != 0);
    case LUA_TNUMBER:
        if (lua_isinteger(state, index)) return rapidjson::Value(static_cast<std::int64_t>(lua_tointeger(state, index)));
        else {
            const double value = lua_tonumber(state, index);
            if (!std::isfinite(value)) throw InvalidData("save numbers must be finite");
            return rapidjson::Value(value);
        }
    case LUA_TSTRING: {
        std::size_t length = 0;
        const char *value = lua_tolstring(state, index, &length);
        context.String(length);
        return rapidjson::Value(value, static_cast<rapidjson::SizeType>(length), allocator);
    }
    case LUA_TTABLE: break;
    default: throw InvalidData("save values must be tables, strings, booleans or finite numbers");
    }
    const void *identity = lua_topointer(state, index);
    if (!context.ancestors.insert(identity).second) throw InvalidData("save tables must not contain cycles");
    AncestorScope ancestor{context, identity};
    std::vector<std::string> keys;
    std::size_t count = 0, maximum = 0;
    bool numeric = false, textual = false;
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        if (++count > SaveStore::kMaxNodes) throw InvalidData("save table exceeds node limit");
        if (lua_type(state, -2) == LUA_TSTRING) {
            textual = true;
            std::size_t length = 0;
            const char *key = lua_tolstring(state, -2, &length);
            context.String(length);
            keys.emplace_back(key, length);
        } else if (lua_isinteger(state, -2)) {
            numeric = true;
            const auto key = lua_tointeger(state, -2);
            if (key <= 0 || static_cast<std::uint64_t>(key) > SaveStore::kMaxNodes)
                throw InvalidData("array keys must be positive dense integers");
            maximum = std::max(maximum, static_cast<std::size_t>(key));
        } else throw InvalidData("save table keys must be strings or dense positive integers");
        lua_pop(state, 1);
    }
    if ((numeric && textual) || (numeric && maximum != count))
        throw InvalidData("save tables must be objects or dense arrays without mixed keys");
    rapidjson::Value result(numeric ? rapidjson::kArrayType : rapidjson::kObjectType);
    if (numeric) {
        result.Reserve(static_cast<rapidjson::SizeType>(count), allocator);
        for (std::size_t key = 1; key <= count; ++key) {
            lua_rawgeti(state, index, static_cast<lua_Integer>(key));
            result.PushBack(Encode(state, -1, allocator, context, depth + 1), allocator);
            lua_pop(state, 1);
        }
    } else {
        std::sort(keys.begin(), keys.end());
        for (const auto &key : keys) {
            lua_pushlstring(state, key.data(), key.size());
            lua_rawget(state, index);
            rapidjson::Value name(key.data(), static_cast<rapidjson::SizeType>(key.size()), allocator);
            result.AddMember(name, Encode(state, -1, allocator, context, depth + 1), allocator);
            lua_pop(state, 1);
        }
    }
    return result;
}
void Decode(lua_State *state, const rapidjson::Value &value) {
    if (!lua_checkstack(state, 8)) throw InvalidData("insufficient Lua stack for loaded save");
    if (value.IsBool()) lua_pushboolean(state, value.GetBool());
    else if (value.IsInt64()) lua_pushinteger(state, static_cast<lua_Integer>(value.GetInt64()));
    else if (value.IsNumber()) lua_pushnumber(state, value.GetDouble());
    else if (value.IsString()) lua_pushlstring(state, value.GetString(), value.GetStringLength());
    else if (value.IsArray()) {
        lua_createtable(state, static_cast<int>(value.Size()), 0);
        for (rapidjson::SizeType i = 0; i < value.Size(); ++i) { Decode(state, value[i]); lua_rawseti(state, -2, i + 1); }
    } else if (value.IsObject()) {
        lua_createtable(state, 0, static_cast<int>(value.MemberCount()));
        for (auto member = value.MemberBegin(); member != value.MemberEnd(); ++member) {
            lua_pushlstring(state, member->name.GetString(), member->name.GetStringLength());
            Decode(state, member->value); lua_rawset(state, -3);
        }
    } else throw InvalidData("unsupported saved value");
}
void StringField(lua_State *state, const char *key, const std::string &value) {
    lua_pushlstring(state, value.data(), value.size()); lua_setfield(state, -2, key);
}
int StatusResult(lua_State *state, bool ok, const std::string &code = {}, const std::string &message = {}) {
    lua_newtable(state);
    lua_pushboolean(state, ok); lua_setfield(state, -2, "ok");
    if (!ok) { StringField(state, "code", code); StringField(state, "error", message); }
    return 1;
}
std::filesystem::path Directory() {
    if (!SaveStore::IsValidIdentifier(ActiveNamespace())) throw std::runtime_error("game.config save_namespace is not configured");
    if (const char *override_root = SDL_getenv("POCKET_SAVE_ROOT"); override_root && *override_root) {
        const std::string text(override_root);
        const auto root = std::filesystem::weakly_canonical(std::filesystem::absolute(
            std::filesystem::path(std::u8string(text.begin(), text.end()))));
        const auto directory = root / ActiveNamespace();
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(directory)))
            throw std::runtime_error("save namespace directory must not be a symbolic link");
        return directory;
    }
    char *raw = SDL_GetPrefPath("Pocket2D", ActiveNamespace().c_str());
    if (!raw) throw std::runtime_error(std::string("cannot resolve save directory: ") + SDL_GetError());
    const std::string text(raw); SDL_free(raw);
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
std::string Slot(lua_State *state) {
    std::size_t size = 0; const char *value = lua_tolstring(state, 1, &size);
    return std::string(value, size);
}
int Write(lua_State *state) {
    const int top = lua_gettop(state);
    if (top != 3 || lua_type(state, 1) != LUA_TSTRING || !lua_isinteger(state, 2) || !lua_istable(state, 3))
        return StatusResult(state, false, "invalid_argument", "Save.Write expects slot, positive integer schema version and data table");
    if (ActiveNamespace().empty()) return StatusResult(state, false, "not_configured", "game.config save_namespace is required");
    const auto version = lua_tointeger(state, 2);
    if (version <= 0 || version > std::numeric_limits<int>::max())
        return StatusResult(state, false, "invalid_argument", "schema_version must be a positive int");
    try {
        rapidjson::Document document;
        Conversion context;
        auto value = Encode(state, 3, document.GetAllocator(), context, 1);
        const auto status = SaveStore::Write(Directory(), Slot(state), static_cast<int>(version), value);
        return StatusResult(state, status.ok, status.code, status.message);
    } catch (const InvalidData &error) {
        lua_settop(state, top); return StatusResult(state, false, "invalid_data", error.what());
    } catch (const std::exception &error) {
        lua_settop(state, top); return StatusResult(state, false, "io_error", error.what());
    }
}
int Read(lua_State *state) {
    const int top = lua_gettop(state);
    if (top != 1 || lua_type(state, 1) != LUA_TSTRING)
        return StatusResult(state, false, "invalid_argument", "Save.Read expects one slot string");
    if (ActiveNamespace().empty()) return StatusResult(state, false, "not_configured", "game.config save_namespace is required");
    try {
        const auto result = SaveStore::Read(Directory(), Slot(state));
        if (!result.status.ok) return StatusResult(state, false, result.status.code, result.status.message);
        StatusResult(state, true);
        lua_pushinteger(state, result.schema_version); lua_setfield(state, -2, "schema_version");
        Decode(state, result.data); lua_setfield(state, -2, "data");
        return 1;
    } catch (const InvalidData &error) {
        lua_settop(state, top); return StatusResult(state, false, "invalid_data", error.what());
    } catch (const std::exception &error) {
        lua_settop(state, top); return StatusResult(state, false, "io_error", error.what());
    }
}
}
namespace GameSave {
void Configure(const std::string &save_namespace) {
    ActiveNamespace().clear();
    if (!save_namespace.empty() && !SaveStore::IsValidIdentifier(save_namespace))
        throw std::runtime_error("game.config save_namespace must be 1..64 ASCII letters/digits/underscore/hyphen, starting with a letter or digit");
    ActiveNamespace() = save_namespace;
}
}
namespace APIRegistrationDetail {
void RegisterSaveAPI() {
    luabridge::getGlobalNamespace(LuaState()).beginNamespace("Save")
        .addFunction("Write", &Write).addFunction("Read", &Read).endNamespace();
}
}
