#include "RegistrationDetail.h"
#include "core/AppUpdate.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"

// Update: the packaged game's updates from its GitHub Releases (AppUpdate).
namespace {

using APIRegistrationDetail::LuaState;

void SetString(lua_State *L, const char *field, const std::string &value) {
    lua_pushlstring(L, value.data(), value.size());
    lua_setfield(L, -2, field);
}

// Update.Status() -> {state, current, latest, notes, asset, size, progress, error, installable}
int Status(lua_State *L) {
    const AppUpdate::Status s = AppUpdate::GetStatus();
    lua_createtable(L, 0, 9);
    SetString(L, "state", s.state);
    SetString(L, "current", s.current);
    SetString(L, "latest", s.latest);
    SetString(L, "notes", s.notes);
    SetString(L, "asset", s.asset);
    SetString(L, "error", s.error);
    lua_pushinteger(L, static_cast<lua_Integer>(s.size));
    lua_setfield(L, -2, "size");
    lua_pushnumber(L, s.progress);
    lua_setfield(L, -2, "progress");
    lua_pushboolean(L, s.installable);
    lua_setfield(L, -2, "installable");
    return 1;
}

} // namespace

namespace APIRegistrationDetail {

void RegisterUpdateAPI() {
    luabridge::getGlobalNamespace(LuaState())
        .beginNamespace("Update")
        .addFunction("Check", &AppUpdate::Check)
        .addFunction("Download", &AppUpdate::Download)
        .addFunction("Install", &AppUpdate::Install)
        .addFunction("Status", &Status)
        .endNamespace();
}

} // namespace APIRegistrationDetail
