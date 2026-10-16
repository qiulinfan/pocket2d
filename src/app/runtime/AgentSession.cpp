#include "app/runtime/AgentSession.h"
#include "engine/core/Engine.h"
#include "engine/core/World.h"
#include "engine/core/Log.h"
#include "engine/core/FrameClock.h"
#include "engine/input/Input.h"
#include "engine/scripting/ComponentManager.h"
#include "engine/scripting/ScriptBudget.h"
#include "engine/scripting/ScriptAPIContract.h"
#include "engine/scripting/EventBus.h"
#include "app/runtime/AgentProtocolSchema.h"
#include "engine/scene/Tilemap.h"
#include "shared/resources/ResourcePath.h"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <charconv>
#include <iostream>
#include <memory>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <optional>
#include <type_traits>
#include <variant>

namespace {
using Writer = rapidjson::Writer<rapidjson::StringBuffer>;

struct SceneLoadFailure : std::runtime_error {
    bool requires_reset;
    SceneLoadFailure(const std::string &message, bool reset)
        : std::runtime_error(message), requires_reset(reset) {}
};

void WriteNumber(Writer &out, double value) {
    if (!std::isfinite(value)) throw std::runtime_error("observation contains a non-finite number");
    out.Double(value);
}

void WriteValue(Writer &out, const Actor::ComponentPropertyValue &value) {
    std::visit([&](const auto &v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, bool>) out.Bool(v);
        else if constexpr (std::is_same_v<T, int>) out.Int(v);
        else if constexpr (std::is_same_v<T, double>) WriteNumber(out, v);
        else if constexpr (std::is_same_v<T, std::string>) out.String(v.c_str());
        else {
            out.StartArray();
            for (const auto &entry : v) {
                if constexpr (std::is_same_v<T, Actor::BoolArray>) out.Bool(entry);
                else if constexpr (std::is_same_v<T, Actor::IntArray>) out.Int(entry);
                else if constexpr (std::is_same_v<T, Actor::DoubleArray>) WriteNumber(out, entry);
                else out.String(entry.c_str());
            }
            out.EndArray();
        }
    }, value);
}

struct CellQueryResult {
    Actor::UID uid;
    std::string key;
    std::string layer;
    std::uint64_t revision;
    struct Cell { int x, y; std::optional<int> tile; };
    std::vector<Cell> cells;
};

std::vector<CellQueryResult> ReadTileQueries(const rapidjson::Value &queries) {
    if (!queries.IsArray() || queries.Size() > 16)
        throw std::runtime_error("tilemaps must contain at most 16 queries");
    std::size_t total_cells = 0;
    std::vector<CellQueryResult> result;
    for (const auto &query : queries.GetArray()) {
        if (!query.IsObject() || !query.HasMember("actor_uid") ||
            !query.HasMember("component_key") || !query["component_key"].IsString() ||
            !query.HasMember("layer") || !query["layer"].IsString() ||
            !query.HasMember("cells") || !query["cells"].IsArray())
            throw std::runtime_error("tile query requires actor_uid, component_key, layer and cells");
        Actor::UID uid = 0;
        const auto &identity = query["actor_uid"];
        if (identity.IsUint64()) uid = identity.GetUint64();
        else if (identity.IsString()) {
            const char *start = identity.GetString(), *end = start + identity.GetStringLength();
            const auto parsed = std::from_chars(start, end, uid);
            if (parsed.ec != std::errc{} || parsed.ptr != end)
                throw std::runtime_error("actor_uid must be a decimal uint64 identifier");
        } else throw std::runtime_error("actor_uid must be a decimal string or unsigned integer");
        auto *map = ComponentManager::GetTilemap(uid, query["component_key"].GetString());
        if (!map) throw std::runtime_error("tilemap component not found");
        const std::string layer = query["layer"].GetString();
        if (layer != "ground" && layer != "solid") throw std::runtime_error("unknown tilemap layer");
        total_cells += query["cells"].Size();
        if (total_cells > 256) throw std::runtime_error("tile queries are limited to 256 cells");
        CellQueryResult entry{uid, query["component_key"].GetString(), layer, map->GetRevision(), {}};
        for (const auto &cell : query["cells"].GetArray()) {
            if (!cell.IsArray() || cell.Size() != 2 || !cell[0].IsNumber() || !cell[1].IsNumber())
                throw std::runtime_error("each cell must be [integer x, integer y]");
            const double x = cell[0].GetDouble(), y = cell[1].GetDouble();
            if (!std::isfinite(x) || !std::isfinite(y) || std::floor(x) != x || std::floor(y) != y ||
                x < std::numeric_limits<int>::min() || x > std::numeric_limits<int>::max() ||
                y < std::numeric_limits<int>::min() || y > std::numeric_limits<int>::max())
                throw std::runtime_error("cell coordinates must be int32 values");
            entry.cells.push_back({static_cast<int>(x), static_cast<int>(y), map->GetCell(layer, x, y)});
        }
        result.push_back(std::move(entry));
    }
    return result;
}

