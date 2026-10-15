// Isolated executable experiment: never include both LuaBridge versions in one TU.
// Deliberate adapters are marked below; no production binding code is changed.
#include "scripting/ScriptBudget.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include "LuaBridge/detail/ClassInfo.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>

#ifndef POCKET_BINDING_CANDIDATE
#error Define POCKET_BINDING_CANDIDATE to 0 or 1 and select the matching include root.
#endif

namespace {
using Ref = luabridge::LuaRef;
int assertions = 0;
void Check(bool value, const char *message) { ++assertions; if (!value) throw std::runtime_error(message); }
// Adapter 1: candidate conversions are expected-like, not unchecked T values.
template <typename T> T Cast(const Ref &reference) {
#if POCKET_BINDING_CANDIDATE
    auto result = reference.template cast<T>();
    if (!result) throw std::runtime_error(result.message());
    return *result;
#else
    return reference.template cast<T>();
#endif
}
struct VM {
    lua_State *state = nullptr;
    lua_CFunction panic = nullptr;
    VM() {
        ScriptBudget::Configure({200000, 16 * 1024 * 1024});
        state = ScriptBudget::NewState(); Check(state != nullptr, "budgeted Lua state creation failed");
        panic = lua_atpanic(state, nullptr); lua_atpanic(state, panic);
        luaL_openlibs(state);
#if POCKET_BINDING_CANDIDATE
        // Adapter 2: normal candidate calls throw only after explicit opt-in.
        luabridge::enableExceptions(state);
        lua_atpanic(state, panic);
#endif
    }
    void Close() { if (state) { ScriptBudget::CloseState(state); state = nullptr; } }
    ~VM() { Close(); }
};
void Lua(lua_State *state, const char *source) {
    const int top = lua_gettop(state);
    ScriptBudget::RequestScope request;
    const int status = luaL_dostring(state, source);
    const std::string error = status == LUA_OK ? "" : lua_tostring(state, -1);
    lua_settop(state, top);
    if (status != LUA_OK) throw std::runtime_error(error);
}
struct Native {
    double value = 3;
    int raw = 7;
    std::string key = "unbound";
    double Readonly() const { return value * 2; }
    double Checked() const { return value; }
    void SetChecked(Ref input) {
        if (input.type() != LUA_TNUMBER) throw std::invalid_argument("number-required");
        const double number = Cast<double>(input);
        if (!std::isfinite(number) || std::abs(number) > 1000) throw std::invalid_argument("native-domain-sentinel");
        value = number;
    }
    void Fail() const { throw std::runtime_error("native-error-sentinel"); }
};
struct Value {
    float x = 0, y = 0;
    Value(float first, float second) : x(first), y(second) {}
};
std::uint64_t current_generation = 1;
int handle_comparisons = 0;
struct Handle {
    std::uint64_t uid = 7, generation = current_generation;
    bool IsValid() const { return uid == 7 && generation == current_generation; }
    int Read() const { if (!IsValid()) throw std::runtime_error("expired-handle-sentinel"); return 42; }
    std::uint64_t UID() const { return uid; }
    std::uint64_t Generation() const { return generation; }
};
int EqualHandles(lua_State *state) {
    ++handle_comparisons;
    if (!luabridge::Stack<Handle>::isInstance(state, 1) || !luabridge::Stack<Handle>::isInstance(state, 2)) {
        lua_pushboolean(state, false); return 1;
    }
    const auto left = Cast<const Handle *>(Ref::fromStack(state, 1));
    const auto right = Cast<const Handle *>(Ref::fromStack(state, 2));
    lua_pushboolean(state, left->uid == right->uid && left->generation == right->generation); return 1;
}
int constructed = 0;
std::array<int, 16> destroyed{};
struct Owned {
    int id = ++constructed;
    Owned() = default;
    Owned(const Owned &) = delete;
    Owned &operator=(const Owned &) = delete;
    ~Owned() { ++destroyed.at(static_cast<std::size_t>(id)); }
    int ID() const { return id; }
};
Owned *borrowed = nullptr;
int native_scopes = 0;
struct Scope { Scope() { ++native_scopes; } ~Scope() { --native_scopes; } };
int Nested(Ref callback) {
    Scope scope;
#if POCKET_BINDING_CANDIDATE
    auto result = callback.call<int>(7);
    if (!result) throw std::runtime_error(result.message());
    return *result;
#else
    return Cast<int>(callback(7));
#endif
}
Handle CurrentHandle() { return {}; }
void Invalidate() { ++current_generation; }
Value CopyValue() { return {1, 2}; }
Owned *Borrow() { return borrowed; }
void Register(lua_State *state) {
    auto native = luabridge::getGlobalNamespace(state).beginClass<Native>("Native");
    native.addConstructor<void (*)()>();
    native.addProperty("raw", &Native::raw); // Intentionally unadapted seam, asserted separately.
#if POCKET_BINDING_CANDIDATE
    // Adapter 3: rc12's two-argument field property is readonly by default.
    native.addPropertyReadWrite("value", &Native::value).addPropertyReadWrite("key", &Native::key);
#else
    native.addProperty("value", &Native::value).addProperty("key", &Native::key);
#endif
    native.addProperty("readonly", &Native::Readonly).addProperty("checked", &Native::Checked, &Native::SetChecked)
        .addFunction("Fail", &Native::Fail).endClass();
    auto value = luabridge::getGlobalNamespace(state).beginClass<Value>("Vector2");
    value.addConstructor<void (*)(float, float)>();
#if POCKET_BINDING_CANDIDATE
    value.addPropertyReadWrite("x", &Value::x).addPropertyReadWrite("y", &Value::y);
#else
    value.addProperty("x", &Value::x).addProperty("y", &Value::y);
#endif
    value.endClass();
    luabridge::getGlobalNamespace(state).beginClass<Handle>("Actor")
        .addProperty("uid", &Handle::UID).addProperty("generation", &Handle::Generation)
        .addProperty("is_valid", &Handle::IsValid).addFunction("IsValid", &Handle::IsValid)
        .addFunction("Read", &Handle::Read).endClass();
    // Reproduce Pocket2D's generation-value __eq seam; rawequal must stay raw.
    lua_rawgetp(state, LUA_REGISTRYINDEX, luabridge::detail::getClassRegistryKey<Handle>());
    lua_pushliteral(state, "__eq"); lua_pushcfunction(state, EqualHandles); lua_rawset(state, -3); lua_pop(state, 1);
    luabridge::getGlobalNamespace(state).beginClass<Owned>("Owned")
        .addConstructor<void (*)()>().addProperty("id", &Owned::ID).endClass();
    luabridge::getGlobalNamespace(state).beginNamespace("Host")
        .addFunction("Handle", &CurrentHandle).addFunction("Invalidate", &Invalidate)
        .addFunction("CopyValue", &CopyValue).addFunction("Borrow", &Borrow).addFunction("Nested", &Nested).endNamespace();
}
// Adapter 4: Lifecycle/EventBus ignore any callback return count. The candidate
// operator() instead requires zero returns. Explicit pcall nresults=0 preserves
// the application policy without ignoring TypeResult failures or changing Lua.
std::string CallIgnoringReturns(const Ref &function) {
    auto *state = function.state(); const int top = lua_gettop(state);
    function.push(state);
    const int status = lua_pcall(state, 0, 0, 0);
    std::string error;
    if (status != LUA_OK) error = lua_tostring(state, -1);
    lua_settop(state, top); return error;
}
void PropertiesAndIdentity(lua_State *state) {
    Lua(state, R"(
        n=Native();n.value=4;assert(n.value==4 and n.readonly==8)
        assert(not pcall(function()n.readonly=99 end));assert(n.readonly==8)
        n.checked=5
        for _,v in ipairs({'6',true,1e309}) do
            local ok=pcall(function()n.checked=v end);assert(not ok and n.value==5)
        end
        n.key='from-lua';assert(n.key=='from-lua')
        raw_writable=pcall(function()n.raw=9 end)
        v1=Host.CopyValue();v2=Host.CopyValue();v1.x=9;assert(v2.x==1)
        vector=Vector2(3,4);assert(vector.x==3 and vector.y==4)
        h1=Host.Handle();h2=Host.Handle();assert(h1==h2 and h1:IsValid() and h1:Read()==42)
        assert(not pcall(function()h1.generation=100 end))
        assert(not pcall(function()h1.uid=8 end))
        eq_calls=0
        local meta={__eq=function()eq_calls=eq_calls+1;return true end}
        t1=setmetatable({},meta);t2=setmetatable({},meta)
    )");
    const bool raw = Cast<bool>(luabridge::getGlobal(state, "raw_writable"));
    Check(raw == !bool(POCKET_BINDING_CANDIDATE), "raw two-argument property behavior differs from inspected pin");
    auto native = luabridge::getGlobal(state, "n"); native["key"] = std::string("engine-bound");
    Check(Cast<std::string>(native["key"]) == "engine-bound", "engine key injection lost writability");
    auto first = luabridge::getGlobal(state, "h1"); auto alias = first; auto second = luabridge::getGlobal(state, "h2");
    const int comparisons = handle_comparisons;
    Check(first.rawequal(alias) && !first.rawequal(second), "LuaRef raw identity did not distinguish handle values from references");
    Check(handle_comparisons == comparisons, "raw handle identity invoked equality metamethod");
    Check(!luabridge::getGlobal(state, "t1").rawequal(luabridge::getGlobal(state, "t2")), "raw table identity invoked __eq");
    Check(Cast<int>(luabridge::getGlobal(state, "eq_calls")) == 0, "table rawequal ran user code");
    Lua(state, R"(
        Host.Invalidate();h3=Host.Handle()
        assert(not h1:IsValid() and h3:IsValid() and h1~=h3)
        local ok,err=pcall(function()return h1:Read()end)
        assert(not ok and tostring(err):find('expired%-handle%-sentinel'))
        local native_ok,native_err=pcall(function()n:Fail()end)
        assert(not native_ok and tostring(native_err):find('native%-error%-sentinel'))
    )");
}
void BooleanConversion(lua_State *state) {
    Lua(state, "disabled=false;enabled=true;wrong_bool={}");
    auto disabled = luabridge::getGlobal(state, "disabled");
    auto enabled = luabridge::getGlobal(state, "enabled");
    auto wrong = luabridge::getGlobal(state, "wrong_bool");
    const bool raw_enabled_policy = !disabled.isBool() || disabled.cast<bool>();
    Check(raw_enabled_policy == bool(POCKET_BINDING_CANDIDATE),
          "exact Shared.cpp boolean expression no longer exposes expected contextual-bool seam");
    Check((!disabled.isBool() || Cast<bool>(disabled)) == false &&
          (!enabled.isBool() || Cast<bool>(enabled)) == true &&
          (!wrong.isBool() || Cast<bool>(wrong)) == true, "adapted shared enabled policy changed");
    Check(disabled.isBool() && !Cast<bool>(disabled) && Cast<bool>(enabled),
          "checked conversion did not preserve false/true values");
#if POCKET_BINDING_CANDIDATE
    // This expression compiles in real Shared.cpp but negates conversion SUCCESS,
    // not the contained Lua false. Explicitly expose, never hide, that seam.
    Check(!disabled.cast<bool>() == false, "candidate TypeResult contextual-bool seam changed");
    const int top = lua_gettop(state);
    const auto invalid_integer = wrong.cast<int>();
    Check(!invalid_integer && lua_gettop(state) == top, "candidate invalid conversion did not return checked error with stack intact");
#else
    Check(!disabled.cast<bool>() == true, "baseline false conversion behavior changed");
#endif
    // STRICT_STACK_CONVERSIONS=0 intentionally preserves Lua truthiness in both
    // libraries. Explicit API validators must still reject wrong Lua types.
    Check(!wrong.isBool() && Cast<bool>(wrong), "default bool truthiness policy changed");
    const auto strict_bool = [](const Ref &value) {
        if (!value.isBool()) throw std::invalid_argument("boolean-required");
        return Cast<bool>(value);
    };
    bool rejected = false;
    try { (void)strict_bool(wrong); } catch (const std::invalid_argument &) { rejected = true; }
    Check(rejected && !strict_bool(disabled) && strict_bool(enabled), "explicit boolean validation/unwrap failed");
}
void Calls(lua_State *state) {
    ScriptBudget::RequestScope request;
    Lua(state, R"(
        function zero() end
        function one() return 42 end
        function many() return 42,'second',true end
        function failure() error('lua-call-sentinel') end
        function nested() return Host.Nested(function(n) return n+1,'ignored' end) end
        function nested_failure() return Host.Nested(function() error('nested-call-sentinel') end) end
    )");
    lua_pushliteral(state, "preserved-stack-marker"); lua_pushinteger(state, 917);
    const int top = lua_gettop(state);
    for (const char *name : {"zero", "one", "many"}) {
        auto function = luabridge::getGlobal(state, name);
#if POCKET_BINDING_CANDIDATE
        auto result = function();
        Check(bool(result) == (std::string(name) == "zero"), "candidate void call return-arity behavior differs from pin");
#else
        auto result = function();
        Check(std::string(name) == "zero" ? result.isNil() : Cast<int>(result) == 42, "baseline first-return behavior changed");
#endif
        Check(lua_gettop(state) == top, "raw callback call changed nonzero stack");
        Check(CallIgnoringReturns(function).empty() && lua_gettop(state) == top, "explicit ignored-return adapter failed");
    }
#if POCKET_BINDING_CANDIDATE
    auto decoded = luabridge::getGlobal(state, "many").call<std::tuple<int, std::string, bool>>();
    Check(decoded && std::get<0>(*decoded) == 42 && std::get<1>(*decoded) == "second" && std::get<2>(*decoded),
          "candidate typed multi-return decoding failed");
#endif
    auto nested = luabridge::getGlobal(state, "nested");
#if POCKET_BINDING_CANDIDATE
    auto nested_result = nested.call<int>(); Check(nested_result && *nested_result == 8, "candidate nested callback result wrong");
#else
    Check(Cast<int>(nested()) == 8, "baseline nested callback result wrong");
#endif
    Check(native_scopes == 0 && lua_gettop(state) == top, "nested success leaked C++ scope or Lua stack");
    for (const char *name : {"failure", "nested_failure"}) {
        const auto function = luabridge::getGlobal(state, name); std::string diagnostic;
        try {
#if POCKET_BINDING_CANDIDATE
            auto result = function(); if (!result) diagnostic = result.message();
#else
            (void)function();
#endif
        } catch (const luabridge::LuaException &error) { diagnostic = error.what(); }
        Check(diagnostic.find(name == std::string("failure") ? "lua-call-sentinel" : "nested-call-sentinel") != std::string::npos,
              "binding call lost original Lua error sentinel");
        // Baseline callers restore their saved stack after catch; candidate
        // calls use StackRestore. Neither policy may corrupt existing entries.
        lua_settop(state, top);
        Check(native_scopes == 0 && lua_tointeger(state, top) == 917 &&
              std::string(lua_tostring(state, top - 1)) == "preserved-stack-marker", "error corrupted outer stack or skipped C++ scope destruction");
        Check(!CallIgnoringReturns(function).empty() && lua_gettop(state) == top, "ignored-return adapter swallowed callback failure");
    }
    lua_pop(state, 2);
}
void Run() {
    int borrowed_id = 0, retained_id = 0, shutdown_id = 0;
    {
        Owned external; borrowed = &external; borrowed_id = external.id;
        VM vm; auto *state = vm.state; Register(state);
        // Registration/exception opt-in must not replace the real host panic policy.
        lua_atpanic(state, vm.panic);
        Check(lua_gettop(state) == 0, "registration leaked stack entries");
        PropertiesAndIdentity(state); BooleanConversion(state); Calls(state);
        Lua(state, "owned=Owned();kept=Owned();borrowed_ref=Host.Borrow();borrowed_alias=Host.Borrow()");
        {
            auto retained = luabridge::getGlobal(state, "owned"); retained_id = Cast<int>(retained["id"]);
            shutdown_id = Cast<int>(luabridge::getGlobal(state, "kept")["id"]);
            Check(Cast<Owned *>(luabridge::getGlobal(state, "borrowed_ref")) == &external &&
                  Cast<Owned *>(luabridge::getGlobal(state, "borrowed_alias")) == &external, "borrowed pointer identity changed");
            Lua(state, "owned=nil;borrowed_ref=nil;borrowed_alias=nil;collectgarbage('collect');collectgarbage('collect')");
            Check(destroyed[retained_id] == 0 && destroyed[borrowed_id] == 0, "LuaRef failed to retain ownership or borrowed pointer was deleted");
        }
        Lua(state, "collectgarbage('collect');collectgarbage('collect')");
        Check(destroyed[retained_id] == 1 && destroyed[shutdown_id] == 0 && destroyed[borrowed_id] == 0,
              "owned userdata was not collected exactly once");
        Check(lua_gettop(state) == 0 && ScriptBudget::HeapBytesUsed() > 0, "probe did not use the real tracked Lua allocator");
        vm.Close();
        Check(destroyed[retained_id] == 1 && destroyed[shutdown_id] == 1 && destroyed[borrowed_id] == 0,
              "VM shutdown double-destroyed owned userdata or destroyed borrowed native value");
        Check(ScriptBudget::HeapBytesUsed() == 0, "VM shutdown retained tracked Lua allocation");
        borrowed = nullptr;
    }
    Check(constructed == 3 && destroyed[borrowed_id] == 1 && destroyed[retained_id] == 1 && destroyed[shutdown_id] == 1,
          "constructor/destructor accounting differs from owned versus borrowed contract");
}
}
int main() {
    try {
        Run();
        std::cout << "binding_semantics variant=" << (POCKET_BINDING_CANDIDATE ? "LuaBridge3-53e031b" : "LuaBridge2.7")
                  << " assertions=" << assertions << " adapted_contract=PASS raw_member_writable=" << !bool(POCKET_BINDING_CANDIDATE)
                  << " raw_operator_accepts_return_values=" << !bool(POCKET_BINDING_CANDIDATE) << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "binding_semantics variant=" << POCKET_BINDING_CANDIDATE << ": " << error.what() << '\n'; return 1;
    }
}
