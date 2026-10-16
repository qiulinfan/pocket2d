#include "scripting/LuaBoundary.h"
#include "core/World.h"
#include "scripting/EventBus.h"
#include "core/Vector2.h"
#include "scene/ActorHandle.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include <cmath>
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <deque>
#include <iterator>
#include <set>
#include <unordered_set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

// 一个已生效的订阅项
// 某个 component 的某个 Lua function 正在订阅某种 event_type
struct EventSubscription {
    luabridge::LuaRef component_ref;
    luabridge::LuaRef function_ref;
    Actor::UID actor_uid = Actor::kInvalidUID;
    std::string component_key;

    EventSubscription(luabridge::LuaRef component_ref_in,
                      luabridge::LuaRef function_ref_in,
                      Actor::UID actor_uid_in,
                      std::string component_key_in)
        : component_ref(std::move(component_ref_in)),
          function_ref(std::move(function_ref_in)), actor_uid(actor_uid_in),
          component_key(std::move(component_key_in)) {}
};

// Subscribe / Unsubscribe 不会立刻改动正式订阅表
// 而是先排入这个待生效队列, 等帧末统一应用
struct PendingEventOperation {
    bool is_subscribe = true;
    std::string event_type;
    luabridge::LuaRef component_ref;
    luabridge::LuaRef function_ref;
    Actor::UID actor_uid = Actor::kInvalidUID;
    std::string component_key;

    PendingEventOperation(bool is_subscribe_in, std::string event_type_in,
                          luabridge::LuaRef component_ref_in,
                          luabridge::LuaRef function_ref_in,
                          Actor::UID actor_uid_in,
                          std::string component_key_in)
        : is_subscribe(is_subscribe_in), event_type(std::move(event_type_in)),
          component_ref(std::move(component_ref_in)),
          function_ref(std::move(function_ref_in)), actor_uid(actor_uid_in),
          component_key(std::move(component_key_in)) {}
};

struct EventBusState {
    EventBus::Hooks hooks;
    std::unordered_map<std::string, std::vector<EventSubscription>>
        event_subscriptions;
    std::vector<PendingEventOperation> pending_event_operations;
    unsigned dispatch_depth = 0;
    bool trace_publishes = false;
    std::deque<EventBus::PublishedEvent> published;
    std::uint64_t published_dropped = 0;
};
EventBusState &Bus() { return World::Current().State<EventBusState>(); }
struct DispatchScope {
    DispatchScope() { ++Bus().dispatch_depth; }
    ~DispatchScope() { --Bus().dispatch_depth; }
};
const void *RawIdentity(const luabridge::LuaRef &value) {
    value.push(Bus().hooks.lua_state);
    const void *identity = lua_topointer(Bus().hooks.lua_state, -1);
    lua_pop(Bus().hooks.lua_state, 1);
    return identity;
}
struct CallbackIdentity {
    Actor::UID uid;
    std::string key;
    const void *component;
    const void *function;
    bool operator==(const CallbackIdentity &) const = default;
};
struct CallbackHash {
    std::size_t operator()(const CallbackIdentity &value) const {
        auto hash = std::hash<Actor::UID>{}(value.uid);
        const auto combine = [&](std::size_t part) { hash ^= part + 0x9e3779b9U + (hash << 6) + (hash >> 2); };
        combine(std::hash<std::string>{}(value.key));
        combine(std::hash<const void *>{}(value.component));
        combine(std::hash<const void *>{}(value.function));
        return hash;
    }
};


bool AreSameRef(const luabridge::LuaRef &a, const luabridge::LuaRef &b) {
    if (Bus().hooks.are_same_ref == nullptr) return false;
    return Bus().hooks.are_same_ref(a, b);
}

// 把一个 component LuaRef 解析为稳定身份: actor_uid + component_key
bool TryExtractComponentIdentity(const luabridge::LuaRef &component_ref,
                                 Actor::UID &actor_uid,
                                 std::string &component_key) {
    if (Bus().hooks.try_extract_component_identity == nullptr) return false;
    return Bus().hooks.try_extract_component_identity(component_ref, actor_uid,
                                                  component_key);
}