// Engine diagnostics written since the previous observation, oldest first.
// Bounded so responses stay within the worker response limit.
struct DiagnosticsCursor { std::uint64_t seen = 0; };
void WriteDiagnostics(Writer &out) {
    constexpr std::size_t kMaxRecords = 32, kMaxMessage = 512;
    auto &cursor = World::Current().State<DiagnosticsCursor>();
    const std::uint64_t written = Log::WrittenCount();
    const auto recent = Log::Recent();
    const std::uint64_t fresh = written - cursor.seen;
    const std::size_t available = static_cast<std::size_t>(std::min<std::uint64_t>(fresh, recent.size()));
    const std::size_t shown = std::min(available, kMaxRecords);
    out.Key("diagnostics"); out.StartArray();
    for (std::size_t i = recent.size() - shown; i < recent.size(); ++i) {
        const Log::Record &record = recent[i];
        out.StartObject();
        out.Key("level"); out.String(Log::LevelName(record.level));
        out.Key("category"); out.String(record.category.c_str());
        out.Key("frame"); out.Int(record.frame);
        const std::string message = record.message.size() > kMaxMessage
            ? record.message.substr(0, kMaxMessage) : record.message;
        out.Key("message"); out.String(message.c_str(), static_cast<rapidjson::SizeType>(message.size()));
        out.EndObject();
    }
    out.EndArray();
    out.Key("diagnostics_dropped"); out.Uint64(fresh - shown);
    cursor.seen = written;
}

std::string Observe(const Engine &engine, std::uint32_t seed,
                    const std::vector<CellQueryResult> *tile_queries = nullptr) {
    rapidjson::StringBuffer buffer;
    Writer out(buffer);
    out.StartObject();
    out.Key("ok"); out.Bool(true);
    out.Key("protocol"); out.Int(PocketAgentProtocol::kVersion);
    out.Key("frame"); out.Int(FrameClock::GetFrameNumber());
    out.Key("delta_time"); out.Double(FrameClock::GetDeltaTime());
    out.Key("seed"); out.Uint(seed);
    out.Key("scene"); out.String(engine.GetCurrentSceneName().c_str());
    out.Key("running"); out.Bool(engine.IsRunning());
    out.Key("actors"); out.StartArray();
    std::vector<const Actor *> actors;
    for (const auto &actor : engine.GetRuntimeActors())
        if (!actor.runtime_destroyed) actors.push_back(&actor);
    std::sort(actors.begin(), actors.end(), [](const auto *a, const auto *b) {
        return a->uid < b->uid;
    });
    for (const auto *actor : actors) {
        out.StartObject();
        // UIDs are opaque decimal strings: JSON/TypeScript numbers cannot
        // represent every uint64 exactly.
        out.Key("uid"); out.String(std::to_string(actor->uid).c_str());
        out.Key("name"); out.String(actor->actor_name.c_str());
        out.Key("parent_uid"); out.String(std::to_string(actor->parent_uid).c_str());
        float x = 0, y = 0, rotation = 0;
        if (ComponentManager::TryGetRuntimeTransformWorld(actor->uid, x, y, rotation)) {
            out.Key("position"); out.StartObject();
            out.Key("x"); WriteNumber(out, x); out.Key("y"); WriteNumber(out, y);
            out.Key("rotation"); WriteNumber(out, rotation); out.EndObject();
        }
        out.Key("components"); out.StartArray();
        auto specs = ComponentManager::GetRuntimeComponentSpecs(actor->uid);
        std::sort(specs.begin(), specs.end(), [](const auto &a, const auto &b) { return a.key < b.key; });
        for (const auto &spec : specs) {
            out.StartObject();
            out.Key("key"); out.String(spec.key.c_str());
            out.Key("type"); out.String(spec.type.c_str());
            out.Key("properties"); out.StartObject();
            auto properties = ComponentManager::GetRuntimeComponentProperties(actor->uid, spec.key);
            std::sort(properties.begin(), properties.end(), [](const auto &a, const auto &b) { return a.name < b.name; });
            for (const auto &property : properties) {
                out.Key(property.name.c_str());
                // The built-in particle emitter uses NaN for an unset optional
                // end scale; expose that documented absence as JSON null.
                const auto *number = std::get_if<double>(&property.value);
                if (spec.type == "ParticleSystem" && property.name == "end_scale" &&
                    number != nullptr && std::isnan(*number)) out.Null();
                else WriteValue(out, property.value);
            }
            out.EndObject(); out.EndObject();
        }
        out.EndArray(); out.EndObject();
    }
    out.EndArray();
    if (tile_queries) {
        out.Key("tilemaps"); out.StartArray();
        for (const auto &query : *tile_queries) {
            out.StartObject();
            out.Key("actor_uid"); out.String(std::to_string(query.uid).c_str());
            out.Key("component_key"); out.String(query.key.c_str());
            out.Key("layer"); out.String(query.layer.c_str());
            out.Key("revision"); out.Uint64(query.revision);
            out.Key("cells"); out.StartArray();
            for (const auto &cell : query.cells) {
                out.StartObject(); out.Key("x"); out.Int(cell.x); out.Key("y"); out.Int(cell.y);
                out.Key("tile"); if (cell.tile) out.Int(*cell.tile); else out.Null();
                out.EndObject();
            }
            out.EndArray(); out.EndObject();
        }
        out.EndArray();
    }
    WriteDiagnostics(out);
    out.EndObject();
    return buffer.GetString();
}

