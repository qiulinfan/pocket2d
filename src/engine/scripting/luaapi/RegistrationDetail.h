#ifndef API_REGISTRATION_DETAIL_H
#define API_REGISTRATION_DETAIL_H

struct lua_State;
class Engine;

namespace APIRegistrationDetail {

// The Lua state and Engine registered for World::Current().
lua_State *&LuaState();
Engine *&BoundEngine();

void RegisterApplicationAndDebugAPI();
void RegisterSceneAndCameraAPI();
void RegisterAudioAPI();
void RegisterRenderingAPI();
void RegisterInputAPI();
void RegisterActorAPI();
void RegisterEventAPI();
void RegisterPhysicsAPI();
void RegisterBuiltinComponentAPI();
void RegisterRigidbodyAPI();
void RegisterParticleSystemAPI();
void RegisterSaveAPI();
void RegisterInventoryAPI();
void RegisterNetAPI();
void RegisterUpdateAPI();

} // namespace APIRegistrationDetail

#endif
