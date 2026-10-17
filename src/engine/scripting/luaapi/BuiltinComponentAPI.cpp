#include "RegistrationDetail.h"
#include "rendering/SpriteRenderer.h"
#include "scene/Transform.h"
#include "scene/Tilemap.h"
#include "rendering/SpriteAnimation.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <cmath>
#include <set>
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

// One Lua array (1-based, row-major) built with raw sets.
luabridge::LuaRef GetTileCells(Tilemap *map, const std::string &layer, double x, double y, double width, double height) {
    const std::vector<int> cells = map->GetCells(layer, x, y, width, height);
    lua_State *state = LuaState();
    lua_createtable(state, static_cast<int>(cells.size()), 0);
    for (std::size_t i = 0; i < cells.size(); ++i) {
        lua_pushinteger(state, cells[i]);
        lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
    }
    luabridge::LuaRef table = luabridge::LuaRef::fromStack(state, -1);
    lua_pop(state, 1);
    return table;
}

// map:ComputeLight(x, y, width, height, {image=, sky=, air_decay=, solid_decay=, ambient=,
//                   open_ground={names}, sources={x, y, r, g, b, ...}, smooth=})
void ComputeTileLight(Tilemap *map, double x, double y, double width, double height, luabridge::LuaRef table) {
    if (!table.isTable()) throw std::invalid_argument("Tilemap:ComputeLight options must be a table");
    for (const double value : {x, y, width, height})
        if (!std::isfinite(value) || std::floor(value) != value || std::abs(value) > 1e6)
            throw std::invalid_argument("Tilemap:ComputeLight rectangle must be integers");
    // A misspelt or retired option would otherwise fall back to its default unnoticed.
    static const std::set<std::string, std::less<>> kOptions = {
        "image", "sky", "air_decay", "solid_decay", "ambient", "open_ground", "sources", "smooth"};
    lua_State *state = LuaState();
    table.push(state);
    lua_pushnil(state);
    while (lua_next(state, -2) != 0) {
        lua_pop(state, 1);
        const bool known = lua_type(state, -1) == LUA_TSTRING && kOptions.contains(lua_tostring(state, -1));
        if (!known) {
            const std::string key = luaL_tolstring(state, -1, nullptr);
            lua_pop(state, 3);
            throw std::invalid_argument("Tilemap:ComputeLight has no option '" + key + "'");
        }
    }
    lua_pop(state, 1);
    Tilemap::LightOptions options;
    const auto number = [&](const char *key, auto &field) {
        const luabridge::LuaRef value = table[key];
        if (value.isNil()) return;
        if (!value.isNumber()) throw std::invalid_argument(std::string("Tilemap:ComputeLight ") + key + " must be a number");
        field = static_cast<std::decay_t<decltype(field)>>(value.cast<double>().value());
    };
    const luabridge::LuaRef image = table["image"];
    if (!image.isString()) throw std::invalid_argument("Tilemap:ComputeLight needs an image name");
    options.image = image.cast<std::string>().value();
    number("sky", options.sky); number("ambient", options.ambient);
    number("air_decay", options.air_decay); number("solid_decay", options.solid_decay);
    const luabridge::LuaRef smooth = table["smooth"];
    if (!smooth.isNil()) {
        if (!smooth.isBool()) throw std::invalid_argument("Tilemap:ComputeLight smooth must be a boolean");
        options.smooth = smooth.cast<bool>().value();
    }
    const luabridge::LuaRef open = table["open_ground"];
    if (open.isTable())
        for (int i = 1; i <= open.length(); ++i) options.open_ground.push_back(open[i].cast<std::string>().value());
    const luabridge::LuaRef sources = table["sources"];
    if (sources.isTable()) {
        const int count = sources.length();
        options.sources.reserve(static_cast<std::size_t>(count));
        for (int i = 1; i <= count; ++i) options.sources.push_back(static_cast<float>(sources[i].cast<double>().value()));
    }
    map->ComputeLight(static_cast<int>(x), static_cast<int>(y), static_cast<int>(width), static_cast<int>(height), options);
}

