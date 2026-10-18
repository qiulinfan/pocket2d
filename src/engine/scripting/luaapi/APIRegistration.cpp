#include "scripting/APIRegistration.h"
#include "RegistrationDetail.h"
#include "core/World.h"
#include "lua.hpp"
#include "pocket_detmath.h"

namespace APIRegistrationDetail {

namespace {
struct RegistrationState {
    lua_State *lua_state = nullptr;
    Engine *engine = nullptr;
};
RegistrationState &Registration() { return World::Current().State<RegistrationState>(); }
} // namespace

lua_State *&LuaState() { return Registration().lua_state; }
Engine *&BoundEngine() { return Registration().engine; }

} // namespace APIRegistrationDetail

namespace {
// math.log as Lua 5.4 defines it, on every platform: Lua's own uses log2 for
// base 2 except where luaconf.h declares the platform C89 (Windows), which
// computes log(x)/log(2) and so gives other last bits.
int MathLog(lua_State *L) {
    const lua_Number x = luaL_checknumber(L, 1);
    lua_Number result;
    if (lua_isnoneornil(L, 2)) {
        result = pocket_det_log(x);
    } else {
        const lua_Number base = luaL_checknumber(L, 2);
        if (base == 2.0) result = pocket_det_log2(x);
        else if (base == 10.0) result = pocket_det_log10(x);
        else result = pocket_det_log(x) / pocket_det_log(base);
    }
    lua_pushnumber(L, result);
    return 1;
}

void RegisterDeterministicMath(lua_State *L) {
    lua_getglobal(L, "math");
    if (lua_istable(L, -1)) {
        lua_pushcfunction(L, MathLog);
        lua_setfield(L, -2, "log");
    }
    lua_pop(L, 1);
}
} // namespace

namespace APIRegistration {

// register host APIs and bindings into Lua
void Initialize(lua_State *lua_state) {
    using namespace APIRegistrationDetail;

    LuaState() = lua_state;
    RegisterDeterministicMath(lua_state);
    RegisterApplicationAndDebugAPI();
    RegisterSceneAndCameraAPI();
    RegisterAudioAPI();
    RegisterRenderingAPI();
    RegisterInputAPI();
    RegisterActorAPI();
    RegisterEventAPI();
    RegisterPhysicsAPI();
    RegisterBuiltinComponentAPI();
    RegisterSaveAPI();
    RegisterInventoryAPI();
    RegisterNetAPI();
}

// update the current engine pointer used by Lua APIs
void BindEngine(Engine *engine) {
    APIRegistrationDetail::BoundEngine() = engine;
}

} // namespace APIRegistration