// 订阅表里可能残留已经被销毁的 component, 需要通过外部查询其存活性
bool IsComponentRefAlive(const luabridge::LuaRef &component_ref,
                         Actor::UID actor_uid,
                         const std::string &component_key) {
    if (Bus().hooks.is_component_ref_alive == nullptr) return false;
    return Bus().hooks.is_component_ref_alive(component_ref, actor_uid,
                                          component_key);
}

// EventBus 统一捕获 Lua 回调异常, 并交回宿主侧格式化输出
void ReportError(Actor::UID actor_uid, const LuaBoundary::Error &e) {
    if (Bus().hooks.report_error != nullptr) {
        Bus().hooks.report_error(actor_uid, e);
    }
}

// 一个订阅是否对应这个 component-function pair
bool DoesSubscriptionMatch(const EventSubscription &subscription,
                           const luabridge::LuaRef &component_ref,
                           const luabridge::LuaRef &function_ref,
                           Actor::UID actor_uid,
                           const std::string &component_key) {
    if (subscription.actor_uid != actor_uid) return false;
    if (subscription.component_key != component_key) return false;
    if (!AreSameRef(subscription.component_ref, component_ref)) return false;
    return AreSameRef(subscription.function_ref, function_ref);
}

} // namespace

namespace EventBus {

struct ReloadPlan::State {
    decltype(Bus().event_subscriptions) active;
    decltype(Bus().pending_event_operations) pending;
};
ReloadPlan::ReloadPlan() = default;
ReloadPlan::~ReloadPlan() = default;
ReloadPlan::ReloadPlan(ReloadPlan &&) noexcept = default;
ReloadPlan &ReloadPlan::operator=(ReloadPlan &&) noexcept = default;
bool IsDispatching() { return Bus().dispatch_depth != 0; }

bool PrepareReload(const std::vector<ComponentReload> &components, ReloadPlan &plan, std::string &error) {
    plan.state_.reset();
    error.clear();
    if (IsDispatching()) { error = "EventBus reload requires an idle dispatch boundary"; return false; }
    if (!Bus().hooks.lua_state) { error = "EventBus is not initialized"; return false; }
    struct StackScope {
        lua_State *state;
        int top;
        ~StackScope() { lua_settop(state, top); }
    } stack{Bus().hooks.lua_state, lua_gettop(Bus().hooks.lua_state)};
    try {
        using FunctionMap = std::unordered_map<const void *, luabridge::LuaRef>;
        std::unordered_map<const void *, FunctionMap> mappings;
        for (const auto &component : components) {
            if (component.component.state() != Bus().hooks.lua_state ||
                (!component.component.isTable() && !component.component.isUserdata()))
                throw std::invalid_argument("reload requires a component object from the active Lua VM");
            const auto [entry, inserted] = mappings.try_emplace(RawIdentity(component.component));
            if (!inserted) throw std::invalid_argument("duplicate component reload entry");
            for (const auto &replacement : component.functions) {
                if (replacement.before.state() != Bus().hooks.lua_state || replacement.after.state() != Bus().hooks.lua_state ||
                    !replacement.before.isFunction() || (!replacement.after.isNil() && !replacement.after.isFunction()))
                    throw std::invalid_argument("reload callback replacement must be a function to function/nil in the active VM");
                const auto [mapping, fresh] = entry->second.emplace(RawIdentity(replacement.before), replacement.after);
                if (!fresh) {
                    const bool same = mapping->second.isNil() && replacement.after.isNil();
                    if (!same && !AreSameRef(mapping->second, replacement.after))
                        throw std::invalid_argument("ambiguous replacement for one old callback function");
                }
            }
        }
        auto candidate = std::make_unique<ReloadPlan::State>();
        candidate->active = Bus().event_subscriptions;
        candidate->pending = Bus().pending_event_operations;
        const auto remap = [&](auto &entry) {
            const auto owner = mappings.find(RawIdentity(entry.component_ref));
            if (owner == mappings.end()) return true;
            const auto function = owner->second.find(RawIdentity(entry.function_ref));
            if (function == owner->second.end())
                throw std::invalid_argument("affected component has an unmapped callback (anonymous/instance closure)");
            if (function->second.isNil()) return false;
            entry.function_ref = function->second;
            return true;
        };
        for (auto event = candidate->active.begin(); event != candidate->active.end();) {
            auto &subscriptions = event->second;
            std::unordered_set<CallbackIdentity, CallbackHash> seen;
            std::erase_if(subscriptions, [&](auto &subscription) {
                if (!remap(subscription)) return true;
                // Same identity rules as Subscribe; retain the first position
                // when two old methods now resolve to the same new function.
                return !seen.insert({subscription.actor_uid, subscription.component_key,
                                     RawIdentity(subscription.component_ref), RawIdentity(subscription.function_ref)}).second;
            });
            if (subscriptions.empty()) event = candidate->active.erase(event);
            else ++event;
        }
        // Keep FIFO exactly. ApplyPendingOperations already deduplicates each
        // Subscribe against the active set; removing nonadjacent queued entries
        // here could change subscribe/unsubscribe/subscribe semantics.
        std::erase_if(candidate->pending, [&](auto &operation) { return !remap(operation); });
        plan.state_ = std::move(candidate);
        return true;
    } catch (const std::exception &failure) {
        error = failure.what();
        return false;
    }
}
void CommitReload(ReloadPlan &&plan) noexcept {
    assert(!IsDispatching()); // caller commits immediately at the idle boundary
    if (!plan.state_) return;
    Bus().event_subscriptions.swap(plan.state_->active);
    Bus().pending_event_operations.swap(plan.state_->pending);
    plan.state_.reset();
}

struct KeyRemapPlan::State {
    lua_State *vm=nullptr;
    std::unordered_map<const void *,ComponentKeyRemap> components;
};
KeyRemapPlan::KeyRemapPlan()=default;
KeyRemapPlan::~KeyRemapPlan()=default;
KeyRemapPlan::KeyRemapPlan(KeyRemapPlan &&) noexcept=default;
KeyRemapPlan &KeyRemapPlan::operator=(KeyRemapPlan &&) noexcept=default;
bool PrepareKeyRemap(const std::vector<ComponentKeyRemap> &components,KeyRemapPlan &plan,std::string &error) {
    plan.state_.reset();error.clear();
    if(IsDispatching()){error="EventBus key remap requires an idle dispatch boundary";return false;}
    if(!Bus().hooks.lua_state){error="EventBus is not initialized";return false;}
    const int top=lua_gettop(Bus().hooks.lua_state);
    try {
        auto candidate=std::make_unique<KeyRemapPlan::State>();candidate->vm=Bus().hooks.lua_state;
        std::set<std::pair<Actor::UID,std::string>> targets;
        for(const auto &component:components) {
            if(component.component.state()!=Bus().hooks.lua_state||(!component.component.isTable()&&!component.component.isUserdata()))
                throw std::invalid_argument("key remap requires a component object from the active Lua VM");
            if(component.actor_uid==Actor::kInvalidUID||component.before.empty()||component.after.empty()||
               component.before.size()>256||component.after.size()>256||component.before.find('\0')!=std::string::npos||component.after.find('\0')!=std::string::npos||component.before==component.after)
                throw std::invalid_argument("key remap requires distinct valid component keys and actor identity");
            if(!IsComponentRefAlive(component.component,component.actor_uid,component.before))
                throw std::invalid_argument("key remap component is not alive at its original key");
            if(!targets.emplace(component.actor_uid,component.after).second)
                throw std::invalid_argument("key remap contains a duplicate target key");
            if(!candidate->components.emplace(RawIdentity(component.component),component).second)
                throw std::invalid_argument("key remap contains a duplicate component");
        }
        plan.state_=std::move(candidate);lua_settop(Bus().hooks.lua_state,top);return true;
    }catch(const std::exception &failure){lua_settop(Bus().hooks.lua_state,top);error=failure.what();return false;}
}
void CommitKeyRemap(KeyRemapPlan &&plan) {
    if(!plan.state_)return;
    if(IsDispatching()||Bus().hooks.lua_state!=plan.state_->vm)
        throw std::runtime_error("EventBus key remap lost its idle Lua lifetime boundary");
    const auto remap=[&](auto &entry) {
        const auto found=plan.state_->components.find(RawIdentity(entry.component_ref));
        if(found!=plan.state_->components.end()&&entry.actor_uid==found->second.actor_uid&&entry.component_key==found->second.before)
            entry.component_key=found->second.after;
    };
    for(auto &[event,entries]:Bus().event_subscriptions)for(auto &entry:entries)remap(entry);
    for(auto &entry:Bus().pending_event_operations)remap(entry);
    plan.state_.reset();
}

// initialize hooks and clear old subscription state
void Initialize(const Hooks &hooks) {
    Bus().hooks = hooks;
    Clear();
}

// clear subscriptions and pending operations, but keep hooks
void Clear() {
    Bus().event_subscriptions.clear();
    Bus().pending_event_operations.clear();
}

// fully reset EventBus, including hooks
void Shutdown() {
    Clear();
    Bus().hooks = Hooks();
}

namespace {
using JsonWriter = rapidjson::Writer<rapidjson::StringBuffer>;
void WritePayload(lua_State *state, int index, JsonWriter &out, int depth) {
    constexpr std::size_t kMaxString = 256;
    constexpr int kMaxEntries = 32, kMaxDepth = 4;
    index = lua_absindex(state, index);
    switch (lua_type(state, index)) {
    case LUA_TBOOLEAN: out.Bool(lua_toboolean(state, index) != 0); return;
    case LUA_TNUMBER:
        if (lua_isinteger(state, index)) out.Int64(lua_tointeger(state, index));
        else if (std::isfinite(lua_tonumber(state, index))) out.Double(lua_tonumber(state, index));
        else out.Null();
        return;
    case LUA_TSTRING: {
        std::size_t length = 0;
        const char *text = lua_tolstring(state, index, &length);
        out.String(text, static_cast<rapidjson::SizeType>(std::min(length, kMaxString)));
        return;
    }
    case LUA_TUSERDATA: {
        const auto value = luabridge::LuaRef::fromStack(state, index);
        if (value.isInstance<Vector2>()) {
            const auto vector = value.cast<Vector2>().value();
            out.StartObject();
            out.Key("x"); out.Double(std::isfinite(vector.x) ? vector.x : 0.0);
            out.Key("y"); out.Double(std::isfinite(vector.y) ? vector.y : 0.0);
            out.EndObject();
        } else if (value.isInstance<ActorHandle>()) {
            out.StartObject();
            out.Key("actor_uid"); out.String(std::to_string(value.cast<ActorHandle>().value().uid).c_str());
            out.EndObject();
        } else out.String("<userdata>");
        return;
    }
    case LUA_TFUNCTION: out.String("<function>"); return;
    case LUA_TTABLE: break;
    default: out.Null(); return;
    }
    if (depth >= kMaxDepth || !lua_checkstack(state, 4)) { out.String("<table>"); return; }
    const lua_Unsigned length = lua_rawlen(state, index);
    if (length > 0) {
        out.StartArray();
        for (lua_Unsigned i = 1; i <= length && i <= kMaxEntries; ++i) {
            lua_rawgeti(state, index, static_cast<lua_Integer>(i));
            WritePayload(state, -1, out, depth + 1);
            lua_pop(state, 1);
        }
        out.EndArray();
        return;
    }
    out.StartObject();
    int entries = 0;
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        if (entries++ < kMaxEntries && (lua_type(state, -2) == LUA_TSTRING || lua_type(state, -2) == LUA_TNUMBER)) {
            lua_pushvalue(state, -2); // tostring on a copy keeps lua_next's key intact
            std::size_t key_length = 0;
            const char *key = lua_tolstring(state, -1, &key_length);
            out.Key(key, static_cast<rapidjson::SizeType>(std::min(key_length, kMaxString)));
            lua_pop(state, 1);
            WritePayload(state, -1, out, depth + 1);
        }
        lua_pop(state, 1);
    }
    out.EndObject();
}

std::string PayloadJson(const luabridge::LuaRef &payload) {
    lua_State *state = payload.state();
    if (state == nullptr) return "null";
    const int top = lua_gettop(state);
    rapidjson::StringBuffer buffer;
    JsonWriter out(buffer);
    try {
        payload.push(state);
        WritePayload(state, -1, out, 0);
    } catch (...) {
        lua_settop(state, top);
        return "null";
    }
    lua_settop(state, top);
    return buffer.GetString();
}
} // namespace

