#include "RegistrationDetail.h"
#include "core/Engine.h"
#include "scripting/LuaBoundary.h"
#include "physics/Rigidbody.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <stdexcept>

namespace {
ActorHandle ActorOf(const Rigidbody *body) { return body->actor_handle; }
void SetActor(Rigidbody *body, ActorHandle handle) { body->actor_handle=handle; body->actor=handle.Resolve(); }
double NumericInput(const luabridge::LuaRef &input) {
    if (input.type() != LUA_TNUMBER) throw std::invalid_argument("Rigidbody numeric value requires a number");
    return LuaBoundary::Cast<double>(input);
}
std::string StringInput(const luabridge::LuaRef &input) {
    if (input.type() != LUA_TSTRING) throw std::invalid_argument("Rigidbody enum requires a string");
    return LuaBoundary::Cast<std::string>(input);
}
template<bool Rigidbody::*member> bool Boolean(const Rigidbody *body) { return body->*member; }
template<bool Rigidbody::*member> void SetBoolean(Rigidbody *body, luabridge::LuaRef input) {
    if (input.type() != LUA_TBOOLEAN) throw std::invalid_argument("Rigidbody flag requires a boolean");
    const bool value = LuaBoundary::Cast<bool>(input);
    if (body->*member == value) return;
    body->*member = value;
    if constexpr (member == &Rigidbody::enabled) {
        if (APIRegistrationDetail::BoundEngine())
            APIRegistrationDetail::BoundEngine()->NotifyRuntimePhysicsConfigurationChanged();
    }
}
template<float Rigidbody::*member> float Number(const Rigidbody *body) { return body->*member; }
#define NUMERIC_SETTER(field) void Set_##field(Rigidbody *body,luabridge::LuaRef input) { body->field=Rigidbody::ValidateNumericProperty(#field,NumericInput(input)); }
NUMERIC_SETTER(x) NUMERIC_SETTER(y) NUMERIC_SETTER(rotation) NUMERIC_SETTER(gravity_scale)
NUMERIC_SETTER(density) NUMERIC_SETTER(angular_friction) NUMERIC_SETTER(width) NUMERIC_SETTER(height)
NUMERIC_SETTER(radius) NUMERIC_SETTER(trigger_width) NUMERIC_SETTER(trigger_height)
NUMERIC_SETTER(trigger_radius) NUMERIC_SETTER(friction) NUMERIC_SETTER(bounciness)
#undef NUMERIC_SETTER
std::string BodyType(const Rigidbody *body) { return body->body_type; }
void SetBodyType(Rigidbody *body,luabridge::LuaRef input) {
    const std::string value = StringInput(input);
    if(value!="dynamic"&&value!="static"&&value!="kinematic") throw std::invalid_argument("Rigidbody body_type must be dynamic/static/kinematic");
    if (body->body_type == value) return;
    body->body_type=value;
    if (APIRegistrationDetail::BoundEngine())
        APIRegistrationDetail::BoundEngine()->NotifyRuntimePhysicsConfigurationChanged();
}
std::string ColliderType(const Rigidbody *body) { return body->collider_type; }
void SetColliderType(Rigidbody *body,luabridge::LuaRef input) {
    const std::string value = StringInput(input);
    if(value!="box"&&value!="circle") throw std::invalid_argument("Rigidbody collider_type must be box/circle");
    body->collider_type=value;
}
std::string TriggerType(const Rigidbody *body) { return body->trigger_type; }
void SetTriggerType(Rigidbody *body,luabridge::LuaRef input) {
    const std::string value = StringInput(input);
    if(value!="box"&&value!="circle") throw std::invalid_argument("Rigidbody trigger_type must be box/circle");
    body->trigger_type=value;
}
void Rotation(Rigidbody *body,luabridge::LuaRef input) { const double value = NumericInput(input); body->SetRotation(Rigidbody::ValidateNumericProperty("rotation",value)); }
void AngularVelocity(Rigidbody *body,luabridge::LuaRef input) { const double value = NumericInput(input); body->SetAngularVelocity(Rigidbody::ValidateNumericProperty("rotation",value)); }
void GravityScale(Rigidbody *body,luabridge::LuaRef input) { const double value = NumericInput(input); body->SetGravityScale(Rigidbody::ValidateNumericProperty("gravity_scale",value)); }
}
namespace APIRegistrationDetail {
void RegisterRigidbodyAPI() {
    luabridge::getGlobalNamespace(LuaState())
        .beginClass<Rigidbody>("Rigidbody")
        .addConstructor<void (*)()>()
        .addProperty("actor",&ActorOf,&SetActor)
        .addPropertyReadWrite("key",&Rigidbody::key)
        .addProperty("enabled",&Boolean<&Rigidbody::enabled>,&SetBoolean<&Rigidbody::enabled>)
#define NUMERIC_PROPERTY(field) .addProperty(#field,&Number<&Rigidbody::field>,&Set_##field)
        NUMERIC_PROPERTY(x) NUMERIC_PROPERTY(y) NUMERIC_PROPERTY(rotation) NUMERIC_PROPERTY(gravity_scale)
        NUMERIC_PROPERTY(density) NUMERIC_PROPERTY(angular_friction) NUMERIC_PROPERTY(width) NUMERIC_PROPERTY(height)
        NUMERIC_PROPERTY(radius) NUMERIC_PROPERTY(trigger_width) NUMERIC_PROPERTY(trigger_height)
        NUMERIC_PROPERTY(trigger_radius) NUMERIC_PROPERTY(friction) NUMERIC_PROPERTY(bounciness)
#undef NUMERIC_PROPERTY
        .addProperty("body_type",&BodyType,&SetBodyType)
        .addProperty("precise",&Boolean<&Rigidbody::precise>,&SetBoolean<&Rigidbody::precise>)
        .addProperty("has_collider",&Boolean<&Rigidbody::has_collider>,&SetBoolean<&Rigidbody::has_collider>)
        .addProperty("has_trigger",&Boolean<&Rigidbody::has_trigger>,&SetBoolean<&Rigidbody::has_trigger>)
        .addProperty("collider_type",&ColliderType,&SetColliderType)
        .addProperty("trigger_type",&TriggerType,&SetTriggerType)
        .addFunction("OnStart",&Rigidbody::OnStart)
        .addFunction("OnDestroy",&Rigidbody::OnDestroy)
        .addFunction("AddForce",&Rigidbody::AddForce)
        .addFunction("SetVelocity",&Rigidbody::SetVelocity)
        .addFunction("SetPosition",&Rigidbody::SetPosition)
        .addFunction("SetRotation",&Rotation)
        .addFunction("SetAngularVelocity",&AngularVelocity)
        .addFunction("SetGravityScale",&GravityScale)
        .addFunction("SetUpDirection",&Rigidbody::SetUpDirection)
        .addFunction("SetRightDirection",&Rigidbody::SetRightDirection)
        .addFunction("GetPosition",&Rigidbody::GetPosition)
        .addFunction("GetRotation",&Rigidbody::GetRotation)
        .addFunction("GetVelocity",&Rigidbody::GetVelocity)
        .addFunction("GetAngularVelocity",&Rigidbody::GetAngularVelocity)
        .addFunction("GetGravityScale",&Rigidbody::GetGravityScale)
        .addFunction("GetUpDirection",&Rigidbody::GetUpDirection)
        .addFunction("GetRightDirection",&Rigidbody::GetRightDirection)
        .endClass();
}
}