// map:ComputeMap(x, y, width, height, {image=, colors={terrain={r, g, b}}, sky={r, g, b, a},
//                 cave={r, g, b, a}, cave_y=, step=})
void ComputeTileMap(Tilemap *map, double x, double y, double width, double height, luabridge::LuaRef table) {
    if (!table.isTable()) throw std::invalid_argument("Tilemap:ComputeMap options must be a table");
    for (const double value : {x, y, width, height})
        if (!std::isfinite(value) || std::floor(value) != value || std::abs(value) > 1e6)
            throw std::invalid_argument("Tilemap:ComputeMap rectangle must be integers");
    static const std::set<std::string, std::less<>> kOptions = {"image", "colors", "sky", "cave", "cave_y", "step",
                                                                 "fog"};
    lua_State *state = LuaState();
    table.push(state);
    lua_pushnil(state);
    while (lua_next(state, -2) != 0) {
        lua_pop(state, 1);
        const bool known = lua_type(state, -1) == LUA_TSTRING && kOptions.contains(lua_tostring(state, -1));
        if (!known) {
            const std::string key = luaL_tolstring(state, -1, nullptr);
            lua_pop(state, 3);
            throw std::invalid_argument("Tilemap:ComputeMap has no option '" + key + "'");
        }
    }
    lua_pop(state, 1);
    // An {r, g, b[, a]} table of 0..255 integers.
    const auto rgba = [](const luabridge::LuaRef &value, const std::string &what, bool alpha) {
        std::array<std::uint8_t, 4> out{0, 0, 0, 255};
        if (!value.isTable() || value.length() < 3 || value.length() > (alpha ? 4 : 3))
            throw std::invalid_argument("Tilemap:ComputeMap " + what + " must be {r, g, b" + (alpha ? "[, a]}" : "}"));
        for (int i = 1; i <= value.length(); ++i) {
            const luabridge::LuaRef channel = value[i];
            const double v = channel.isNumber() ? channel.cast<double>().value() : -1;
            if (!(v >= 0 && v <= 255) || std::floor(v) != v)
                throw std::invalid_argument("Tilemap:ComputeMap " + what + " channels must be integers 0..255");
            out[static_cast<std::size_t>(i - 1)] = static_cast<std::uint8_t>(v);
        }
        return out;
    };
    Tilemap::MapOptions options;
    const luabridge::LuaRef image = table["image"];
    if (!image.isString()) throw std::invalid_argument("Tilemap:ComputeMap needs an image name");
    options.image = image.cast<std::string>().value();
    for (const char *key : {"sky", "cave"}) {
        const luabridge::LuaRef value = table[key];
        if (!value.isNil()) (std::string(key) == "sky" ? options.sky : options.cave) = rgba(value, key, true);
    }
    const luabridge::LuaRef fog = table["fog"];
    if (!fog.isNil()) options.fog = rgba(fog, "fog", true);
    for (const auto &[key, field] : {std::pair{"cave_y", &options.cave_y}, std::pair{"step", &options.step}}) {
        const luabridge::LuaRef value = table[key];
        if (value.isNil()) continue;
        if (!value.isNumber() || std::floor(value.cast<double>().value()) != value.cast<double>().value())
            throw std::invalid_argument(std::string("Tilemap:ComputeMap ") + key + " must be an integer");
        *field = static_cast<int>(value.cast<double>().value());
    }
    const luabridge::LuaRef colors = table["colors"];
    if (!colors.isNil()) {
        if (!colors.isTable()) throw std::invalid_argument("Tilemap:ComputeMap colors must be a table");
        colors.push(state);
        lua_pushnil(state);
        while (lua_next(state, -2) != 0) {
            if (lua_type(state, -2) != LUA_TSTRING) {
                lua_pop(state, 3);
                throw std::invalid_argument("Tilemap:ComputeMap colors are keyed by terrain name");
            }
            const std::string name = lua_tostring(state, -2);
            const luabridge::LuaRef value = luabridge::LuaRef::fromStack(state, -1);
            lua_pop(state, 1);
            try {
                const auto c = rgba(value, "colors." + name, false);
                options.colors[name] = {c[0], c[1], c[2]};
            } catch (...) {
                lua_pop(state, 2);
                throw;
            }
        }
        lua_pop(state, 1);
    }
    map->ComputeMap(static_cast<int>(x), static_cast<int>(y), static_cast<int>(width), static_cast<int>(height), options);
}

luabridge::LuaRef GetTileLayerNames(Tilemap *map) {
    luabridge::LuaRef names = luabridge::newTable(LuaState());
    int index = 1;
    for (const auto &name : map->GetLayerNames()) names[index++] = name;
    return names;
}

luabridge::LuaRef GetTileTerrain(Tilemap *map, const std::string &layer, double x, double y) {
    const auto value = map->GetTerrain(layer, x, y);
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
        .addFunction("GetCells", &GetTileCells)
        .addFunction("GetLayerNames", &GetTileLayerNames)
        .addFunction("ComputeLight", &ComputeTileLight)
        .addFunction("ComputeMap", &ComputeTileMap)
        .addFunction("RevealMap", +[](Tilemap *map, double x, double y, double radius) {
            for (const double value : {x, y, radius})
                if (!std::isfinite(value) || std::abs(value) > 1e6)
                    throw std::invalid_argument("Tilemap:RevealMap needs finite numbers");
            map->RevealMap(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)), static_cast<int>(radius));
        })
        .addFunction("IsRevealed", +[](Tilemap *map, double x, double y) {
            return map->IsRevealed(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)));
        })
        .addFunction("SaveRevealed", &Tilemap::SaveRevealed)
        .addFunction("LoadRevealed", &Tilemap::LoadRevealed)
        .addFunction("GetTerrain", &GetTileTerrain)
        .addFunction("SetTerrain", &Tilemap::SetTerrain)
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