void SetPublishTracing(bool enabled) {
    Bus().trace_publishes = enabled;
    if (!enabled) { Bus().published.clear(); Bus().published_dropped = 0; }
}

std::vector<PublishedEvent> TakePublishedEvents(std::uint64_t *dropped) {
    if (dropped) *dropped = Bus().published_dropped;
    Bus().published_dropped = 0;
    auto &published = Bus().published;
    std::vector<PublishedEvent> result(std::make_move_iterator(published.begin()),
                                       std::make_move_iterator(published.end()));
    published.clear();
    return result;
}

// publish immediately to current active subscribers
void Publish(const std::string &event_type, luabridge::LuaRef event_object) {
    if (Bus().trace_publishes) {
        auto &published = Bus().published;
        if (published.size() == kPublishTraceLimit) {
            published.pop_front();
            ++Bus().published_dropped;
        }
        published.push_back({World::Current().frame_number, event_type, PayloadJson(event_object)});
    }
    const DispatchScope dispatch;
    // Publish 是立即分发的: 只读取“当前已生效”的订阅表
    auto subscriptions_it = Bus().event_subscriptions.find(event_type);
    if (subscriptions_it == Bus().event_subscriptions.end()) return;

    std::vector<EventSubscription> &subscriptions = subscriptions_it->second;
    // 先顺手清掉已经失效的订阅, 避免把事件投给被销毁的 component
    subscriptions.erase(
        std::remove_if(subscriptions.begin(), subscriptions.end(),
                       [](const EventSubscription &subscription) {
                           return !IsComponentRefAlive(
                               subscription.component_ref, subscription.actor_uid,
                               subscription.component_key);
                       }),
        subscriptions.end());

    if (subscriptions.empty()) {
        Bus().event_subscriptions.erase(subscriptions_it);
        return;
    }

    // 用快照分发, 避免 callback 内再次 Publish / Unsubscribe 时
    // 直接打乱当前这次派发的遍历
    const std::vector<EventSubscription> subscribers = subscriptions;
    for (const EventSubscription &subscription : subscribers) {
        if (!IsComponentRefAlive(subscription.component_ref, subscription.actor_uid,
                                 subscription.component_key)) {
            continue;
        }

        const int top = lua_gettop(Bus().hooks.lua_state);
        try {
            LuaBoundary::CallDiscard(subscription.function_ref, subscription.component_ref, event_object);
        } catch (const LuaBoundary::Error &e) {
            ReportError(subscription.actor_uid, e);
        }
        lua_settop(Bus().hooks.lua_state, top);
    }
}

