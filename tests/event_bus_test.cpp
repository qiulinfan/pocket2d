#include "scripting/EventBus.h"
#include "scripting/LuaBoundary.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
void Check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

class LuaFixture;
LuaFixture *active_fixture = nullptr;

bool SameRef(const luabridge::LuaRef &a, const luabridge::LuaRef &b) {
    if (a.state() != b.state()) return false;
    lua_State *state = a.state();
    const int top = lua_gettop(state);
    a.push(state);
    b.push(state);
    const bool same = lua_rawequal(state, -1, -2) != 0;
    lua_settop(state, top);
    return same;
}

bool Identity(const luabridge::LuaRef &component, Actor::UID &uid, std::string &key) {
    if (!component.isTable()) return false;
    luabridge::LuaRef uid_ref = component["uid"];
    luabridge::LuaRef key_ref = component["key"];
    if (!uid_ref.isNumber() || !key_ref.isString()) return false;
    const int value = uid_ref.cast<int>().value();
    if (value <= 0) return false;
    uid = static_cast<Actor::UID>(value);
    key = key_ref.cast<std::string>().value();
    return !key.empty();
}

class LuaFixture {
public:
    lua_State *state = nullptr;
    std::map<std::pair<Actor::UID, std::string>, luabridge::LuaRef> live;
    int errors = 0;
    Actor::UID last_error_actor = Actor::kInvalidUID;
    std::string last_error;

    LuaFixture() {
        state = luaL_newstate();
        Check(state != nullptr, "Lua state creation failed");
        luaL_openlibs(state);
        active_fixture = this;
        InitializeBus();
        luabridge::getGlobalNamespace(state)
            .beginNamespace("Test")
            .addFunction("NewComponent", &NewComponent)
            .addFunction("DestroyComponent", &DestroyComponent)
            .addFunction("Publish", &EventBus::Publish)
            .addFunction("Subscribe", &EventBus::Subscribe)
            .addFunction("Unsubscribe", &EventBus::Unsubscribe)
            .addFunction("Apply", &EventBus::ApplyPendingOperations)
            .addFunction("Clear", &EventBus::Clear)
            .addFunction("ReloadBusyRejected", &ReloadBusyRejected)
            .addFunction("KeyRemapBusyRejected", &KeyRemapBusyRejected)
            .endNamespace();
    }

    ~LuaFixture() {
        EventBus::Shutdown();
        live.clear();
        lua_close(state);
        active_fixture = nullptr;
    }

    void InitializeBus() {
        EventBus::Hooks hooks;
        hooks.lua_state = state;
        hooks.are_same_ref = &SameRef;
        hooks.try_extract_component_identity = &Identity;
        hooks.is_component_ref_alive = &IsAlive;
        hooks.report_error = &ReportError;
        EventBus::Initialize(hooks);
    }

    void Run(const char *source) {
        if (luaL_dostring(state, source) != LUA_OK) {
            const std::string message = lua_tostring(state, -1);
            lua_settop(state, 0);
            throw std::runtime_error(message);
        }
        Check(lua_gettop(state) == 0, "event callbacks leaked Lua stack entries");
    }

private:
    static bool KeyRemapBusyRejected() {
        EventBus::KeyRemapPlan plan;std::string error;
        return EventBus::IsDispatching()&&!EventBus::PrepareKeyRemap({},plan,error)&&!error.empty();
    }
    static bool ReloadBusyRejected() {
        EventBus::ReloadPlan plan;
        std::string error;
        return EventBus::IsDispatching() && !EventBus::PrepareReload({}, plan, error) && !error.empty();
    }
    static luabridge::LuaRef NewComponent(int uid, const std::string &key) {
        auto component = luabridge::newTable(active_fixture->state);
        component["uid"] = uid;
        component["key"] = key;
        component["calls"] = 0;
        const auto identity = std::make_pair(static_cast<Actor::UID>(uid), key);
        active_fixture->live.insert_or_assign(identity, component);
        return component;
    }

    static bool IsAlive(const luabridge::LuaRef &component, Actor::UID uid,
                         const std::string &key) {
        const auto found = active_fixture->live.find({uid, key});
        return found != active_fixture->live.end() && SameRef(found->second, component);
    }

