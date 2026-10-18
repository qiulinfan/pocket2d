#include "RegistrationDetail.h"
#include "LuaSnapshot.h"
#include "net/Netplay.h"
#include "net/UdpSocket.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"

#include <string>
#include <vector>

// Net: two-player netplay (see net/Netplay.h).
namespace {

std::string OptString(lua_State *L, int index, const char *field, const std::string &fallback) {
    lua_getfield(L, index, field);
    std::string value = lua_isstring(L, -1) ? lua_tostring(L, -1) : fallback;
    lua_pop(L, 1);
    return value;
}

std::vector<std::string> OptStrings(lua_State *L, int index, const char *field, const std::vector<std::string> &fallback) {
    lua_getfield(L, index, field);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return fallback; }
    std::vector<std::string> out;
    const auto n = lua_rawlen(L, -1);
    for (lua_Integer i = 1; i <= static_cast<lua_Integer>(n); ++i) {
        lua_rawgeti(L, -1, i);
        if (lua_isstring(L, -1)) out.emplace_back(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return out;
}

int OptInt(lua_State *L, int index, const char *field, int fallback) {
    lua_getfield(L, index, field);
    const int value = lua_isnumber(L, -1) ? static_cast<int>(lua_tointeger(L, -1)) : fallback;
    lua_pop(L, 1);
    return value;
}

// Net.Configure{local_prefix, sides, shared, mode = "lockstep" | "rollback", delay (0: automatic),
//               max_rollback, force_relay, game, name, stun, discovery_port}
int Configure(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    Netplay::Config c;
    c.local_prefix = OptString(L, 1, "local_prefix", c.local_prefix);
    c.sides = OptStrings(L, 1, "sides", c.sides);
    c.shared = OptStrings(L, 1, "shared", c.shared);
    c.mode = OptString(L, 1, "mode", "lockstep") == "rollback" ? Netplay::SyncMode::Rollback : Netplay::SyncMode::Lockstep;
    c.delay = OptInt(L, 1, "delay", c.delay);
    c.max_rollback = OptInt(L, 1, "max_rollback", c.max_rollback);
    lua_getfield(L, 1, "force_relay");
    c.force_relay = lua_toboolean(L, -1);
    lua_pop(L, 1);
    c.game = OptString(L, 1, "game", c.game);
    c.name = OptString(L, 1, "name", c.name);
    c.stun = OptStrings(L, 1, "stun", c.stun);
    c.discovery_port = OptInt(L, 1, "discovery_port", c.discovery_port);
    Netplay::Configure(c);
    return 0;
}

int Push(lua_State *L, bool ok) {
    lua_pushboolean(L, ok);
    return 1;
}

int Host(lua_State *L) { return Push(L, Netplay::Host(luaL_checkstring(L, 1), luaL_optstring(L, 2, ""))); }
int Join(lua_State *L) { return Push(L, Netplay::Join(luaL_checkstring(L, 1), luaL_checkstring(L, 2))); }
int HostDirect(lua_State *L) {
    return Push(L, Netplay::HostDirect(static_cast<int>(luaL_optinteger(L, 1, 0)), luaL_optstring(L, 2, "")));
}
int JoinDirect(lua_State *L) { return Push(L, Netplay::JoinDirect(luaL_checkstring(L, 1))); }
int CodeHost(lua_State *L) { return Push(L, Netplay::CodeHost(luaL_optstring(L, 1, ""))); }
int CodeJoin(lua_State *L) { return Push(L, Netplay::CodeJoin(luaL_checkstring(L, 1))); }
int CodeConnect(lua_State *L) { return Push(L, Netplay::CodeConnect(luaL_checkstring(L, 1))); }
int SyncTest(lua_State *L) { return Push(L, Netplay::SyncTest(static_cast<int>(luaL_optinteger(L, 1, 7)))); }
int Leave(lua_State *) {
    Netplay::Leave();
    return 0;
}
int Running(lua_State *L) { return Push(L, Netplay::Running()); }
int IsResimulating(lua_State *L) { return Push(L, Netplay::Resimulating()); }
int Checksum(lua_State *L) {
    Netplay::ReportChecksum(static_cast<std::uint32_t>(luaL_checkinteger(L, 1)),
                            static_cast<std::uint32_t>(luaL_checkinteger(L, 2) & 0xffffffff));
    return 0;
}

void SetString(lua_State *L, const char *field, const std::string &value) {
    lua_pushlstring(L, value.data(), value.size());
    lua_setfield(L, -2, field);
}
void SetInt(lua_State *L, const char *field, lua_Integer value) {
    lua_pushinteger(L, value);
    lua_setfield(L, -2, field);
}
void SetBool(lua_State *L, const char *field, bool value) {
    lua_pushboolean(L, value);
    lua_setfield(L, -2, field);
}

// Net.Status() -> {state, side, link, mode, room, code, nat, error, payload, seed, ping, delay, relay,
//                  frame, stalls, rollbacks, resimulated, max_rollback_seen, desync, desync_frame, peer, lan}
int Status(lua_State *L) {
    const Netplay::Status s = Netplay::GetStatus();
    lua_createtable(L, 0, 22);
    SetString(L, "state", Netplay::StateName(s.state));
    SetInt(L, "side", s.side);
    SetString(L, "link", s.link);
    SetString(L, "mode", s.mode);
    SetString(L, "room", s.room);
    SetString(L, "code", s.code);
    SetString(L, "nat", s.nat);
    SetString(L, "error", s.error);
    SetString(L, "payload", s.payload);
    SetInt(L, "seed", s.seed);
    SetInt(L, "ping", s.ping_ms);
    SetInt(L, "delay", s.delay);
    SetBool(L, "relay", s.relay);
    SetInt(L, "frame", s.frame);
    SetInt(L, "stalls", s.stalls);
    SetInt(L, "rollbacks", s.rollbacks);
    SetInt(L, "resimulated", s.resimulated);
    SetInt(L, "max_rollback_seen", s.max_rollback_seen);
    SetBool(L, "desync", s.desync);
    SetInt(L, "desync_frame", s.desync_frame);
    SetString(L, "peer", s.peer);
    SetString(L, "lan", s.lan);
    return 1;
}

// Net.Discover(on): LAN discovery of direct hosts with the configured game id.
int Discover(lua_State *L) {
    Netplay::Discover(lua_isnone(L, 1) || lua_toboolean(L, 1));
    return 0;
}

// Net.Hosts() -> {{address, name, payload, age}, ...}
int Hosts(lua_State *L) {
    const auto hosts = Netplay::FoundHosts();
    lua_createtable(L, static_cast<int>(hosts.size()), 0);
    for (std::size_t i = 0; i < hosts.size(); ++i) {
        lua_createtable(L, 0, 4);
        SetString(L, "address", hosts[i].address);
        SetString(L, "name", hosts[i].name);
        SetString(L, "payload", hosts[i].payload);
        SetInt(L, "age", hosts[i].age_ms);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

// Net.LocalAddresses() -> {{name, ip, kind = "lan" | "tailscale" | "zerotier" | "radmin"}, ...}
int LocalAddresses(lua_State *L) {
    const auto list = Net::LocalInterfaces();
    lua_createtable(L, static_cast<int>(list.size()), 0);
    for (std::size_t i = 0; i < list.size(); ++i) {
        lua_createtable(L, 0, 3);
        SetString(L, "name", list[i].name);
        const std::string text = Net::Address{list[i].ip, 0}.ToString(); // "a.b.c.d:0"
        SetString(L, "ip", text.substr(0, text.rfind(':')));
        SetString(L, "kind", list[i].kind);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

// Net.Track(root, ...): the roots of the rollback state (the Lua values a
// snapshot walks); without arguments the global table.
int Track(lua_State *L) {
    if (lua_gettop(L) == 0) {
        lua_pushglobaltable(L);
        LuaSnapshot::Track(L, 1, 1);
    } else {
        LuaSnapshot::Track(L, 1, lua_gettop(L));
    }
    return 0;
}

// Net.Static(table, ...): tables that never change, left out of snapshots.
int Static(lua_State *L) {
    for (int i = 1; i <= lua_gettop(L); ++i) LuaSnapshot::MarkStatic(L, i);
    return 0;
}

// Net.Snapshot(slot, "save" | "load" | "hash" | "objects"): the rollback state by hand (tests).
int Snapshot(lua_State *L) {
    const int slot = static_cast<int>(luaL_checkinteger(L, 1));
    const std::string what = luaL_optstring(L, 2, "save");
    if (what == "save") LuaSnapshot::Save(slot);
    else if (what == "load") LuaSnapshot::Load(slot);
    else if (what == "hash") { lua_pushinteger(L, LuaSnapshot::Hash(slot)); return 1; }
    else if (what == "objects") { lua_pushinteger(L, LuaSnapshot::Objects(slot)); return 1; }
    return 0;
}

} // namespace

namespace APIRegistrationDetail {
void RegisterNetAPI() {
    // Rollback state lives in Lua: snapshots of the tracked roots (default: the global table).
    Netplay::SetStateHooks({
        [](int slot) {
            if (!LuaSnapshot::HasRoots()) {
                lua_State *L = LuaState();
                lua_pushglobaltable(L);
                LuaSnapshot::Track(L, lua_gettop(L), lua_gettop(L));
                lua_pop(L, 1);
            }
            LuaSnapshot::Save(slot);
        },
        [](int slot) { LuaSnapshot::Load(slot); },
        [](int slot) { return LuaSnapshot::Hash(slot); },
    });
    luabridge::getGlobalNamespace(LuaState())
        .beginNamespace("Net")
        .addFunction("Configure", &Configure)
        .addFunction("Host", &Host)
        .addFunction("Join", &Join)
        .addFunction("HostDirect", &HostDirect)
        .addFunction("JoinDirect", &JoinDirect)
        .addFunction("CodeHost", &CodeHost)
        .addFunction("CodeJoin", &CodeJoin)
        .addFunction("CodeConnect", &CodeConnect)
        .addFunction("SyncTest", &SyncTest)
        .addFunction("Leave", &Leave)
        .addFunction("Status", &Status)
        .addFunction("Running", &Running)
        .addFunction("IsResimulating", &IsResimulating)
        .addFunction("Checksum", &Checksum)
        .addFunction("Discover", &Discover)
        .addFunction("Hosts", &Hosts)
        .addFunction("LocalAddresses", &LocalAddresses)
        .addFunction("Track", &Track)
        .addFunction("Static", &Static)
        .addFunction("Snapshot", &Snapshot)
        .endNamespace();
}
} // namespace APIRegistrationDetail
