#ifndef COMPONENT_MANAGER_H
#define COMPONENT_MANAGER_H

#include "scene/Actor.h"
#include "scene/ActorHandle.h"
#include "core/Vector2.h"
#include <list>
#include <filesystem>
#include <memory>
#include <vector>

class Engine;
struct lua_State;
class Tilemap;
class SpriteAnimation;
class SpriteRenderer;

// Owner-thread scene preparation. Destroy before Lua shutdown, and after every
// prepared instance/ref that relies on its GC pause has been released.
class SceneScriptPlan {
public:
    ~SceneScriptPlan();
    SceneScriptPlan(SceneScriptPlan &&) noexcept;
    SceneScriptPlan &operator=(SceneScriptPlan &&) noexcept;
    SceneScriptPlan(const SceneScriptPlan &) = delete;
    SceneScriptPlan &operator=(const SceneScriptPlan &) = delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit SceneScriptPlan(lua_State *state);
    friend class ComponentManager;
};

struct ScriptReloadDiagnostic {
    std::string type, resource, stage, message;
    int line = 0;
};
struct ScriptReloadResult {
    bool ok = false;
    std::vector<std::string> types;
    int instances_updated = 0;
    std::vector<ScriptReloadDiagnostic> diagnostics;
};

class ComponentManager {
public:
    // Create the Lua VM and register APIs; project scripts bootstrap separately.
    static void Initialize();
    // Once per VM, execute selected project files as trusted full Lua.
    static void BootstrapComponentTypes(const std::filesystem::path &preferred);
    // Previously loaded canonical sources are not reread when switching back
    // to an earlier variant. Unselected/deleted modules retain their in-memory
    // lifetime until Shutdown; explicit ReloadScripts handles edits. Only
    // never-loaded selected paths use isolated staging.
    static std::unique_ptr<SceneScriptPlan> PrepareSceneScripts(
        const std::filesystem::path &preferred, const std::vector<Actor> &incoming,
        const std::vector<Actor::UID> &persistent_uids, std::string &error);
    static luabridge::LuaRef GetPreparedSceneType(const SceneScriptPlan &plan,
                                                const std::string &type);
    static void CommitSceneScripts(SceneScriptPlan &plan);
    // Applied before component scripts are loaded, and again on explicit resets.
    static void SetRandomSeed(std::uint32_t seed);
    static void RouteScriptPrintToStderr();
    // Explicit owner-thread, idle-boundary hot reload. Ordinary candidate errors
    // preserve the running world; this does not restart scene/lifecycle state.
    static ScriptReloadResult ReloadScripts(const std::vector<std::string> &types);
    static std::string GetComponentScriptSource(const std::string &type);

    static void Shutdown();

    // clear all runtime component state tied to scene actors
    // 清空与场景 actor 绑定的组件运行时状态
    static void ClearActorComponents();

    // rebind actor pointers after a scene container changes
    // 在场景 actor 容器变化后重新绑定 actor 指针
    static void BindActorsForScene(std::list<Actor> &actors);
    static Actor *ResolveActorHandle(Actor::UID uid, std::uint64_t generation);
    static void BindEngine(Engine *engine);

    // instantiate one component from parsed scene / template spec
    // 根据解析后的场景或模板数据实例化一个组件
    static void InstantiateComponentForActor(Actor::UID actor_uid,
                                             const Actor::ComponentSpec &component_spec);

    // runtime component add / remove
    // 运行时组件增删接口
    static luabridge::LuaRef AddComponent(Actor::UID actor_uid,
                                          const std::string &type_name);
    static void RemoveComponent(Actor::UID actor_uid,
                                luabridge::LuaRef component_ref);
    static bool RenameComponentKey(Actor::UID actor_uid,
                                   const std::string &old_key,
                                   const std::string &new_key);
    static bool SetComponentPropertyValue(
        Actor::UID actor_uid, const std::string &component_key,
        const std::string &property_name,
        const Actor::ComponentPropertyValue &value);

    // per-frame lifecycle entry points
    // 每帧生命周期入口
    static void ProcessPendingOnStart();
    // OnAwake runs once per component as soon as it is published: before
    // Actor.Instantiate / AddComponent return, and for scene components before
    // any OnStart, so the spawner can read initialized fields immediately.
    static void ProcessPendingOnAwake();
    static void ApplyEffectiveRigidbodyBodyTypes();
    static void ProcessOnUpdate();
    static void ProcessOnLateUpdate();
    static void FinalizePrePhysicsDestructions();
    static void StepPhysics();
    static void FinalizeFrameMutations();

    // immediate physics event dispatch helpers. A destroyed receiver gets no
    // callback; a destroyed `other` is still reported, with an invalid handle.
    // 立即派发物理事件的辅助接口
    static void QueueCollisionEvent(const ActorHandle &actor, const ActorHandle &other,
                                    const Vector2 &point,
                                    const Vector2 &relative_velocity,
                                    const Vector2 &normal, bool is_enter);
    static void QueueTriggerEvent(const ActorHandle &actor, const ActorHandle &other,
                                  const Vector2 &point,
                                  const Vector2 &relative_velocity,
                                  const Vector2 &normal, bool is_enter);