    static void DestroyComponent(luabridge::LuaRef component) {
        Actor::UID uid;
        std::string key;
        if (!Identity(component, uid, key)) return;
        if (IsAlive(component, uid, key)) active_fixture->live.erase({uid, key});
        EventBus::RemoveSubscriptionsForComponent(component, uid, key);
    }

    static void ReportError(Actor::UID uid, const LuaBoundary::Error &error) {
        ++active_fixture->errors;
        active_fixture->last_error_actor = uid;
        active_fixture->last_error = error.what();
    }
};

void TestSubscriptionBoundaries() {
    LuaFixture fixture;
    fixture.Run(R"lua(
        a = Test.NewComponent(1, "a")
        function receive(self, event) self.calls = self.calls + event.delta end
        Test.Subscribe("tick", a, receive)
        Test.Subscribe("tick", a, receive)
        Test.Publish("tick", {delta=1})
        assert(a.calls == 0, "queued subscribers must not receive events")
        Test.Apply()
        Test.Publish("tick", {delta=1})
        assert(a.calls == 1, "duplicate pair must subscribe only once")
        Test.Unsubscribe("tick", a, receive)
        Test.Publish("tick", {delta=1})
        assert(a.calls == 2, "unsubscribe is deferred until apply")
        Test.Apply()
        Test.Publish("tick", {delta=1})
        assert(a.calls == 2, "unsubscribe must remove active subscription")
        Test.Subscribe("tick", a, receive)
        Test.Unsubscribe("tick", a, receive)
        Test.Subscribe("", a, receive)
        Test.Subscribe("tick", a, 42)
        Test.Subscribe("tick", nil, receive)
        Test.Apply()
        Test.Publish("tick", {delta=1})
        Test.Publish("", {delta=1})
        assert(a.calls == 2, "FIFO subscribe/unsubscribe and invalid input filtering")
    )lua");
    Check(fixture.errors == 0, "valid subscription operations reported an error");
}

void TestRecursivePublishAndQueuedMutation() {
    LuaFixture fixture;
    fixture.Run(R"lua(
        a = Test.NewComponent(1, "a")
        b = Test.NewComponent(2, "b")
        c = Test.NewComponent(3, "c")
        trace = {}
        function later(self, event) self.calls = self.calls + 1 end
        function first(self, event)
            table.insert(trace, "a" .. event.depth)
            if event.depth == 0 then
                Test.Unsubscribe("event", self, first)
                Test.Subscribe("event", c, later)
                Test.Publish("event", {depth=1})
            end
        end
        function second(self, event) table.insert(trace, "b" .. event.depth) end
        Test.Subscribe("event", a, first)
        Test.Subscribe("event", b, second)
        Test.Apply()
        Test.Publish("event", {depth=0})
        assert(table.concat(trace, ",") == "a0,a1,b1,b0", "recursive snapshot order")
        assert(c.calls == 0, "new callback cannot join an in-progress dispatch")
        Test.Apply()
        Test.Publish("event", {depth=2})
        assert(table.concat(trace, ",") == "a0,a1,b1,b0,b2" and c.calls == 1,
               "callback mutations must apply in order after dispatch")
    )lua");
}

void TestDestroyAndIdentityReuse() {
    LuaFixture fixture;
    fixture.Run(R"lua(
        a = Test.NewComponent(1, "a")
        b = Test.NewComponent(2, "b")
        function receive(self, event) self.calls = self.calls + 1 end
        function destroy_self(self, event)
            self.calls = self.calls + 1
            Test.Subscribe("other", self, receive)
            Test.DestroyComponent(self)
            Test.Publish("event", {})
        end
        Test.Subscribe("event", a, destroy_self)
        Test.Subscribe("event", b, receive)
        Test.Apply()
        Test.Publish("event", {})
        assert(a.calls == 1 and b.calls == 2, "self destruction must filter recursive publish")
        Test.Apply()
        Test.Publish("other", {})
        assert(a.calls == 1, "destroy removes pending subscriptions too")
        Test.Clear()
        a = Test.NewComponent(1, "a")
        function destroy_next(self, event) Test.DestroyComponent(b) end
        Test.Subscribe("event", a, destroy_next)
        Test.Subscribe("event", b, receive)
        Test.Apply()
        Test.Publish("event", {})
        assert(b.calls == 2, "alive check must run immediately before each callback")
        Test.Clear()
        old = Test.NewComponent(7, "same-key")
        Test.Subscribe("event", old, receive)
        Test.Apply()
        replacement = Test.NewComponent(7, "same-key")
        Test.Publish("event", {})
        assert(old.calls == 0 and replacement.calls == 0, "identity reuse must not revive old refs")
        Test.Subscribe("event", old, receive)
        Test.Apply()
        Test.Publish("event", {})
        assert(old.calls == 0, "dead subscription must be rejected when applied")
    )lua");
}