// queued subscription operations become visible only when applied
void Subscribe(const std::string &event_type, luabridge::LuaRef component_ref,
               luabridge::LuaRef function_ref) {
    // 只有合法 Lua function 才能被订阅
    if (!function_ref.isFunction()) return;

    Actor::UID actor_uid = Actor::kInvalidUID;
    std::string component_key;
    if (!TryExtractComponentIdentity(component_ref, actor_uid, component_key)) {
        return;
    }

    Bus().pending_event_operations.emplace_back(true, event_type, component_ref,
                                            function_ref, actor_uid,
                                            component_key);
}

// queued subscription operations become visible only when applied
void Unsubscribe(const std::string &event_type, luabridge::LuaRef component_ref,
                 luabridge::LuaRef function_ref) {
    if (!function_ref.isFunction()) return;

    Actor::UID actor_uid = Actor::kInvalidUID;
    std::string component_key;
    if (!TryExtractComponentIdentity(component_ref, actor_uid, component_key)) {
        return;
    }

    Bus().pending_event_operations.emplace_back(false, event_type, component_ref,
                                            function_ref, actor_uid,
                                            component_key);
}

// remove both active and pending subscriptions for one component
void RemoveSubscriptionsForComponent(const luabridge::LuaRef &component_ref,
                                     Actor::UID actor_uid,
                                     const std::string &component_key) {
    for (auto subscriptions_it = Bus().event_subscriptions.begin();
         subscriptions_it != Bus().event_subscriptions.end();) {
        std::vector<EventSubscription> &subscriptions = subscriptions_it->second;
        subscriptions.erase(
            std::remove_if(subscriptions.begin(), subscriptions.end(),
                           [&](const EventSubscription &subscription) {
                               return DoesSubscriptionMatch(
                                   subscription, component_ref,
                                   subscription.function_ref, actor_uid,
                                   component_key);
                           }),
            subscriptions.end());

        if (subscriptions.empty()) {
            subscriptions_it = Bus().event_subscriptions.erase(subscriptions_it);
        } else {
            ++subscriptions_it;
        }
    }

    Bus().pending_event_operations.erase(
        std::remove_if(
            Bus().pending_event_operations.begin(), Bus().pending_event_operations.end(),
            [&](const PendingEventOperation &operation) {
                return operation.actor_uid == actor_uid &&
                       operation.component_key == component_key &&
                       AreSameRef(operation.component_ref, component_ref);
            }),
        Bus().pending_event_operations.end());
}

