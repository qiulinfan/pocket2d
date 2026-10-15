#ifndef POCKET2D_SCENE_COMPONENT_PLAN_H
#define POCKET2D_SCENE_COMPONENT_PLAN_H

#include "scene/Actor.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <list>
#include <memory>
#include <string>
#include <vector>

class SceneScriptPlan;
// Main-VM instances prepared without callbacks or actor registration. Destroy
// before the associated SceneScriptPlan releases its GC pause, and before VM shutdown.
class SceneComponentPlan {
public:
    ~SceneComponentPlan();
    SceneComponentPlan(SceneComponentPlan &&) noexcept;
    SceneComponentPlan &operator=(SceneComponentPlan &&) noexcept;
    SceneComponentPlan(const SceneComponentPlan &) = delete;
    SceneComponentPlan &operator=(const SceneComponentPlan &) = delete;
private:
    SceneComponentPlan();
    struct State;
    std::unique_ptr<State> state_;
    friend std::unique_ptr<SceneComponentPlan> PrepareSceneComponents(
        const std::vector<Actor> &, const SceneScriptPlan &, std::string &);
    friend void InstallSceneComponents(SceneComponentPlan &, std::list<Actor> &, std::size_t);
};
std::unique_ptr<SceneComponentPlan> PrepareSceneComponents(
    const std::vector<Actor> &actors, const SceneScriptPlan &scripts, std::string &error);
void InstallSceneComponents(SceneComponentPlan &plan, std::list<Actor> &actors, std::size_t begin);

// A single detached record for host property/type edits. This token owns a
// nested GC pause; destruction releases the record before resuming collection.
// Caller validates actor-level peer/resource constraints before deleting the old
// component. Install performs no default cloning or lifecycle-field discovery.
class PreparedRuntimeComponent {
public:
    ~PreparedRuntimeComponent();
    PreparedRuntimeComponent(PreparedRuntimeComponent &&) noexcept;
    PreparedRuntimeComponent &operator=(PreparedRuntimeComponent &&) noexcept;
    PreparedRuntimeComponent(const PreparedRuntimeComponent &) = delete;
    PreparedRuntimeComponent &operator=(const PreparedRuntimeComponent &) = delete;
private:
    struct State;
    std::unique_ptr<State> state_;
    explicit PreparedRuntimeComponent(lua_State *state);
    friend std::unique_ptr<PreparedRuntimeComponent> PrepareRuntimeComponent(const Actor::ComponentSpec &, std::string &);
    friend void InstallRuntimeComponent(PreparedRuntimeComponent &, Actor::UID);
};
std::unique_ptr<PreparedRuntimeComponent> PrepareRuntimeComponent(const Actor::ComponentSpec &spec, std::string &error);
void InstallRuntimeComponent(PreparedRuntimeComponent &plan, Actor::UID uid);

namespace ManagerDetail {
struct ComponentRecord;
// Shared with ordinary runtime AddComponent so construction has one static
// lifecycle-discovery contract. Native setters invoked here are trusted bindings.
std::unique_ptr<ComponentRecord> PrepareComponentRecord(
    const Actor::ComponentSpec &spec, const luabridge::LuaRef &prototype);
void InstallPreparedComponent(Actor::UID uid, std::unique_ptr<ComponentRecord> record);
}
#endif
