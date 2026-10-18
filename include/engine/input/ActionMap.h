#ifndef POCKET2D_ACTION_MAP_H
#define POCKET2D_ACTION_MAP_H

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Semantic gameplay actions declared in a project's optional actions.json.
// Human input reaches them through key and mouse bindings; agents set them
// directly. Gameplay reads actions instead of keys, so rebinding never
// changes agent tests. State is per World.
//
// actions.json: {"version":1,"actions":{
//   "attack":{"type":"button","keys":["space"],"mouse":[1]},
//   "zoom":{"type":"axis","negative":["q"],"positive":["e"]},
//   "move":{"type":"axis2d","left":["a"],"right":["d"],"up":["w"],"down":["s"]}}}
// Screen y grows downward, so "up" is y = -1.
namespace ActionMap {

enum class Type { Button, Axis, Axis2D };

struct Value {
    float x = 0.0f, y = 0.0f;
};

struct Declaration {
    std::string name;
    Type type = Type::Button;
};

// Reads <project>/actions.json, replacing this World's actions and clearing
// agent values. A missing file declares no actions; an invalid one throws.
void Load(const std::filesystem::path &project);
std::vector<Declaration> Declarations();
std::optional<Type> Find(const std::string &name);
const char *TypeName(Type type);

// The complete set of agent-held action values; actions not listed are
// released. Each value is clamped like bound input.
void SetAgentValues(const std::map<std::string, Value> &values);

// Bound input combined with agent values: buttons are held when either holds,
// axes sum and clamp to [-1, 1], 2D axes sum and clamp to unit length.
// Unknown names throw std::invalid_argument.
Value GetValue(const std::string &name);
bool IsHeld(const std::string &name);
bool WasPressed(const std::string &name);  // held now, not at the previous frame end
bool WasReleased(const std::string &name); // held at the previous frame end, not now

// Records held state for the next frame's edges; called at frame end.
void EndFrame();

// Netplay: while set, every action reads only these values (absent = released);
// keys, mouse and agent values no longer reach gameplay. nullptr clears it.
void SetNetValues(const std::map<std::string, Value> *values);
bool HasNetValues();
// Bound input and agent values, ignoring netplay: the local player's input.
Value SampleLocal(const std::string &name);

} // namespace ActionMap

#endif