void TestCallbackErrors() {
    LuaFixture fixture;
    fixture.Run(R"lua(
        a = Test.NewComponent(1, "a")
        b = Test.NewComponent(2, "b")
        function broken(self, event) error("intentional nested subscriber failure") end
        function receive(self, event) self.calls = self.calls + 1 end
        function outer(self, event)
            Test.Publish("inner", {})
            self.calls = self.calls + 1
        end
        Test.Subscribe("inner", a, broken)
        Test.Subscribe("inner", b, receive)
        Test.Subscribe("outer", b, outer)
        Test.Apply()
        Test.Publish("outer", {})
        assert(b.calls == 2, "errors must not prevent later subscribers or outer Lua continuation")
    )lua");
    Check(fixture.errors == 1 && fixture.last_error_actor == 1 &&
              fixture.last_error.find("intentional nested subscriber failure") != std::string::npos,
          "callback error must reach the host exactly once with actor identity");
}

void TestClearAndReset() {
    LuaFixture fixture;
    fixture.Run(R"lua(
        a = Test.NewComponent(1, "a")
        function receive(self, event) self.calls = self.calls + 1 end
        Test.Subscribe("active", a, receive)
        Test.Apply()
        Test.Subscribe("pending", a, receive)
        Test.Clear()
        Test.Apply()
        Test.Publish("active", {})
        Test.Publish("pending", {})
        assert(a.calls == 0, "clear must remove active and pending subscriptions")
        Test.Subscribe("event", a, receive)
        Test.Apply()
        Test.Publish("event", {})
        assert(a.calls == 1, "clear must retain hooks for subsequent subscriptions")
    )lua");
    EventBus::Shutdown();
    fixture.Run(R"lua(
        Test.Subscribe("event", a, receive)
        Test.Apply()
        Test.Publish("event", {})
        assert(a.calls == 1, "shutdown must disable old hooks and subscriptions")
    )lua");
    fixture.InitializeBus();
    fixture.Run(R"lua(
        Test.Publish("event", {})
        assert(a.calls == 1, "reinitialization must not restore old subscribers")
        Test.Subscribe("event", a, receive)
        Test.Apply()
        Test.Publish("event", {})
        assert(a.calls == 2, "reinitialized bus must dispatch again")
    )lua");
}
void TestReloadActivePendingAndRemoval() {
    LuaFixture fixture;
    fixture.Run(R"lua(
        a=Test.NewComponent(1,"a"); b=Test.NewComponent(2,"b")
        trace={}
        function old(self,e) self.calls=self.calls+1;table.insert(trace,"old-"..self.key) end
        function removed(self,e) self.calls=self.calls+10;table.insert(trace,"removed") end
        function replacement(self,e) self.calls=self.calls+100;table.insert(trace,"new-"..self.key) end
        Test.Subscribe("active",a,old); Test.Subscribe("active",b,old); Test.Subscribe("active",a,removed)
        Test.Subscribe("cancel",a,old); Test.Subscribe("gone",a,removed)
        Test.Apply()
        Test.Subscribe("queued",a,old); Test.Subscribe("queued",b,old)
        Test.Unsubscribe("cancel",a,old)
        Test.Subscribe("gone-pending",a,removed); Test.Unsubscribe("gone",a,removed)
    )lua");
    const auto a=luabridge::getGlobal(fixture.state,"a");
    const auto old=luabridge::getGlobal(fixture.state,"old");
    const auto removed=luabridge::getGlobal(fixture.state,"removed");
    const auto next=luabridge::getGlobal(fixture.state,"replacement");
    const luabridge::LuaRef nil(fixture.state);
    const std::vector<EventBus::ComponentReload> changes{{a,{{old,next},{removed,nil},{removed,nil}}}};
    std::string error;
    {
        EventBus::ReloadPlan abandoned;
        Check(EventBus::PrepareReload(changes,abandoned,error) && error.empty(),"valid reload preparation failed");
    }
    fixture.Run(R"lua(
        Test.Publish("active",{})
        assert(a.calls==11 and b.calls==1 and table.concat(trace,",")=="old-a,old-b,removed",
               "abandoned prepare modified active subscriptions")
        a.calls=0;b.calls=0;trace={}
    )lua");
    EventBus::ReloadPlan plan;
    Check(EventBus::PrepareReload(changes,plan,error),"prepare replacement");
    EventBus::ReloadPlan moved(std::move(plan));
    EventBus::CommitReload(std::move(moved));
    EventBus::CommitReload(std::move(plan)); // moved-from plan is harmless
    fixture.Run(R"lua(
        Test.Publish("active",{})
        assert(a.calls==100 and b.calls==1 and table.concat(trace,",")=="new-a,old-b",
               "rebind changed order/unaffected instance or retained deleted method")
        Test.Publish("queued",{});assert(a.calls==100 and b.calls==1,"pending subscribe became early")
        Test.Apply()
        Test.Publish("queued",{});assert(a.calls==200 and b.calls==2,"pending subscribe not remapped")
        Test.Publish("cancel",{});assert(a.calls==200,"pending unsubscribe not remapped")
        Test.Publish("gone",{});Test.Publish("gone-pending",{});assert(a.calls==200,"deleted callback survived")
        a.Handler=replacement
        Test.Unsubscribe("active",a,a.Handler);Test.Apply()
        Test.Publish("active",{});assert(a.calls==200 and b.calls==3,"new self method cannot unsubscribe replacement")
    )lua");
}
void TestReloadAliasesAndFIFO() {
    LuaFixture fixture;
    fixture.Run(R"lua(
        a=Test.NewComponent(1,"a");b=Test.NewComponent(2,"b")
        function old1(self,e) self.calls=self.calls+1 end
        function old2(self,e) self.calls=self.calls+10 end
        function new(self,e) self.calls=self.calls+100 end
        Test.Subscribe("alias",a,old1);Test.Subscribe("alias",a,old2);Test.Subscribe("alias",b,old1)
        Test.Subscribe("fifo",a,old1);Test.Apply()
        Test.Unsubscribe("fifo",a,old1);Test.Subscribe("fifo",a,old2)
        Test.Unsubscribe("fifo",a,old2);Test.Subscribe("fifo",a,old1);Test.Subscribe("fifo",a,old2)
    )lua");
    auto a=luabridge::getGlobal(fixture.state,"a");
    auto old1=luabridge::getGlobal(fixture.state,"old1");
    auto old2=luabridge::getGlobal(fixture.state,"old2");
    auto next=luabridge::getGlobal(fixture.state,"new");
    EventBus::ReloadPlan plan;std::string error;
    Check(EventBus::PrepareReload({{a,{{old1,next},{old1,next},{old2,next}}}},plan,error),"equivalent aliases rejected");
    EventBus::CommitReload(std::move(plan));
    fixture.Run(R"lua(
        Test.Publish("alias",{});assert(a.calls==100 and b.calls==1,"mapped active aliases duplicated or removed other owner")
        Test.Apply();Test.Publish("fifo",{});assert(a.calls==200,"pending FIFO/subscribe dedup changed")
    )lua");
}
void TestReloadFailuresPreserveBus() {
    LuaFixture fixture;
    fixture.Run(R"lua(
        a=Test.NewComponent(1,"a")
        function old(self,e) self.calls=self.calls+1 end
        function new1(self,e) self.calls=self.calls+100 end
        function new2(self,e) self.calls=self.calls+1000 end
        local captured=7
        anonymous=function(self,e) self.calls=self.calls+captured end
        Test.Subscribe("active",a,old);Test.Apply();Test.Subscribe("pending",a,anonymous)
    )lua");
    auto a=luabridge::getGlobal(fixture.state,"a");
    auto old=luabridge::getGlobal(fixture.state,"old");
    auto next=luabridge::getGlobal(fixture.state,"new1");
    auto other=luabridge::getGlobal(fixture.state,"new2");
    auto anonymous=luabridge::getGlobal(fixture.state,"anonymous");
    EventBus::ReloadPlan plan;std::string error;
    Check(!EventBus::PrepareReload({{a,{{old,next}}}},plan,error) && error.find("unmapped")!=std::string::npos,
          "unmapped pending closure did not reject reload");
    Check(!EventBus::PrepareReload({{a,{{old,next},{old,other},{anonymous,next}}}},plan,error) &&
          error.find("ambiguous")!=std::string::npos,"conflicting old-function mapping accepted");
    Check(!EventBus::PrepareReload({{a,{{old,next},{anonymous,next}}},{a,{{old,next}}}},plan,error),"duplicate affected component accepted");
    Check(!EventBus::PrepareReload({{a,{{old,luabridge::LuaRef(fixture.state,42)}}}},plan,error),"non-function replacement accepted");
    EventBus::CommitReload(std::move(plan)); // failed prepare leaves no candidate
    fixture.Run(R"lua(
        Test.Publish("active",{});assert(a.calls==1,"failure replaced active callback")
        Test.Apply();Test.Publish("pending",{});assert(a.calls==8,"failure removed pending closure")
    )lua");
    Check(!EventBus::PrepareReload({{a,{{old,next}}}},plan,error),"unmapped active closure did not reject reload");
    Check(lua_gettop(fixture.state)==0,"failed preparation leaked Lua stack");
}
void TestReloadBusyNestedAndErrors() {
    LuaFixture fixture;
    Check(!EventBus::IsDispatching(),"dispatch depth leaked from previous test");
    fixture.Run(R"lua(
        a=Test.NewComponent(1,"a");b=Test.NewComponent(2,"b")
        function nested(self,e)
            assert(Test.ReloadBusyRejected(),"nested dispatch accepted reload")
            if e.fail then error("reload busy nested sentinel") end
        end
        function outer(self,e)
            assert(Test.ReloadBusyRejected(),"outer dispatch accepted reload")
            Test.Publish("inner",{fail=true})
            assert(Test.ReloadBusyRejected(),"nested error cleared outer depth")
            self.calls=self.calls+1
        end
        function last(self,e) assert(Test.ReloadBusyRejected());self.calls=self.calls+1 end
        Test.Subscribe("inner",a,nested);Test.Subscribe("outer",a,outer);Test.Subscribe("outer",b,last);Test.Apply()
        Test.Publish("outer",{})
        assert(a.calls==1 and b.calls==1,"busy rejection disrupted dispatch/error continuation")
    )lua");
    Check(!EventBus::IsDispatching() && fixture.errors==1,"dispatch depth/error handling did not unwind");
    EventBus::ReloadPlan empty;std::string error;
    Check(EventBus::PrepareReload({},empty,error),"idle reload rejected after nested callback failure");
    EventBus::CommitReload(std::move(empty));
}