    // component queries used by Actor's Lua-facing methods
    // 给 Actor 的 Lua 接口使用的组件查询
    static luabridge::LuaRef GetComponentByKey(Actor::UID actor_uid,
                                               const std::string &key);
    static luabridge::LuaRef GetComponentByType(Actor::UID actor_uid,
                                                const std::string &type_name);
    static luabridge::LuaRef GetComponentsByType(Actor::UID actor_uid,
                                                 const std::string &type_name);
    static int GetChildCount(Actor::UID actor_uid);
    static luabridge::LuaRef GetChildren(Actor::UID actor_uid);
    static luabridge::LuaRef GetComponentInChildrenByType(
        Actor::UID actor_uid, const std::string &type_name);
    static luabridge::LuaRef GetComponentsInChildrenByType(
        Actor::UID actor_uid, const std::string &type_name);

    // editor-facing metadata queries for available component types/defaults
    // 给编辑器使用的组件元数据查询接口
    static std::vector<std::string> GetRegisteredComponentTypes();
    static bool IsRegisteredComponentType(const std::string &type_name);
    static int GetComponentTypeWarningCount();
    static const std::string &GetLastComponentTypeWarning();
    static int GetRuntimeErrorCount();
    static const std::string &GetLastRuntimeError();
    static void ClearRuntimeErrors();
    static std::vector<Actor::ComponentProperty>
    GetComponentTypeDefaultProperties(const std::string &type_name);
    static std::vector<Actor::ComponentSpec>
    GetRuntimeComponentSpecs(Actor::UID actor_uid);
    static std::vector<Actor::ComponentProperty>
    GetRuntimeComponentProperties(Actor::UID actor_uid,
                                  const std::string &component_key);
    static bool SetRuntimeComponentPropertyValue(
        Actor::UID actor_uid, const std::string &component_key,
        const std::string &property_name,
        const Actor::ComponentPropertyValue &value);
    static void QueueBuiltinRenderers(bool scene_backed_only = false);
    static void ResolveTransformHierarchy();
    // Tile assets are validated before replacing the live scene; all cell and
    // physics mutations commit at the owner-thread pre-physics boundary.
    static void ValidateSpriteAnimationActor(const Actor &actor);
    static void PrepareSpriteAnimationAssets(const std::vector<Actor> &actors);
    static void ClearPreparedSpriteAnimationAssets();
    static void InitializeSpriteAnimations(Actor::UID actor_uid);
    static SpriteAnimation *GetSpriteAnimation(Actor::UID actor_uid, const std::string &key);
    static SpriteRenderer *GetSpriteRenderer(Actor::UID actor_uid, const std::string &key);
    // Calls the Lua component's OnAgentObserve(self) in a protected call and
    // stores its result as bounded JSON (or "null" with the Lua error).
    // Returns false when the component has no such method.
    static bool CallAgentObserve(Actor::UID actor_uid, const std::string &key, std::string &json, std::string &error);
    // The native object of the built-in component at (uid, key), or nullptr.
    // Built-ins compare it with `this` to check that they are still bound.
    static const void *FindBuiltinComponent(Actor::UID actor_uid, const std::string &key);
    static void ProcessSpriteAnimations();
    static void ApplySpriteAnimationPoses();
    static bool SetSpriteAnimationProperty(Actor::UID actor_uid, const std::string &key,
                                          const std::string &name, const Actor::ComponentPropertyValue &value);
    static void PrepareTilemapAssets(const std::vector<Actor> &actors);
    static void ClearPreparedTilemapAssets();
    static void InitializeTilemap(Actor::UID actor_uid, const std::string &key);
    static Tilemap *GetTilemap(Actor::UID actor_uid, const std::string &key);
    static void CommitTilemaps();
    static void QueueTilemaps(float min_x, float min_y, float max_x, float max_y,
                              bool scene_backed_only = false);
    // Whether any live tilemap has a solid cell at the world point.
    static bool IsSolidTileAt(float x, float y);
    static bool SetTilemapResource(Actor::UID actor_uid, const std::string &key,
                                  const std::string &resource);
    static bool TryGetRuntimeTransformWorld(Actor::UID actor_uid,
                                            float &x, float &y,
                                            float &rotation,
                                            std::string *out_component_key = nullptr);
    static bool SetRuntimeTransformWorldPosition(Actor::UID actor_uid,
                                                 float world_x, float world_y);

    // actor-level helpers exposed through Actor static APIs
    // 通过 Actor 静态接口暴露出去的 actor 级辅助函数
    static luabridge::LuaRef InstantiateActor(const std::string &template_name);
    static void DestroyActor(Actor *actor);
    static bool SetActorParent(Actor::UID actor_uid, Actor *parent_actor);
    static luabridge::LuaRef FindActorByName(const std::string &name);
    static luabridge::LuaRef FindAllActorsByName(const std::string &name);
};

#endif
