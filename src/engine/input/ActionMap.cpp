#include "input/ActionMap.h"
#include "core/World.h"
#include "input/Input.h"
#include "rapidjson/document.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ActionMap {
namespace {
struct Action {
    Type type = Type::Button;
    // Button: keys, mouse. Axis: negative, positive. Axis2D: left, right, up, down.
    std::vector<std::string> keys, negative, positive, left, right, up, down;
    std::vector<int> mouse;
};

struct State {
    std::map<std::string, Action> actions;
    std::map<std::string, Value> agent;
    std::map<std::string, bool> held_at_frame_end;
};
State &Current() { return World::Current().State<State>(); }

std::vector<std::string> KeyList(const rapidjson::Value &action, const char *field, const std::string &name) {
    std::vector<std::string> keys;
    if (!action.HasMember(field)) return keys;
    const auto &values = action[field];
    if (!values.IsArray() || values.Size() > 16)
        throw std::runtime_error("actions.json: " + name + "." + field + " must be an array of at most 16 key names");
    for (const auto &value : values.GetArray()) {
        if (!value.IsString() || !Input::IsKnownKeyName(value.GetString()))
            throw std::runtime_error("actions.json: " + name + "." + field + " has an unknown key name");
        keys.emplace_back(value.GetString(), value.GetStringLength());
    }
    return keys;
}

bool AnyHeld(const std::vector<std::string> &keys) {
    return std::any_of(keys.begin(), keys.end(), [](const std::string &key) { return Input::GetKey(key); });
}

const Action &Require(const std::string &name) {
    const auto found = Current().actions.find(name);
    if (found == Current().actions.end()) throw std::invalid_argument("unknown action: " + name);
    return found->second;
}

Value Clamp(Type type, Value value) {
    if (type == Type::Button) return {value.x != 0.0f ? 1.0f : 0.0f, 0.0f};
    if (type == Type::Axis) return {std::clamp(value.x, -1.0f, 1.0f), 0.0f};
    const float length = std::hypot(value.x, value.y);
    if (length > 1.0f) { value.x /= length; value.y /= length; }
    return value;
}

bool Holds(Type type, const Value &value) {
    return type == Type::Axis2D ? (value.x != 0.0f || value.y != 0.0f) : value.x != 0.0f;
}
} // namespace

const char *TypeName(Type type) {
    switch (type) {
    case Type::Button: return "button";
    case Type::Axis: return "axis";
    case Type::Axis2D: return "axis2d";
    }
    return "button";
}

void Load(const std::filesystem::path &project) {
    State next;
    const auto path = project / "actions.json";
    if (std::filesystem::exists(path)) {
        std::ifstream file(path, std::ios::binary);
        std::stringstream text;
        text << file.rdbuf();
        rapidjson::Document document;
        const std::string contents = text.str();
        document.Parse(contents.data(), contents.size());
        if (document.HasParseError() || !document.IsObject() || !document.HasMember("version") ||
            !document["version"].IsInt() || document["version"].GetInt() != 1 ||
            !document.HasMember("actions") || !document["actions"].IsObject())
            throw std::runtime_error("actions.json must be {\"version\":1,\"actions\":{...}}");
        for (const auto &member : document["actions"].GetObject()) {
            const std::string name(member.name.GetString(), member.name.GetStringLength());
            const auto &value = member.value;
            if (name.empty() || name.size() > 64 || !value.IsObject() || !value.HasMember("type") || !value["type"].IsString())
                throw std::runtime_error("actions.json: action '" + name + "' needs a name of 1..64 bytes and a type");
            const std::string type = value["type"].GetString();
            Action action;
            if (type == "button") {
                action.type = Type::Button;
                action.keys = KeyList(value, "keys", name);
                if (value.HasMember("mouse")) {
                    if (!value["mouse"].IsArray() || value["mouse"].Size() > 3)
                        throw std::runtime_error("actions.json: " + name + ".mouse must list buttons 1..3");
                    for (const auto &button : value["mouse"].GetArray()) {
                        if (!button.IsInt() || button.GetInt() < 1 || button.GetInt() > 3)
                            throw std::runtime_error("actions.json: " + name + ".mouse must list buttons 1..3");
                        action.mouse.push_back(button.GetInt());
                    }
                }
            } else if (type == "axis") {
                action.type = Type::Axis;
                action.negative = KeyList(value, "negative", name);
                action.positive = KeyList(value, "positive", name);
            } else if (type == "axis2d") {
                action.type = Type::Axis2D;
                action.left = KeyList(value, "left", name);
                action.right = KeyList(value, "right", name);
                action.up = KeyList(value, "up", name);
                action.down = KeyList(value, "down", name);
            } else {
                throw std::runtime_error("actions.json: action '" + name + "' type must be button, axis or axis2d");
            }
            next.actions.emplace(name, std::move(action));
        }
    }
    Current() = std::move(next);
}

std::vector<Declaration> Declarations() {
    std::vector<Declaration> result;
    for (const auto &[name, action] : Current().actions) result.push_back({name, action.type});
    return result;
}

std::optional<Type> Find(const std::string &name) {
    const auto found = Current().actions.find(name);
    if (found == Current().actions.end()) return std::nullopt;
    return found->second.type;
}

void SetAgentValues(const std::map<std::string, Value> &values) {
    std::map<std::string, Value> next;
    for (const auto &[name, value] : values) next[name] = Clamp(Require(name).type, value);
    Current().agent = std::move(next);
}

Value GetValue(const std::string &name) {
    const Action &action = Require(name);
    Value bound;
    switch (action.type) {
    case Type::Button:
        bound.x = AnyHeld(action.keys) ||
                  std::any_of(action.mouse.begin(), action.mouse.end(), [](int button) { return Input::GetMouseButton(button); })
                  ? 1.0f : 0.0f;
        break;
    case Type::Axis:
        bound.x = (AnyHeld(action.positive) ? 1.0f : 0.0f) - (AnyHeld(action.negative) ? 1.0f : 0.0f);
        break;
    case Type::Axis2D:
        bound.x = (AnyHeld(action.right) ? 1.0f : 0.0f) - (AnyHeld(action.left) ? 1.0f : 0.0f);
        bound.y = (AnyHeld(action.down) ? 1.0f : 0.0f) - (AnyHeld(action.up) ? 1.0f : 0.0f);
        break;
    }
    const auto agent = Current().agent.find(name);
    if (agent != Current().agent.end()) {
        if (action.type == Type::Button) bound.x = std::max(bound.x, agent->second.x);
        else { bound.x += agent->second.x; bound.y += agent->second.y; }
    }
    return Clamp(action.type, bound);
}

bool IsHeld(const std::string &name) { return Holds(Require(name).type, GetValue(name)); }

bool WasPressed(const std::string &name) {
    const bool held = IsHeld(name);
    const auto previous = Current().held_at_frame_end.find(name);
    return held && (previous == Current().held_at_frame_end.end() || !previous->second);
}

bool WasReleased(const std::string &name) {
    const bool held = IsHeld(name);
    const auto previous = Current().held_at_frame_end.find(name);
    return !held && previous != Current().held_at_frame_end.end() && previous->second;
}

void EndFrame() {
    auto &state = Current();
    for (const auto &[name, action] : state.actions) state.held_at_frame_end[name] = Holds(action.type, GetValue(name));
}

} // namespace ActionMap
