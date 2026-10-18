#include "RegistrationDetail.h"
#include "net/Netplay.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"

#include <string>
#include <vector>

// Net: two-player lockstep netplay (see net/Netplay.h).
namespace {

std::string OptString(lua_State *L, int index, const char *field, const std::string &fallback) {
    lua_getfield(L, index, field);
    std::string value = lua_isstring(L, -1) ? lua_tostring(L, -1) : fallback;
    lua_pop(L, 1);
    return value;
}

// Net.Configure{local_prefix = "p1_", sides = {"p1_", "p2_"}, shared = {"confirm"}, delay = 3 (0: automatic), force_relay}
int Configure(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    Netplay::Config c;
    c.local_prefix = OptString(L, 1, "local_prefix", c.local_prefix);
    lua_getfield(L, 1, "sides");
    if (lua_istable(L, -1)) {
        c.sides.clear();
        for (int i = 1; i <= 2; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_isstring(L, -1)) c.sides.emplace_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "shared");
    if (lua_istable(L, -1)) {
        const auto n = lua_rawlen(L, -1);
        for (lua_Integer i = 1; i <= static_cast<lua_Integer>(n); ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_isstring(L, -1)) c.shared.emplace_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "force_relay");
    c.force_relay = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "delay");
    if (lua_isnumber(L, -1)) c.delay = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    Netplay::Configure(c);
    return 0;
}

int Host(lua_State *L) {
    lua_pushboolean(L, Netplay::Host(luaL_checkstring(L, 1), luaL_optstring(L, 2, "")));
    return 1;
}
int Join(lua_State *L) {
    lua_pushboolean(L, Netplay::Join(luaL_checkstring(L, 1), luaL_checkstring(L, 2)));
    return 1;
}
int HostDirect(lua_State *L) {
    lua_pushboolean(L, Netplay::HostDirect(static_cast<int>(luaL_optinteger(L, 1, 0)), luaL_optstring(L, 2, "")));
    return 1;
}
int JoinDirect(lua_State *L) {
    lua_pushboolean(L, Netplay::JoinDirect(luaL_checkstring(L, 1)));
    return 1;
}
int Leave(lua_State *) {
    Netplay::Leave();
    return 0;
}
int Checksum(lua_State *L) {
    Netplay::ReportChecksum(static_cast<std::uint32_t>(luaL_checkinteger(L, 1)),
                            static_cast<std::uint32_t>(luaL_checkinteger(L, 2) & 0xffffffff));
    return 0;
}

// Net.Status() -> {state, side, room, error, payload, seed, ping, delay, relay, frame, stalls, desync, desync_frame, peer, lan}
int Status(lua_State *L) {
    const Netplay::Status s = Netplay::GetStatus();
    lua_createtable(L, 0, 15);
    lua_pushstring(L, Netplay::StateName(s.state)); lua_setfield(L, -2, "state");
    lua_pushinteger(L, s.side); lua_setfield(L, -2, "side");
    lua_pushstring(L, s.room.c_str()); lua_setfield(L, -2, "room");
    lua_pushstring(L, s.error.c_str()); lua_setfield(L, -2, "error");
    lua_pushlstring(L, s.payload.data(), s.payload.size()); lua_setfield(L, -2, "payload");
    lua_pushinteger(L, s.seed); lua_setfield(L, -2, "seed");
    lua_pushinteger(L, s.ping_ms); lua_setfield(L, -2, "ping");
    lua_pushinteger(L, s.delay); lua_setfield(L, -2, "delay");
    lua_pushboolean(L, s.relay); lua_setfield(L, -2, "relay");
    lua_pushinteger(L, s.frame); lua_setfield(L, -2, "frame");
    lua_pushinteger(L, s.stalls); lua_setfield(L, -2, "stalls");
    lua_pushboolean(L, s.desync); lua_setfield(L, -2, "desync");
    lua_pushinteger(L, s.desync_frame); lua_setfield(L, -2, "desync_frame");
    lua_pushstring(L, s.peer.c_str()); lua_setfield(L, -2, "peer");
    lua_pushstring(L, s.lan.c_str()); lua_setfield(L, -2, "lan");
    return 1;
}

int Running(lua_State *L) {
    lua_pushboolean(L, Netplay::Running());
    return 1;
}

} // namespace

namespace APIRegistrationDetail {
void RegisterNetAPI() {
    luabridge::getGlobalNamespace(LuaState())
        .beginNamespace("Net")
        .addFunction("Configure", &Configure)
        .addFunction("Host", &Host)
        .addFunction("Join", &Join)
        .addFunction("HostDirect", &HostDirect)
        .addFunction("JoinDirect", &JoinDirect)
        .addFunction("Leave", &Leave)
        .addFunction("Status", &Status)
        .addFunction("Running", &Running)
        .addFunction("Checksum", &Checksum)
        .endNamespace();
}
} // namespace APIRegistrationDetail
