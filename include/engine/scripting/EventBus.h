#ifndef EVENTBUS_H
#define EVENTBUS_H

#include "scene/Actor.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <memory>
#include <vector>
#include "lua.hpp"
#include "scripting/LuaBoundary.h"
#include "LuaBridge/LuaBridge.h"

struct lua_State;

namespace luabridge {
class LuaRef;
}

namespace EventBus {

struct Hooks {
    // lua state used by EventBus, mainly for stack cleanup after errors
    // EventBus 使用的 lua state, 主要用于异常后的栈清理
    lua_State *lua_state = nullptr;

    // compare whether two LuaRef objects point to the same Lua object
    // 判断两个 LuaRef 是否指向同一个 Lua 对象
    bool (*are_same_ref)(const luabridge::LuaRef &,
                         const luabridge::LuaRef &) = nullptr;

    // extract a stable component identity from a component ref
    // 从组件引用中提取稳定身份, 即 actor_uid 和 component_key
    bool (*try_extract_component_identity)(const luabridge::LuaRef &,
                                           Actor::UID &,
                                           std::string &) = nullptr;

    // check whether a component ref is still alive
    // 判断组件引用当前是否仍然有效
    bool (*is_component_ref_alive)(const luabridge::LuaRef &, Actor::UID,
                                   const std::string &) = nullptr;

    // host-side error reporter for Lua callback failures
    // Lua 回调报错时交给宿主侧输出
    void (*report_error)(Actor::UID actor_uid,
                         const LuaBoundary::Error &) = nullptr;
};

struct FunctionReplacement {
    luabridge::LuaRef before;
    luabridge::LuaRef after; // nil removes this callback, including queued operations
};
struct ComponentReload {
    luabridge::LuaRef component;
    std::vector<FunctionReplacement> functions;
};

// An owned candidate snapshot. It must be destroyed before its Lua VM closes.
// Prepare and commit belong to one idle owner-thread transaction: no EventBus
// mutation or user-code dispatch may occur between these calls.
class ReloadPlan {
public:
    ReloadPlan();
    ~ReloadPlan();
    ReloadPlan(ReloadPlan &&) noexcept;
    ReloadPlan &operator=(ReloadPlan &&) noexcept;
    ReloadPlan(const ReloadPlan &) = delete;
    ReloadPlan &operator=(const ReloadPlan &) = delete;
private:
    struct State;
    std::unique_ptr<State> state_;
    friend bool PrepareReload(const std::vector<ComponentReload> &, ReloadPlan &, std::string &);
    friend void CommitReload(ReloadPlan &&) noexcept;
};

bool PrepareReload(const std::vector<ComponentReload> &components, ReloadPlan &plan, std::string &error);
void CommitReload(ReloadPlan &&plan) noexcept;
bool IsDispatching();

// Identity-only rename delta. Callback functions (including anonymous closures)
// and FIFO subscription operations are retained, including operations queued by
// teardown callbacks after preparation. Registry validation/publication belongs
// to the caller; only component identities alive at `before` may be prepared.
struct ComponentKeyRemap {
    luabridge::LuaRef component;
    Actor::UID actor_uid;
    std::string before;
    std::string after;
};
class KeyRemapPlan {
public:
    KeyRemapPlan();
    ~KeyRemapPlan();
    KeyRemapPlan(KeyRemapPlan &&) noexcept;
    KeyRemapPlan &operator=(KeyRemapPlan &&) noexcept;
    KeyRemapPlan(const KeyRemapPlan &) = delete;
    KeyRemapPlan &operator=(const KeyRemapPlan &) = delete;
private:
    struct State;
    std::unique_ptr<State> state_;
    friend bool PrepareKeyRemap(const std::vector<ComponentKeyRemap> &, KeyRemapPlan &, std::string &);
    friend void CommitKeyRemap(KeyRemapPlan &&);
};
bool PrepareKeyRemap(const std::vector<ComponentKeyRemap> &components, KeyRemapPlan &plan, std::string &error);
// Call after publishing all registry keys at an idle dispatch boundary. Does
// not invoke callbacks. String allocation failure is a post-publication failure;
// callers must use their fatal/reset path rather than claim an ordinary rollback.
void CommitKeyRemap(KeyRemapPlan &&plan);

// initialize hooks and clear old subscription state
// 初始化 hooks, 并清空旧的订阅状态
void Initialize(const Hooks &hooks);

// clear subscriptions and pending operations, but keep hooks
// 清空订阅和待处理操作, 但保留 hooks
void Clear();

// fully reset EventBus, including hooks
// 完整重置 EventBus, 包括 hooks
void Shutdown();

// Agent observation of published event types, per World and off by default.
// At most kPublishTraceLimit records are kept; older ones are dropped.
struct PublishedEvent {
    int frame = 0;
    std::string type;
};
inline constexpr std::size_t kPublishTraceLimit = 4096;
void SetPublishTracing(bool enabled);
// Returns and clears the records since the last call; dropped receives the
// number discarded for exceeding the limit.
std::vector<PublishedEvent> TakePublishedEvents(std::uint64_t *dropped = nullptr);

// publish immediately to current active subscribers
// 立即发布给当前已经生效的订阅者
void Publish(const std::string &event_type, luabridge::LuaRef event_object);

// queued subscription operations become visible only when applied
// 订阅和取消订阅都会先进入待处理队列, 统一 apply 后才生效
void Subscribe(const std::string &event_type, luabridge::LuaRef component_ref,
               luabridge::LuaRef function_ref);
void Unsubscribe(const std::string &event_type, luabridge::LuaRef component_ref,
                 luabridge::LuaRef function_ref);

// remove both active and pending subscriptions for one component
// 移除某个组件已有的和待生效的所有订阅
void RemoveSubscriptionsForComponent(const luabridge::LuaRef &component_ref,
                                     Actor::UID actor_uid,
                                     const std::string &component_key);

// apply queued subscribe / unsubscribe operations
// 统一应用排队中的订阅变更
void ApplyPendingOperations();

} // namespace EventBus

#endif
