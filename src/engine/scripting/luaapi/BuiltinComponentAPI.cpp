#include "RegistrationDetail.h"
#include "rendering/SpriteRenderer.h"
#include "scene/Transform.h"
#include "scene/Tilemap.h"
#include "rendering/SpriteAnimation.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <cmath>
#include <stdexcept>

namespace {

using APIRegistrationDetail::LuaState;

template <typename Component>
ActorHandle GetComponentActor(const Component *component) {
    return component->actor_handle;
}

template <typename Component>
void SetComponentActor(Component *component, ActorHandle handle) {
    component->actor_handle = handle;
    component->actor = handle.Resolve();
}

// One call instead of two property writes for per-frame movement.
void SetTransformLocalPosition(Transform *value, float x, float y) {
    value->x = x;
    value->y = y;
}
float GetTransformRotation(const Transform *value) { return value->rotation; }
void SetTransformRotation(Transform *value, float rotation) {
    if (value->actor_handle.IsValid() &&
        !value->actor_handle.GetComponent("Tilemap").isNil() && rotation != 0)
        throw std::runtime_error("Tilemap rotation must remain zero");
    value->rotation = rotation;
}

void InjectTransformAPI() {
    luabridge::getGlobalNamespace(LuaState())
        .beginClass<Transform>("Transform")
        .addConstructor<void (*)()>()
        .addProperty("actor", &GetComponentActor<Transform>, &SetComponentActor<Transform>)
        .addPropertyReadWrite("key", &Transform::key)
        .addPropertyReadWrite("enabled", &Transform::enabled)
        .addPropertyReadWrite("x", &Transform::x)
        .addPropertyReadWrite("y", &Transform::y)
        .addProperty("rotation", &GetTransformRotation, &SetTransformRotation)
        .addFunction("SetLocalPosition", &SetTransformLocalPosition)
        .endClass();
}

void InjectSpriteRendererAPI() {
    luabridge::getGlobalNamespace(LuaState())
        .beginClass<SpriteRenderer>("SpriteRenderer")
        .addConstructor<void (*)()>()
        .addProperty("actor", &GetComponentActor<SpriteRenderer>, &SetComponentActor<SpriteRenderer>)
        .addPropertyReadWrite("key", &SpriteRenderer::key)
        .addPropertyReadWrite("enabled", &SpriteRenderer::enabled)
        .addPropertyReadWrite("sprite", &SpriteRenderer::sprite)
        .addPropertyReadWrite("r", &SpriteRenderer::r)
        .addPropertyReadWrite("g", &SpriteRenderer::g)
        .addPropertyReadWrite("b", &SpriteRenderer::b)
        .addPropertyReadWrite("a", &SpriteRenderer::a)
        .addPropertyReadWrite("pivot_x", &SpriteRenderer::pivot_x)
        .addPropertyReadWrite("pivot_y", &SpriteRenderer::pivot_y)
        .addPropertyReadWrite("scale_x", &SpriteRenderer::scale_x)
        .addPropertyReadWrite("scale_y", &SpriteRenderer::scale_y)
        .addPropertyReadWrite("sprite_row", &SpriteRenderer::sprite_row)
        .addPropertyReadWrite("sprite_column", &SpriteRenderer::sprite_column)
        .addPropertyReadWrite("sorting_order", &SpriteRenderer::sorting_order)
        .addPropertyReadWrite("auto_sorting_order", &SpriteRenderer::auto_sorting_order)
        .addFunction("SetSpriteCell", &SpriteRenderer::SetSpriteCell)
        .endClass();
}

luabridge::LuaRef GetTileCell(Tilemap *map, const std::string &layer, double x, double y) {
    const auto value = map->GetCell(layer, x, y);
    return value ? luabridge::LuaRef(LuaState(), *value) : luabridge::LuaRef(LuaState());
}

std::string GetMapResource(const Tilemap *map) { return map->map_resource; }

bool PlayAnimation(SpriteAnimation *animation, const std::string &clip) { return animation->Play(clip); }
std::string AnimationResource(const SpriteAnimation *animation) { return animation->animation_resource; }
std::string AnimationTarget(const SpriteAnimation *animation) { return animation->renderer_key; }
std::string AnimationInitialClip(const SpriteAnimation *animation) { return animation->clip; }
void InjectSpriteAnimationAPI() {
    luabridge::getGlobalNamespace(LuaState())
        .beginClass<SpriteAnimation>("SpriteAnimation")
        .addProperty("actor", &GetComponentActor<SpriteAnimation>, &SetComponentActor<SpriteAnimation>)
        .addPropertyReadWrite("key", &SpriteAnimation::key)
        .addPropertyReadWrite("enabled", &SpriteAnimation::enabled)
        .addProperty("animation_resource", &AnimationResource)
        .addProperty("renderer_key", &AnimationTarget)
        .addProperty("clip", &AnimationInitialClip)
        .addProperty("current_clip", &SpriteAnimation::GetCurrentClip)
        .addProperty("frame", &SpriteAnimation::GetFrame)
        .addProperty("elapsed_ticks", &SpriteAnimation::GetElapsedTicks)
        .addProperty("playing", &SpriteAnimation::IsPlaying)
        .addProperty("paused", &SpriteAnimation::IsPaused)
        .addProperty("completed", &SpriteAnimation::IsCompleted)
        .addFunction("Play", &PlayAnimation)
        .addFunction("Restart", &SpriteAnimation::Restart)
        .addFunction("Pause", &SpriteAnimation::Pause)
        .addFunction("Resume", &SpriteAnimation::Resume)
        .addFunction("Stop", &SpriteAnimation::Stop)
        .addFunction("OnStart", &SpriteAnimation::OnStart)
        .addFunction("OnDestroy", &SpriteAnimation::OnDestroy)
        .endClass();
}

void InjectTilemapAPI() {
    luabridge::getGlobalNamespace(LuaState())
        .beginClass<Tilemap>("Tilemap")
        .addProperty("actor", &GetComponentActor<Tilemap>, &SetComponentActor<Tilemap>)
        .addPropertyReadWrite("key", &Tilemap::key)
        .addPropertyReadWrite("enabled", &Tilemap::enabled)
        .addProperty("map_resource", &GetMapResource)
        .addProperty("width", &Tilemap::GetWidth)
        .addProperty("height", &Tilemap::GetHeight)
        .addProperty("cell_size", &Tilemap::GetCellSize)
        .addProperty("revision", &Tilemap::GetRevision)
        .addFunction("WorldToCell", &Tilemap::WorldToCell)
        .addFunction("GetCellCenter", &Tilemap::GetCellCenter)
        .addFunction("InBounds", &Tilemap::InBounds)
        .addFunction("GetCell", &GetTileCell)
        .addFunction("SetCell", &Tilemap::SetCell)
        .addFunction("GetRevision", &Tilemap::GetRevision)
        .addFunction("OnStart", &Tilemap::OnStart)
        .addFunction("OnDestroy", &Tilemap::OnDestroy)
        .endClass();
}

} // namespace

namespace APIRegistrationDetail {

void RegisterBuiltinComponentAPI() {
    InjectTransformAPI();
    InjectSpriteRendererAPI();
    RegisterRigidbodyAPI();
    RegisterParticleSystemAPI();
    InjectTilemapAPI();
    InjectSpriteAnimationAPI();
}

} // namespace APIRegistrationDetail