std::string Error(const std::string &message, const std::string &code = {}, bool requires_reset = false) {
    rapidjson::StringBuffer buffer;
    Writer out(buffer);
    out.StartObject(); out.Key("ok"); out.Bool(false);
    out.Key("error"); out.String(message.c_str());
    if (ScriptBudget::GetFailure() != ScriptBudget::Failure::None) {
        out.Key("requires_reset"); out.Bool(true);
        out.Key("code"); out.String(ScriptBudget::GetFailure() == ScriptBudget::Failure::HeapLimit
                                     ? "lua_heap_limit" : "lua_instruction_limit");
    } else if (!code.empty()) { out.Key("code"); out.String(code.c_str()); }
    if (requires_reset && ScriptBudget::GetFailure() == ScriptBudget::Failure::None) {
        out.Key("requires_reset"); out.Bool(true);
    }
    out.Key("frame"); out.Int(FrameClock::GetFrameNumber()); out.EndObject();
    return buffer.GetString();
}

std::string SceneLoadError(const SceneLoadFailure &failure) {
    rapidjson::StringBuffer buffer;
    Writer out(buffer);
    out.StartObject();
    out.Key("ok"); out.Bool(false);
    out.Key("code"); out.String("scene_load_failed");
    out.Key("error"); out.String(failure.what());
    out.Key("frame"); out.Int(FrameClock::GetFrameNumber());
    out.Key("requires_reset"); out.Bool(failure.requires_reset);
    out.EndObject();
    return buffer.GetString();
}

std::vector<std::string> ReadReloadTypes(const rapidjson::Value &command) {
    if (!command.HasMember("types") || !command["types"].IsArray() ||
        command["types"].Empty() || command["types"].Size() > 64)
        throw std::invalid_argument("reload_scripts types must contain 1..64 distinct component type strings");
    std::vector<std::string> result;
    std::set<std::string> unique;
    for (const auto &value : command["types"].GetArray()) {
        if (!value.IsString()) throw std::invalid_argument("reload_scripts types must contain strings");
        std::string type(value.GetString(), value.GetStringLength());
        if (type.empty() || type.find('\0') != std::string::npos || !unique.insert(type).second)
            throw std::invalid_argument("reload_scripts types must be nonempty, distinct and contain no NUL bytes");
        result.push_back(std::move(type));
    }
    return result;
}

rapidjson::Value ReloadDetails(const ScriptReloadResult &result, rapidjson::Document::AllocatorType &allocator) {
    rapidjson::Value details(rapidjson::kObjectType), types(rapidjson::kArrayType), diagnostics(rapidjson::kArrayType);
    for (const auto &type : result.types)
        types.PushBack(rapidjson::Value(type.data(), static_cast<rapidjson::SizeType>(type.size()), allocator), allocator);
    for (const auto &entry : result.diagnostics) {
        rapidjson::Value diagnostic(rapidjson::kObjectType);
        auto text = [&](const char *key, const std::string &value) {
            diagnostic.AddMember(rapidjson::Value(key, allocator),
                rapidjson::Value(value.data(), static_cast<rapidjson::SizeType>(value.size()), allocator), allocator);
        };
        text("type", entry.type); text("resource", entry.resource); text("stage", entry.stage); text("message", entry.message);
        diagnostic.AddMember("line", entry.line, allocator);
        diagnostics.PushBack(std::move(diagnostic), allocator);
    }
    details.AddMember("types", std::move(types), allocator);
    details.AddMember("instances_updated", result.instances_updated, allocator);
    details.AddMember("diagnostics", std::move(diagnostics), allocator);
    return details;
}

std::string ReloadResponse(const Engine &engine, std::uint32_t seed, const ScriptReloadResult &result) {
    rapidjson::Document response;
    if (result.ok) {
        const auto observed = Observe(engine, seed);
        response.Parse(observed.data(), observed.size());
        response.AddMember("reload", ReloadDetails(result, response.GetAllocator()), response.GetAllocator());
    } else {
        response.SetObject();
        auto &allocator = response.GetAllocator();
        response.AddMember("ok", false, allocator);
        response.AddMember("code", "script_reload_failed", allocator);
        const std::string message = result.diagnostics.empty() ? "script reload failed" : result.diagnostics.front().message;
        response.AddMember("error", rapidjson::Value(message.data(), static_cast<rapidjson::SizeType>(message.size()), allocator), allocator);
        response.AddMember("frame", FrameClock::GetFrameNumber(), allocator);
        response.AddMember("requires_reset", false, allocator);
        auto details = ReloadDetails(result, allocator);
        response.AddMember("types", std::move(details["types"]), allocator);
        response.AddMember("diagnostics", std::move(details["diagnostics"]), allocator);
    }
    rapidjson::StringBuffer buffer;
    Writer writer(buffer); response.Accept(writer);
    return buffer.GetString();
}