void TestKeyRemapAliasesFIFOAndLateOperations() {
    LuaFixture fixture;
    fixture.Run(R"(
        a=Test.NewComponent(1,'a');b=Test.NewComponent(1,'b');trace=''
        function named(self,e)trace=trace..'n';self.calls=self.calls+1 end
        local captured=10
        anonymous=function(self,e)trace=trace..'a';self.calls=self.calls+captured end
        function other(self,e)trace=trace..'b';self.calls=self.calls+100 end
        Test.Subscribe('active',a,named);Test.Subscribe('active',a,anonymous);Test.Subscribe('active',b,other)
        Test.Subscribe('fifo',a,named);Test.Apply()
        Test.Unsubscribe('fifo',a,named);Test.Subscribe('fifo',a,anonymous)
        Test.Unsubscribe('fifo',a,anonymous);Test.Subscribe('fifo',a,named)
        Test.Subscribe('pending',b,anonymous)
    )");
    auto a=luabridge::getGlobal(fixture.state,"a"),b=luabridge::getGlobal(fixture.state,"b");
    EventBus::KeyRemapPlan plan;std::string error;
    Check(EventBus::PrepareKeyRemap({{a,1,"a","b"},{b,1,"b","a"}},plan,error),"key swap preparation failed");
    fixture.Run("Test.Subscribe('late',a,anonymous);Test.Unsubscribe('pending',b,anonymous);Test.Subscribe('pending',b,anonymous)");
    fixture.live.erase({1,"a"});fixture.live.erase({1,"b"});a["key"]=std::string("b");b["key"]=std::string("a");
    fixture.live.emplace(std::make_pair(Actor::UID(1),std::string("b")),a);fixture.live.emplace(std::make_pair(Actor::UID(1),std::string("a")),b);
    EventBus::CommitKeyRemap(std::move(plan));
    fixture.Run(R"(
        Test.Publish('active',{});assert(a.calls==11 and b.calls==100 and trace=='nab','active function refs/order changed')
        Test.Publish('pending',{});assert(b.calls==100,'pending operation activated during rename')
        Test.Apply();Test.Publish('fifo',{});Test.Publish('pending',{});Test.Publish('late',{})
        assert(a.calls==22 and b.calls==110,'pending FIFO or late queued operations lost')
        Test.Unsubscribe('active',a,anonymous);Test.Apply();Test.Publish('active',{})
        assert(a.calls==23 and b.calls==210,'renamed callback could not unsubscribe')
    )");
}
void TestKeyRemapFailureAndDispatchBoundary() {
    LuaFixture fixture;
    fixture.Run(R"(
        a=Test.NewComponent(1,'a');b=Test.NewComponent(1,'b')
        function callback(self,event)assert(Test.KeyRemapBusyRejected());self.calls=self.calls+1 end
        Test.Subscribe('active',a,callback);Test.Apply()
    )");
    auto a=luabridge::getGlobal(fixture.state,"a"),b=luabridge::getGlobal(fixture.state,"b");
    EventBus::KeyRemapPlan plan;std::string error;
    Check(!EventBus::PrepareKeyRemap({{a,1,"missing","next"}},plan,error),"key remap accepted stale original identity");
    Check(!EventBus::PrepareKeyRemap({{a,1,"a","next"},{a,1,"a","other"}},plan,error),"key remap accepted duplicate component");
    Check(!EventBus::PrepareKeyRemap({{a,1,"a","next"},{b,1,"b","next"}},plan,error),"key remap accepted duplicate target");
    Check(!EventBus::PrepareKeyRemap({{a,1,"a","a"}},plan,error),"key remap accepted unchanged key");
    EventBus::CommitKeyRemap(std::move(plan));
    fixture.Run("Test.Publish('active',{});assert(a.calls==1)");
    Check(EventBus::PrepareKeyRemap({{a,1,"a","next"}},plan,error),"idle key remap rejected");
    // A discarded preparation has no active/pending effects and retains no key mutation.
    plan=EventBus::KeyRemapPlan();
    fixture.Run("Test.Publish('active',{});assert(a.calls==2)");
    Check(fixture.errors==0&&lua_gettop(fixture.state)==0,"key remap changed callback error/stack handling");
}

} // namespace

int main() {
    try {
        TestSubscriptionBoundaries();
        TestRecursivePublishAndQueuedMutation();
        TestDestroyAndIdentityReuse();
        TestCallbackErrors();
        TestClearAndReset();
        TestReloadActivePendingAndRemoval();
        TestReloadAliasesAndFIFO();
        TestReloadFailuresPreserveBus();
        TestReloadBusyNestedAndErrors();
        TestKeyRemapAliasesFIFOAndLateOperations();
        TestKeyRemapFailureAndDispatchBoundary();
        TestSubscriptionBoundaries(); // A fresh Lua VM cannot inherit the previous bus state.
        std::cout << "EventBus Lua lifecycle and reentrancy regressions passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
