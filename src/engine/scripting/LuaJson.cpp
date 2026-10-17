#include "lua.hpp"
#include "scripting/LuaJson.h"
#include "core/Vector2.h"
#include "scene/ActorHandle.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace {
using JsonWriter = rapidjson::Writer<rapidjson::StringBuffer>;
void WritePayload(lua_State *state, int index, JsonWriter &out, int depth) {
    constexpr std::size_t kMaxString = 256;
    constexpr int kMaxEntries = 32, kMaxDepth = 4;
    index = lua_absindex(state, index);
    switch (lua_type(state, index)) {
    case LUA_TBOOLEAN: out.Bool(lua_toboolean(state, index) != 0); return;
    case LUA_TNUMBER:
        if (lua_isinteger(state, index)) out.Int64(lua_tointeger(state, index));
        else if (std::isfinite(lua_tonumber(state, index))) out.Double(lua_tonumber(state, index));
        else out.Null();
        return;
    case LUA_TSTRING: {
        std::size_t length = 0;
        const char *text = lua_tolstring(state, index, &length);
        out.String(text, static_cast<rapidjson::SizeType>(std::min(length, kMaxString)));
        return;
    }
    case LUA_TUSERDATA: {
        const auto value = luabridge::LuaRef::fromStack(state, index);
        if (value.isInstance<Vector2>()) {
            const auto vector = value.cast<Vector2>().value();
            out.StartObject();
            out.Key("x"); out.Double(std::isfinite(vector.x) ? vector.x : 0.0);
            out.Key("y"); out.Double(std::isfinite(vector.y) ? vector.y : 0.0);
            out.EndObject();
        } else if (value.isInstance<ActorHandle>()) {
            out.StartObject();
            out.Key("actor_uid"); out.String(std::to_string(value.cast<ActorHandle>().value().uid).c_str());
            out.EndObject();
        } else out.String("<userdata>");
        return;
    }
    case LUA_TFUNCTION: out.String("<function>"); return;
    case LUA_TTABLE: break;
    default: out.Null(); return;
    }
    if (depth >= kMaxDepth || !lua_checkstack(state, 4)) { out.String("<table>"); return; }
    const lua_Unsigned length = lua_rawlen(state, index);
    if (length > 0) {
        out.StartArray();
        for (lua_Unsigned i = 1; i <= length && i <= kMaxEntries; ++i) {
            lua_rawgeti(state, index, static_cast<lua_Integer>(i));
            WritePayload(state, -1, out, depth + 1);
            lua_pop(state, 1);
        }
        out.EndArray();
        return;
    }
    out.StartObject();
    int entries = 0;
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        if (entries++ < kMaxEntries && (lua_type(state, -2) == LUA_TSTRING || lua_type(state, -2) == LUA_TNUMBER)) {
            lua_pushvalue(state, -2); // tostring on a copy keeps lua_next's key intact
            std::size_t key_length = 0;
            const char *key = lua_tolstring(state, -1, &key_length);
            out.Key(key, static_cast<rapidjson::SizeType>(std::min(key_length, kMaxString)));
            lua_pop(state, 1);
            WritePayload(state, -1, out, depth + 1);
        }
        lua_pop(state, 1);
    }
    out.EndObject();
}

} // namespace

std::string LuaJson::Bounded(const luabridge::LuaRef &payload) {
    lua_State *state = payload.state();
    if (state == nullptr) return "null";
    const int top = lua_gettop(state);
    rapidjson::StringBuffer buffer;
    JsonWriter out(buffer);
    try {
        payload.push(state);
        WritePayload(state, -1, out, 0);
    } catch (...) {
        lua_settop(state, top);
        return "null";
    }
    lua_settop(state, top);
    return buffer.GetString();
}

void LuaJson::Push(lua_State *state, const rapidjson::Value &value) {
    if (!lua_checkstack(state, 4)) throw std::runtime_error("insufficient Lua stack for a JSON value");
    if (value.IsNull()) lua_pushnil(state);
    else if (value.IsBool()) lua_pushboolean(state, value.GetBool());
    else if (value.IsInt64()) lua_pushinteger(state, static_cast<lua_Integer>(value.GetInt64()));
    else if (value.IsNumber()) lua_pushnumber(state, value.GetDouble());
    else if (value.IsString()) lua_pushlstring(state, value.GetString(), value.GetStringLength());
    else if (value.IsArray()) {
        lua_createtable(state, static_cast<int>(value.Size()), 0);
        for (rapidjson::SizeType i = 0; i < value.Size(); ++i) { Push(state, value[i]); lua_rawseti(state, -2, i + 1); }
    } else {
        lua_createtable(state, 0, static_cast<int>(value.MemberCount()));
        for (auto member = value.MemberBegin(); member != value.MemberEnd(); ++member) {
            lua_pushlstring(state, member->name.GetString(), member->name.GetStringLength());
            Push(state, member->value);
            lua_rawset(state, -3);
        }
    }
}