std::string Describe(bool runtime_available) {
    rapidjson::StringBuffer buffer;
    Writer out(buffer);
    out.StartObject();
    out.Key("ok"); out.Bool(true);
    out.Key("protocol"); out.Int(PocketAgentProtocol::kVersion);
    out.Key("commands"); out.StartArray();
    for (const auto name : PocketAgentProtocol::kCommands)
        out.String(name.data(), static_cast<rapidjson::SizeType>(name.size()));
    out.EndArray();
    out.Key("command_schema");
    out.RawValue(PocketAgentProtocol::kSchemaJson.data(), PocketAgentProtocol::kSchemaJson.size(), rapidjson::kObjectType);
    out.Key("simulation_hz"); out.Int(60);
    out.Key("step"); out.StartObject();
    out.Key("frames"); out.StartObject();
    out.Key("minimum"); out.Int(1); out.Key("maximum"); out.Int(3600);
    out.Key("default"); out.Int(1); out.EndObject();
    out.Key("keys"); out.String("complete held SDL key names; omit to retain, [] to release");
    out.Key("mouse"); out.String("optional {x,y,buttons:[1..3],wheel}; omitted position/buttons retained, wheel first tick only");
    out.EndObject();
    out.Key("reload_scripts"); out.StartObject();
    out.Key("types"); out.String("required 1..64 distinct registered non-builtin component type names");
    out.Key("state"); out.String("atomic selected-source reload at idle; keeps instances and state; does not advance a tick or rerun OnStart");
    out.Key("diagnostics"); out.String("type, resource, stage, message, line (0 when unavailable); ordinary failure keeps the runtime usable");
    out.EndObject();
    out.Key("reset"); out.String("optional uint32 seed; retained when omitted");
    out.Key("transport"); out.String("one JSON object per line; diagnostics on stderr");
    out.Key("component_property_coverage");
    out.String("engine/editor serialized scalar and homogeneous-array fields; not a full Lua type system");
    out.Key("script_api");
    out.RawValue(PocketScriptAPI::kContractJson.data(), PocketScriptAPI::kContractJson.size(), rapidjson::kObjectType);
    out.Key("component_types"); out.StartArray();
    if (runtime_available) {
        auto types = ComponentManager::GetRegisteredComponentTypes();
        std::sort(types.begin(), types.end());
        constexpr const char *kinds[] = {"boolean", "integer", "number", "string",
                                         "boolean[]", "integer[]", "number[]", "string[]"};
        for (const auto &type : types) {
            out.StartObject(); out.Key("type"); out.String(type.c_str());
            out.Key("properties"); out.StartArray();
            auto properties = ComponentManager::GetComponentTypeDefaultProperties(type);
            std::sort(properties.begin(), properties.end(), [](const auto &a, const auto &b) { return a.name < b.name; });
            for (const auto &property : properties) {
                out.StartObject(); out.Key("name"); out.String(property.name.c_str());
                out.Key("value_type"); out.String(kinds[property.value.index()]);
                out.EndObject();
            }
            out.EndArray(); out.EndObject();
        }
    }
    out.EndArray(); out.EndObject();
    return buffer.GetString();
}

std::set<SDL_Scancode> ReadKeys(const rapidjson::Value &keys) {
    if (!keys.IsArray() || keys.Size() > SDL_SCANCODE_COUNT)
        throw std::runtime_error("keys must be an array of key names");
    std::set<SDL_Scancode> result;
    for (const auto &key : keys.GetArray()) {
        if (!key.IsString()) throw std::runtime_error("key names must be strings");
        const auto code = SDL_GetScancodeFromName(key.GetString());
        if (code == SDL_SCANCODE_UNKNOWN)
            throw std::runtime_error("unknown key: " + std::string(key.GetString()));
        result.insert(code);
    }
    return result;
}

void ApplyKeys(Engine &engine, const std::set<SDL_Scancode> &before,
               const std::set<SDL_Scancode> &after) {
    auto emit = [&](SDL_Scancode code, bool down) {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        event.key.scancode = code;
        event.key.key = SDL_GetKeyFromScancode(code, SDL_KMOD_NONE, false);
        event.key.down = down;
        bool quit = false;
        engine.ProcessSDLEvent(event, quit);
    };
    for (auto code : before) if (!after.count(code)) emit(code, false);
    for (auto code : after) if (!before.count(code)) emit(code, true);
}

struct MouseAction {
    float x = 0, y = 0, wheel = 0;
    std::set<int> buttons;
};

MouseAction ReadMouse(const rapidjson::Value &value, MouseAction previous) {
    if (!value.IsObject()) throw std::runtime_error("mouse must be an object");
    previous.wheel = 0;
    auto number = [&](const char *key, float &field) {
        if (!value.HasMember(key)) return;
        if (!value[key].IsNumber() || !std::isfinite(value[key].GetDouble()) ||
            std::abs(value[key].GetDouble()) > std::numeric_limits<float>::max())
            throw std::runtime_error(std::string("mouse.") + key + " must be finite float");
        field = static_cast<float>(value[key].GetDouble());
    };
    number("x", previous.x); number("y", previous.y); number("wheel", previous.wheel);
    if (value.HasMember("buttons")) {
        if (!value["buttons"].IsArray() || value["buttons"].Size() > 3)
            throw std::runtime_error("mouse.buttons must be an array of buttons 1..3");
        previous.buttons.clear();
        for (const auto &button : value["buttons"].GetArray()) {
            if (!button.IsInt() || button.GetInt() < 1 || button.GetInt() > 3)
                throw std::runtime_error("mouse buttons must be integers in [1, 3]");
            previous.buttons.insert(button.GetInt());
        }
    }
    return previous;
}

