#include "scripting/APIRegistration.h"
#include "RegistrationDetail.h"
#include "core/World.h"

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

namespace APIRegistration {

// register host APIs and bindings into Lua
void Initialize(lua_State *lua_state) {
    using namespace APIRegistrationDetail;

    LuaState() = lua_state;
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
}

// update the current engine pointer used by Lua APIs
void BindEngine(Engine *engine) {
    APIRegistrationDetail::BoundEngine() = engine;
}

} // namespace APIRegistration