// apply queued subscribe / unsubscribe operations
void ApplyPendingOperations() {
    if (Bus().pending_event_operations.empty()) return;

    std::vector<PendingEventOperation> pending_operations = std::move(Bus().pending_event_operations);
    Bus().pending_event_operations.clear();

    for (const PendingEventOperation &operation : pending_operations) {
        if (operation.event_type.empty()) continue;

        auto subscriptions_it = Bus().event_subscriptions.find(operation.event_type);
        if (!operation.is_subscribe) {
            if (subscriptions_it == Bus().event_subscriptions.end()) continue;

            std::vector<EventSubscription> &subscriptions = subscriptions_it->second;
            subscriptions.erase(
                std::remove_if(
                    subscriptions.begin(), subscriptions.end(),
                    [&](const EventSubscription &subscription) {
                        return DoesSubscriptionMatch(
                            subscription, operation.component_ref,
                            operation.function_ref, operation.actor_uid,
                            operation.component_key);
                    }),
                subscriptions.end());

            if (subscriptions.empty()) {
                Bus().event_subscriptions.erase(subscriptions_it);
            }
            continue;
        }

        // Subscribe 生效时再次检查 component 是否还活着
        if (!operation.function_ref.isFunction()) continue;
        if (!IsComponentRefAlive(operation.component_ref, operation.actor_uid,
                                 operation.component_key)) {
            continue;
        }

        std::vector<EventSubscription> &subscriptions = Bus().event_subscriptions[operation.event_type];
        // 同一个 component-function pair 只保留一份正式订阅
        const bool already_subscribed = std::any_of(
            subscriptions.begin(), subscriptions.end(),
            [&](const EventSubscription &subscription) {
                return DoesSubscriptionMatch(
                    subscription, operation.component_ref, operation.function_ref,
                    operation.actor_uid, operation.component_key);
            });
        if (already_subscribed) continue;

        subscriptions.emplace_back(operation.component_ref, operation.function_ref,
                                   operation.actor_uid, operation.component_key);
    }
}

} // namespace EventBus