void ApplyMouse(Engine &engine, const MouseAction &before, const MouseAction &after) {
    bool quit = false;
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = after.x; event.motion.y = after.y;
    engine.ProcessSDLEvent(event, quit);
    auto button = [&](int id, bool down) {
        SDL_Event change{};
        change.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        change.button.button = static_cast<Uint8>(id); change.button.down = down;
        change.button.x = after.x; change.button.y = after.y;
        engine.ProcessSDLEvent(change, quit);
    };
    for (int id : before.buttons) if (!after.buttons.count(id)) button(id, false);
    for (int id : after.buttons) if (!before.buttons.count(id)) button(id, true);
    if (after.wheel != 0) {
        SDL_Event wheel{};
        wheel.type = SDL_EVENT_MOUSE_WHEEL; wheel.wheel.y = after.wheel;
        engine.ProcessSDLEvent(wheel, quit);
    }
}

// run_until stop conditions. Any condition holding after a tick stops the run.
struct StopCondition {
    enum class Kind { Event, Property, Exists } kind = Kind::Event;
    std::string event;
    std::string actor, component, property, op;
    std::variant<double, std::string, bool> value;
    bool exists = true;
};

std::string RequiredString(const rapidjson::Value &object, const char *key, const char *context) {
    if (!object.HasMember(key) || !object[key].IsString() || object[key].GetStringLength() == 0)
        throw std::invalid_argument(std::string(context) + "." + key + " must be a nonempty string");
    return {object[key].GetString(), object[key].GetStringLength()};
}

std::vector<StopCondition> ReadConditions(const rapidjson::Value &command) {
    if (!command.HasMember("until") || !command["until"].IsArray() ||
        command["until"].Empty() || command["until"].Size() > 16)
        throw std::invalid_argument("run_until.until must contain 1..16 conditions");
    std::vector<StopCondition> result;
    for (const auto &entry : command["until"].GetArray()) {
        if (!entry.IsObject()) throw std::invalid_argument("each run_until condition must be an object");
        StopCondition condition;
        if (entry.HasMember("event")) {
            condition.kind = StopCondition::Kind::Event;
            condition.event = RequiredString(entry, "event", "condition");
        } else if (entry.HasMember("actor_exists")) {
            condition.kind = StopCondition::Kind::Exists;
            condition.actor = RequiredString(entry, "actor_exists", "condition");
            if (entry.HasMember("value")) {
                if (!entry["value"].IsBool()) throw std::invalid_argument("condition.value for actor_exists must be a boolean");
                condition.exists = entry["value"].GetBool();
            }
        } else if (entry.HasMember("actor")) {
            condition.kind = StopCondition::Kind::Property;
            condition.actor = RequiredString(entry, "actor", "condition");
            condition.property = RequiredString(entry, "property", "condition");
            if (entry.HasMember("component")) condition.component = RequiredString(entry, "component", "condition");
            condition.op = RequiredString(entry, "op", "condition");
            static const std::set<std::string> ops{"==", "!=", "<", "<=", ">", ">="};
            if (!ops.count(condition.op)) throw std::invalid_argument("condition.op must be one of == != < <= > >=");
            if (!entry.HasMember("value")) throw std::invalid_argument("condition.value is required");
            const auto &value = entry["value"];
            if (value.IsNumber() && std::isfinite(value.GetDouble())) condition.value = value.GetDouble();
            else if (value.IsString()) condition.value = std::string(value.GetString(), value.GetStringLength());
            else if (value.IsBool()) condition.value = value.GetBool();
            else throw std::invalid_argument("condition.value must be a finite number, string or boolean");
        } else {
            throw std::invalid_argument("each run_until condition needs event, actor or actor_exists");
        }
        result.push_back(std::move(condition));
    }
    return result;
}

// An actor named by decimal UID, else the lowest live UID with that name.
const Actor *FindActorRef(const Engine &engine, const std::string &reference) {
    Actor::UID uid = 0;
    const char *start = reference.data(), *end = start + reference.size();
    const auto parsed = std::from_chars(start, end, uid);
    const bool numeric = parsed.ec == std::errc{} && parsed.ptr == end;
    const Actor *found = nullptr;
    for (const auto &actor : engine.GetRuntimeActors()) {
        if (actor.runtime_destroyed) continue;
        if (numeric ? actor.uid == uid : actor.actor_name == reference)
            if (found == nullptr || actor.uid < found->uid) found = &actor;
    }
    return found;
}

template<class T> bool Compare(const T &left, const std::string &op, const T &right) {
    if (op == "==") return left == right;
    if (op == "!=") return left != right;
    if (op == "<") return left < right;
    if (op == "<=") return left <= right;
    if (op == ">") return left > right;
    return left >= right;
}

bool PropertyHolds(const Engine &engine, const StopCondition &condition) {
    const Actor *actor = FindActorRef(engine, condition.actor);
    if (actor == nullptr) return false;
    std::optional<Actor::ComponentPropertyValue> current;
    if (condition.component.empty()) {
        float x = 0, y = 0, rotation = 0;
        if (!ComponentManager::TryGetRuntimeTransformWorld(actor->uid, x, y, rotation)) return false;
        if (condition.property == "x") current = static_cast<double>(x);
        else if (condition.property == "y") current = static_cast<double>(y);
        else if (condition.property == "rotation") current = static_cast<double>(rotation);
    } else {
        for (auto &property : ComponentManager::GetRuntimeComponentProperties(actor->uid, condition.component))
            if (property.name == condition.property) { current = std::move(property.value); break; }
    }
    if (!current) return false;
    if (const auto *expected = std::get_if<double>(&condition.value)) {
        if (const auto *number = std::get_if<double>(&*current)) return Compare(*number, condition.op, *expected);
        if (const auto *integer = std::get_if<int>(&*current)) return Compare(static_cast<double>(*integer), condition.op, *expected);
        return false;
    }
    if (const auto *expected = std::get_if<std::string>(&condition.value)) {
        const auto *text = std::get_if<std::string>(&*current);
        return text != nullptr && Compare(*text, condition.op, *expected);
    }
    const auto *flag = std::get_if<bool>(&*current);
    const bool expected = std::get<bool>(condition.value);
    return flag != nullptr && (condition.op == "==" ? *flag == expected : condition.op == "!=" ? *flag != expected : false);
}

std::string WithRunResult(const std::string &observation, const char *reason, int frames,
                          std::optional<std::size_t> matched, const EventBus::PublishedEvent *event) {
    rapidjson::Document response;
    response.Parse(observation.data(), observation.size());
    auto &allocator = response.GetAllocator();
    rapidjson::Value result(rapidjson::kObjectType);
    result.AddMember("reason", rapidjson::StringRef(reason), allocator);
    result.AddMember("frames", frames, allocator);
    if (matched) result.AddMember("condition", static_cast<std::uint64_t>(*matched), allocator);
    else result.AddMember("condition", rapidjson::Value(rapidjson::kNullType), allocator);
    if (event) {
        rapidjson::Value matched_event(rapidjson::kObjectType);
        matched_event.AddMember("type", rapidjson::Value(event->type.c_str(), allocator), allocator);
        matched_event.AddMember("frame", event->frame, allocator);
        result.AddMember("event", std::move(matched_event), allocator);
    }
    response.AddMember("run_until", std::move(result), allocator);
    rapidjson::StringBuffer buffer;
    Writer writer(buffer); response.Accept(writer);
    return buffer.GetString();
}

struct RunSummary {
    const char *reason;
    int frames;
    std::optional<std::size_t> condition;
    std::optional<EventBus::PublishedEvent> event;
};

struct PublishTracing {
    PublishTracing() { EventBus::SetPublishTracing(true); EventBus::TakePublishedEvents(); }
    ~PublishTracing() { EventBus::SetPublishTracing(false); }
};

void EmitResponse(std::ostream &output, const std::string &json, const std::string &request_id = {}) {
    rapidjson::Document response;
    response.Parse(json.data(), json.size());
    if (response.HasParseError() || !response.IsObject())
        throw std::runtime_error("internal protocol serialization failure");
    if (!request_id.empty()) response.AddMember("_pocket_request_id",
        rapidjson::Value(request_id.c_str(), response.GetAllocator()), response.GetAllocator());
    rapidjson::StringBuffer buffer;
    Writer writer(buffer); response.Accept(writer);
    output << buffer.GetString() << std::endl;
}

std::string BootstrapResponse(const Engine &engine, std::uint32_t seed) {
    rapidjson::Document description, observation;
    const auto description_json = Describe(true);
    const auto observation_json = Observe(engine, seed);
    description.Parse(description_json.c_str()); observation.Parse(observation_json.c_str());
    rapidjson::Value snapshot;
    snapshot.CopyFrom(observation, description.GetAllocator());
    description.AddMember("_initial_observation", snapshot.Move(), description.GetAllocator());
    rapidjson::StringBuffer buffer;
    Writer writer(buffer); description.Accept(writer);
    return buffer.GetString();
}

struct DiagnosticRouting {
    std::streambuf *original = std::cout.rdbuf(std::cerr.rdbuf());
    ~DiagnosticRouting() { std::cout.rdbuf(original); }
};
}

int RunAgentSession(const std::filesystem::path &project, std::uint32_t seed) {
    std::ostream responses(std::cout.rdbuf());
    DiagnosticRouting diagnostics;
    ResourcePath::SetResourcesRootPath(project);
    ComponentManager::RouteScriptPrintToStderr();
    std::unique_ptr<Engine> engine;
    std::set<SDL_Scancode> held;
    MouseAction mouse;
    auto startup_request = std::make_unique<ScriptBudget::RequestScope>();
    auto reset = [&] {
        engine.reset();
        held.clear();
        mouse = {};
        ComponentManager::SetRandomSeed(seed);
        World::Current().audio_output_enabled = false;
        auto candidate = std::make_unique<Engine>();
        candidate->InitializeHeadless();
        if (!candidate->GetLastSceneLoadError().empty())
            throw std::runtime_error(candidate->GetLastSceneLoadError());
        if (ComponentManager::GetComponentTypeWarningCount() != 0)
            throw std::runtime_error(ComponentManager::GetLastComponentTypeWarning());
        if (ComponentManager::GetRuntimeErrorCount() != 0)
            throw std::runtime_error(ComponentManager::GetLastRuntimeError());
        engine = std::move(candidate);
    };
    try {
        reset();
        if (ScriptBudget::GetFailure() != ScriptBudget::Failure::None)
            throw std::runtime_error(ScriptBudget::FailureMessage());
    }
    catch (const std::exception &e) { EmitResponse(responses, Error(e.what())); return 1; }

    std::string line;
    while (std::getline(std::cin, line)) {
        std::string request_id;
        try {
            ScriptBudget::RequestScope request_scope;
            // Public host enforces 64 KiB before adding its private correlation token.
            if (line.size() > 65536 + 256) throw std::runtime_error("internal command exceeds its size limit");
            rapidjson::Document command;
            command.Parse(line.data(), line.size());
            if (command.HasParseError() || !command.IsObject() ||
                !command.HasMember("cmd") || !command["cmd"].IsString())
                throw std::runtime_error("expected a JSON object with string cmd");
            if (command.HasMember("_pocket_request_id")) {
                if (!command["_pocket_request_id"].IsString())
                    throw std::runtime_error("invalid internal correlation field");
                request_id = command["_pocket_request_id"].GetString();
            }
            const std::string name = command["cmd"].GetString();
            if (name == "_bootstrap") {
                if (!startup_request) throw std::runtime_error("worker already bootstrapped");
                const auto initial = BootstrapResponse(*engine, seed);
                if (ScriptBudget::GetFailure() != ScriptBudget::Failure::None)
                    throw std::runtime_error(ScriptBudget::FailureMessage());
                EmitResponse(responses, initial, request_id);
                startup_request.reset();
                continue;
            }
            if (startup_request) throw std::runtime_error("internal worker bootstrap required");
            if (name == "quit") {
                if (engine) engine->RequestQuit();
                EmitResponse(responses, "{\"ok\":true,\"protocol\":" + std::to_string(PocketAgentProtocol::kVersion) +
                             ",\"running\":false,\"frame\":" +
                             std::to_string(FrameClock::GetFrameNumber()) + "}", request_id);
                break;
            }
            if (name == "describe") {
                const auto description = Describe(engine != nullptr);
                if (ScriptBudget::GetFailure() != ScriptBudget::Failure::None)
                    throw std::runtime_error(ScriptBudget::FailureMessage());
                EmitResponse(responses, description, request_id);
                continue;
            }
            if (!engine && name != "reset")
                throw std::runtime_error("runtime unavailable after failed reset; reset required");
            if (name == "reload_scripts") {
                std::vector<std::string> types;
                try { types = ReadReloadTypes(command); }
                catch (const std::invalid_argument &error) {
                    EmitResponse(responses, Error(error.what(), "invalid_argument"), request_id);
                    continue;
                }
                ScriptReloadResult result;
                try { result = ComponentManager::ReloadScripts(types); }
                catch (const std::exception &) {
                    // Unexpected exceptions may occur after commit started. Do
                    // not present this as an ordinary rejected candidate or
                    // continue using a world whose rollback is not guaranteed.
                    const std::string failure = "{\"ok\":false,\"code\":\"script_reload_host_failure\","
                        "\"requires_reset\":true,\"error\":\"Unexpected host failure during script reload; runtime reset required; rollback not guaranteed\","
                        "\"frame\":" + std::to_string(FrameClock::GetFrameNumber()) + "}";
                    EmitResponse(responses, failure, request_id);
                    return 1;
                }
                if (ScriptBudget::GetFailure() != ScriptBudget::Failure::None)
                    throw std::runtime_error(ScriptBudget::FailureMessage());
                EmitResponse(responses, ReloadResponse(*engine, seed, result), request_id);
                continue;
            }
            std::optional<RunSummary> run_summary;
            if (name == "reset") {
                if (command.HasMember("seed")) {
                    if (!command["seed"].IsUint()) throw std::runtime_error("seed must be uint32");
                    seed = command["seed"].GetUint();
                }
                reset();
            } else if (name == "step" || name == "run_until") {
                const bool run_until = name == "run_until";
                int frames = 1;
                std::vector<StopCondition> conditions;
                if (run_until) {
                    try {
                        if (!command.HasMember("max_frames") || !command["max_frames"].IsInt() ||
                            command["max_frames"].GetInt() < 1 || command["max_frames"].GetInt() > 36000)
                            throw std::invalid_argument("run_until.max_frames must be an integer in [1, 36000]");
                        frames = command["max_frames"].GetInt();
                        conditions = ReadConditions(command);
                    } catch (const std::invalid_argument &error) {
                        EmitResponse(responses, Error(error.what(), "invalid_argument"), request_id);
                        continue;
                    }
                } else {
                    if (command.HasMember("frames")) {
                        if (!command["frames"].IsInt()) throw std::runtime_error("frames must be an integer");
                        frames = command["frames"].GetInt();
                    }
                    if (frames < 1 || frames > 3600) throw std::runtime_error("frames must be in [1, 3600]");
                }
                auto next = command.HasMember("keys") ? ReadKeys(command["keys"]) : held;
                auto next_mouse = mouse;
                next_mouse.wheel = 0;
                if (command.HasMember("mouse")) next_mouse = ReadMouse(command["mouse"], mouse);
                if (!engine->IsRunning()) throw std::runtime_error("runtime stopped; reset required");
                ApplyKeys(*engine, held, next); held = std::move(next);
                ApplyMouse(*engine, mouse, next_mouse); mouse = std::move(next_mouse);
                std::optional<PublishTracing> tracing;
                if (run_until) tracing.emplace();
                const int previous_errors = ComponentManager::GetRuntimeErrorCount();
                int advanced = 0;
                std::optional<std::size_t> matched;
                std::optional<EventBus::PublishedEvent> matched_event;
                for (int i = 0; i < frames && engine->IsRunning() && !matched; ++i) {
                    const auto scene_attempt = engine->GetSceneLoadAttemptRevision();
                    engine->StepSimulation();
                    ++advanced;
                    if (engine->RequiresSceneReset() && !engine->GetLastSceneEditError().empty())
                        throw std::runtime_error(engine->GetLastSceneEditError());
                    if (engine->RequiresSceneReset() && !engine->GetLastRuntimeActorError().empty())
                        throw std::runtime_error(engine->GetLastRuntimeActorError());
                    if (ScriptBudget::GetFailure() != ScriptBudget::Failure::None)
                        throw std::runtime_error(ScriptBudget::FailureMessage());
                    if (engine->GetSceneLoadAttemptRevision() != scene_attempt &&
                        (!engine->GetLastSceneLoadError().empty() || engine->RequiresSceneReset()))
                        throw SceneLoadFailure(engine->GetLastSceneLoadError(), engine->RequiresSceneReset());
                    if (ComponentManager::GetRuntimeErrorCount() != previous_errors)
                        throw std::runtime_error(ComponentManager::GetLastRuntimeError());
                    if (!run_until) continue;
                    const auto events = EventBus::TakePublishedEvents();
                    for (std::size_t c = 0; c < conditions.size() && !matched; ++c) {
                        const auto &condition = conditions[c];
                        if (condition.kind == StopCondition::Kind::Event) {
                            for (const auto &event : events)
                                if (event.type == condition.event) { matched = c; matched_event = event; break; }
                        } else if (condition.kind == StopCondition::Kind::Exists) {
                            if ((FindActorRef(*engine, condition.actor) != nullptr) == condition.exists) matched = c;
                        } else if (PropertyHolds(*engine, condition)) {
                            matched = c;
                        }
                    }
                }
                if (run_until)
                    run_summary = RunSummary{matched ? "condition" : engine->IsRunning() ? "max_frames" : "stopped",
                                             advanced, matched, matched_event};
            } else if (name != "observe") {
                throw std::runtime_error("unknown command: " + name);
            }
            if (ScriptBudget::GetFailure() != ScriptBudget::Failure::None)
                throw std::runtime_error(ScriptBudget::FailureMessage());
            if (engine->RequiresSceneReset() && !engine->GetLastSceneEditError().empty())
                throw std::runtime_error(engine->GetLastSceneEditError());
            if (engine->RequiresSceneReset() && !engine->GetLastRuntimeActorError().empty())
                throw std::runtime_error(engine->GetLastRuntimeActorError());
            if (name == "observe" && command.HasMember("tilemaps")) {
                const auto queries = ReadTileQueries(command["tilemaps"]);
                EmitResponse(responses, Observe(*engine, seed, &queries), request_id);
            } else if (run_summary) {
                EmitResponse(responses, WithRunResult(Observe(*engine, seed), run_summary->reason, run_summary->frames,
                                                      run_summary->condition,
                                                      run_summary->event ? &*run_summary->event : nullptr), request_id);
            } else EmitResponse(responses, Observe(*engine, seed), request_id);
        } catch (const SceneLoadFailure &failure) {
            EmitResponse(responses, SceneLoadError(failure), request_id);
            if (failure.requires_reset) return 1;
        } catch (const std::exception &e) {
            if (engine && engine->RequiresSceneReset()) {
                if (!engine->GetLastSceneEditError().empty()) {
                    EmitResponse(responses, Error(engine->GetLastSceneEditError(), "scene_edit_failed", true), request_id);
                    return 1;
                }
                if (!engine->GetLastRuntimeActorError().empty()) {
                    EmitResponse(responses, Error(engine->GetLastRuntimeActorError(), "actor_publish_failed", true), request_id);
                    return 1;
                }
                const SceneLoadFailure failure(engine->GetLastSceneLoadError().empty() ? e.what() : engine->GetLastSceneLoadError(), true);
                EmitResponse(responses, SceneLoadError(failure), request_id);
                return 1;
            }
            EmitResponse(responses, Error(e.what()), request_id);
        }
    }
    return 0;
}
