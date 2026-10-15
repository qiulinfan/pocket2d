// Isolated binding experiment. Production code and vendor headers are unchanged.
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include "scripting/ScriptBudget.h"
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef POCKET_BINDING_CANDIDATE
#define POCKET_BINDING_CANDIDATE 0
#endif
namespace {
void Check(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
struct StackScope { lua_State *state; int top; ~StackScope() { lua_settop(state, top); } };
struct VM {
    lua_State *state;
    explicit VM(ScriptBudget::Limits limits = {}) {
        ScriptBudget::Configure(limits); state = ScriptBudget::NewState(); Check(state, "new Lua state failed");
        luaL_openlibs(state);
        lua_pushliteral(state, "STACK_SENTINEL"); lua_pushinteger(state, 4242);
    }
    ~VM() { ScriptBudget::CloseState(state); }
    void Stack() const {
        Check(lua_gettop(state) == 2 && lua_type(state, 1) == LUA_TSTRING &&
              std::string(lua_tostring(state, 1)) == "STACK_SENTINEL" && lua_tointeger(state, 2) == 4242,
              "protected boundary damaged preexisting Lua stack");
    }
};
// All C trampolines crossing lua_error/longjmp have only trivial local state.
int Traceback(lua_State *state);
int ReadField(lua_State *state) { lua_settop(state, 2); lua_gettable(state, 1); return 1; }
struct Outcome { int status = LUA_OK; std::string error; bool boolean = false; };
Outcome Finish(lua_State *state, int status) {
    Outcome out; out.status = status;
    if (status != LUA_OK) {
        std::size_t length = 0;
        const char *text = lua_tolstring(state, -1, &length);
        out.error = text ? std::string(text, length) : "non-string Lua error";
    } else out.boolean = lua_toboolean(state, -1);
    return out;
}
Outcome Run(lua_State *state, std::string_view text, const char *name, bool traceback = true) {
    const StackScope stack{state, lua_gettop(state)};
    int handler = 0;
    if (traceback) { lua_pushcfunction(state, Traceback); handler = lua_gettop(state); }
    int status = luaL_loadbuffer(state, text.data(), text.size(), name);
    if (status == LUA_OK) status = lua_pcall(state, 0, 1, handler);
    return Finish(state, status);
}
Outcome Call(lua_State *state, const char *name, bool traceback = true) {
    const StackScope stack{state, lua_gettop(state)};
    int handler = 0;
    if (traceback) { lua_pushcfunction(state, Traceback); handler = lua_gettop(state); }
    lua_getglobal(state, name);
    return Finish(state, lua_pcall(state, 0, 1, handler));
}
void Load(lua_State *state, std::string_view source, const char *name) {
    auto result = Run(state, source, name);
    if (result.status != LUA_OK) throw std::runtime_error(result.error);
}
// Library-specific API spelling is confined to this experimental adapter.
// LuaBridge3 does not provide LuaException::pcall; enableExceptions is explicit.
void LibraryPcall(lua_State *state, int arguments, int results, int handler) {
#if POCKET_BINDING_CANDIDATE
    const int status = luabridge::pcall(state, arguments, results, handler);
    Check(status == LUA_OK, "candidate pcall failed without enabled exceptions");
#else
    luabridge::LuaException::pcall(state, arguments, results, handler);
#endif
}
luabridge::LuaRef ProtectedField(const luabridge::LuaRef &object, const char *name) {
    auto *state = object.state(); const StackScope stack{state, lua_gettop(state)};
    lua_pushcfunction(state, Traceback); const int handler = lua_gettop(state);
    lua_pushcfunction(state, ReadField); object.push(state); lua_pushstring(state, name);
    LibraryPcall(state, 2, 1, handler);
    return luabridge::LuaRef::fromStack(state, -1);
}
// A bounded UTF-8 reporting adapter is project work, not a binding guarantee.
// Invalid bytes and NUL are escaped; truncation never splits a valid codepoint.
std::size_t Sequence(std::string_view text, std::size_t offset) {
    const auto byte = static_cast<unsigned char>(text[offset]);
    if (byte < 0x80) return byte == 0 ? 0 : 1;
    const std::size_t length = byte >= 0xc2 && byte <= 0xdf ? 2 : byte >= 0xe0 && byte <= 0xef ? 3 : byte >= 0xf0 && byte <= 0xf4 ? 4 : 0;
    if (!length || offset + length > text.size()) return 0;
    for (std::size_t i = 1; i < length; ++i)
        if ((static_cast<unsigned char>(text[offset+i]) & 0xc0) != 0x80) return 0;
    const auto second = static_cast<unsigned char>(text[offset+1]);
    if ((byte == 0xe0 && second < 0xa0) || (byte == 0xed && second >= 0xa0) ||
        (byte == 0xf0 && second < 0x90) || (byte == 0xf4 && second >= 0x90)) return 0;
    return length;
}
// Sanitize before luaL_traceback's C-string interface can truncate an embedded
// NUL. Fixed storage/trivial locals are deliberate: luaL_traceback can longjmp.
int Traceback(lua_State *state) {
    std::size_t length = 0;
    const char *bytes = lua_type(state, 1) == LUA_TSTRING ? lua_tolstring(state, 1, &length) : nullptr;
    const std::string_view input = bytes ? std::string_view(bytes, length) : std::string_view("non-string Lua error");
    constexpr std::string_view marker = "...[truncated]";
    constexpr std::size_t budget = 4096 - marker.size();
    char output[4097]{}; std::size_t used = 0, cursor = 0;
    while (cursor < input.size()) {
        const auto count = Sequence(input, cursor);
        if (used + (count ? count : 4) > budget) break;
        if (count) { for (std::size_t i=0;i<count;++i) output[used++]=input[cursor++]; }
        else {
            constexpr char hex[]="0123456789ABCDEF";
            const auto byte=static_cast<unsigned char>(input[cursor++]);
            output[used++]='\\';output[used++]='x';output[used++]=hex[byte>>4];output[used++]=hex[byte&15];
        }
    }
    if (cursor != input.size()) for (const char c : marker) output[used++]=c;
    output[used]='\0';
    luaL_traceback(state, state, output, 1); return 1;
}
std::string Bounded(std::string_view text) {
    constexpr std::size_t cap = 4096; constexpr std::string_view marker = "...[truncated]";
    const std::size_t budget = cap - marker.size(); std::string result;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        const auto length = Sequence(text, cursor);
        if (result.size() + (length ? length : 4) > budget) break;
        if (length) { result.append(text.substr(cursor, length)); cursor += length; }
        else { constexpr char hex[] = "0123456789ABCDEF"; const auto byte = static_cast<unsigned char>(text[cursor++]);
            result += "\\x"; result += hex[byte >> 4]; result += hex[byte & 15]; }
    }
    if (cursor != text.size()) result += marker;
    return result;
}
void ValidateUTF8(lua_State *state, const std::string &text) {
    const StackScope stack{state, lua_gettop(state)};
    lua_getglobal(state, "utf8"); lua_getfield(state, -1, "len"); lua_pushlstring(state, text.data(), text.size());
    Check(lua_pcall(state, 1, 1, 0) == LUA_OK && lua_isinteger(state, -1), "bounded diagnostic is not valid UTF-8");
    Check(text.size() <= 4096 && text.find('\0') == std::string::npos, "diagnostic is unbounded or contains NUL");
}
int NativeFailure() { throw std::runtime_error("NATIVE_SENTINEL"); }
#if POCKET_BINDING_CANDIDATE
int RawNativeFailure(lua_State *) { throw std::runtime_error("RAW_NATIVE_SENTINEL"); }
#else
// Baseline project-style explicit C boundary: finish C++ unwinding before
// invoking lua_error. Never intentionally unwind C++ across C-built Lua.
int RawNativeFailure(lua_State *state) {
    char message[128]{};
    try { throw std::runtime_error("RAW_NATIVE_SENTINEL"); }
    catch (const std::exception &error) { std::snprintf(message, sizeof(message), "%s", error.what()); }
    lua_pushstring(state, message); return lua_error(state);
}
#endif
struct Thing { int Explodes() const { throw std::runtime_error("NATIVE_GETTER_SENTINEL"); } };
void TestDefaultCallbackPolicy() {
    VM vm; Load(vm.state, "function broken() error('DEFAULT_SENTINEL') end", "@default.lua");
    {
        auto callback = luabridge::getGlobal(vm.state, "broken"); bool caught = false;
#if POCKET_BINDING_CANDIDATE
        const auto result = callback();
        Check(!result, "candidate default callback did not report failure");
        Check(result.message().find("DEFAULT_SENTINEL") == std::string::npos,
              "candidate error category unexpectedly contains original Lua payload; reassess adapter requirement");
        std::cout << "default_callback=result_failure catch_only_accounting=0 original_payload_in_category=0\n";
#else
        try { callback(); } catch (const luabridge::LuaException &error) {
            caught = std::string(error.what()).find("DEFAULT_SENTINEL") != std::string::npos;
        }
        Check(caught, "baseline callback exception lost Lua sentinel");
        std::cout << "default_callback=exception catch_only_accounting=1 original_payload_in_exception=1\n";
#endif
        (void)caught;
    }
    const int raw_stack_delta = lua_gettop(vm.state) - 2;
#if POCKET_BINDING_CANDIDATE
    Check(raw_stack_delta == 0, "candidate default call did not restore stack");
#else
    Check(raw_stack_delta == 1, "baseline default error stack behavior changed; reassess host reset requirement");
#endif
    Check(lua_type(vm.state, 1) == LUA_TSTRING && std::string(lua_tostring(vm.state, 1)) == "STACK_SENTINEL" &&
          lua_tointeger(vm.state, 2) == 4242, "raw library error damaged preexisting values");
    std::cout << "default_callback_error_stack_delta=" << raw_stack_delta << " host_stack_reset_required=" << (raw_stack_delta != 0) << '\n';
    lua_settop(vm.state, 2); vm.Stack();
}
void TestHostErrors() {
    Thing thing; VM vm({0, 8*1024*1024}); auto *state = vm.state;
    // Save/restore the project's aborting panic callback. Enabling library
    // exceptions otherwise replaces it with a C++-throwing panic callback.
    const auto project_panic = lua_atpanic(state, nullptr); lua_atpanic(state, project_panic);
    luabridge::getGlobalNamespace(state)
        .addFunction("NativeFailure", &NativeFailure)
        .addFunction("RawNativeFailure", &RawNativeFailure)
        .beginClass<Thing>("Thing").addProperty("explodes", &Thing::Explodes).endClass();
    luabridge::setGlobal(state, &thing, "native");
    luabridge::enableExceptions(state);
    const auto binding_panic = lua_atpanic(state, project_panic);
    std::cout << "exception_enable_replaces_project_panic=" << (binding_panic != project_panic)
              << " adapter_restores_project_panic=1\n";
    Load(state, "function broken()\n error('LUA_SENTINEL')\nend\n"
                "function native_error()\n NativeFailure()\nend\n"
                "function native_getter()\n return native.explodes\nend\n"
                "function raw_native_error()\n RawNativeFailure()\nend\n"
                "object=setmetatable({}, {__index=function() error('GETTER_SENTINEL') end})\n"
                "good={OnUpdate=function(self) self.calls=(self.calls or 0)+1 end}\n"
                "bad={OnUpdate=function(self) error('CALLBACK_SENTINEL') end}\n", "@errors.lua");
    for (const auto name : {"broken", "native_error", "native_getter", "raw_native_error"}) {
        const auto result = Call(state, name);
        Check(result.status != LUA_OK && result.error.find("SENTINEL") != std::string::npos &&
              result.error.find("errors.lua:") != std::string::npos && result.error.find("stack traceback:") != std::string::npos,
              "protected callback lost native/Lua sentinel, source or traceback");
        if (std::string_view(name) == "broken") Check(result.error.find("errors.lua:2:") != std::string::npos, "Lua source line changed");
        ValidateUTF8(state, Bounded(result.error)); vm.Stack();
    }
    {
        auto object = luabridge::getGlobal(state, "object"); bool caught = false;
        try { (void)ProtectedField(object, "OnUpdate"); }
        catch (const luabridge::LuaException &error) {
            const std::string message = error.what(); caught = message.find("GETTER_SENTINEL") != std::string::npos &&
                message.find("stack traceback:") != std::string::npos;
        }
        Check(caught, "throwing getter escaped/lost protected-field diagnostic"); vm.Stack();
        int callbacks = 0, errors = 0;
        for (const auto name : {"bad", "object", "good", "bad", "good"}) {
            {
            const StackScope callback_stack{state, lua_gettop(state)};
            auto component = luabridge::getGlobal(state, name);
            try {
                auto callback = ProtectedField(component, "OnUpdate");
                if (callback.isFunction()) {
#if POCKET_BINDING_CANDIDATE
                    const auto result = callback(component); Check(bool(result), "enabled candidate exception path returned unhandled failure");
#else
                    callback(component);
#endif
                    ++callbacks;
                }
            } catch (const luabridge::LuaException &error) {
                ++errors; ValidateUTF8(state, Bounded(error.what()));
            }
            }
            vm.Stack();
        }
        Check(callbacks == 2 && errors == 3, "host callback accounting dropped or duplicated errors");
        std::cout << "protected_callbacks_success=2 errors=3 getter_and_native_errors_preserved=1\n";
    }
    const auto large = Run(state, "error(string.rep('界',5000)..'TAIL_SENTINEL')", "@large.lua", false);
    const auto bounded = Bounded(large.error); ValidateUTF8(state, bounded);
    Check(large.error.size() > 4096 && bounded.find("[truncated]") != std::string::npos, "large diagnostic truncation not exercised");
    const auto invalid = Run(state, "error('HEAD'..string.char(255,0)..'TAIL_SENTINEL')", "@bytes.lua", false);
    const auto escaped = Bounded(invalid.error); ValidateUTF8(state, escaped);
    Check(escaped.find("\\xFF\\x00TAIL_SENTINEL") != std::string::npos, "raw protected adapter lost invalid bytes/NUL suffix");
    const auto traced_invalid = Run(state, "error('HEAD'..string.char(255,0)..'TAIL_SENTINEL')", "@bytes.lua", true);
    const auto traced_bounded = Bounded(traced_invalid.error); ValidateUTF8(state, traced_bounded);
    Check(traced_bounded.find("\\xFF\\x00TAIL_SENTINEL") != std::string::npos &&
          traced_bounded.find("stack traceback:") != std::string::npos,
          "protected traceback adapter truncated embedded-NUL payload");
    vm.Stack(); std::cout << "bounded_utf8_adapter=1 cap_bytes=4096 raw_error_payload_preserved=1\n";
}
void TestInstructionBudgets() {
    for (const bool coroutines : {false, true}) {
        VM vm({1200, 4*1024*1024});
        Load(vm.state, coroutines ?
            "function run() while true do local co=coroutine.create(function() return 1 end); coroutine.resume(co) end end" :
            "function run() while true do pcall(function() while true do end end) end end", "@budget.lua");
        {
            ScriptBudget::RequestScope request;
            const auto result = Call(vm.state, "run", false);
            Check(result.status != LUA_OK && ScriptBudget::GetFailure() == ScriptBudget::Failure::InstructionLimit,
                  "instruction limit escaped through coroutine/pcall");
        }
        vm.Stack();
        {
            ScriptBudget::RequestScope reset;
            Check(ScriptBudget::GetFailure() == ScriptBudget::Failure::None, "new request did not reset sticky failure");
            Check(Run(vm.state, "return true", "@after.lua").status == LUA_OK, "VM cannot perform a protected call after instruction failure");
        }
    }
    {
        VM vm({1000, 4*1024*1024});
        Load(vm.state, "function burn() local n=0;for i=1,60 do n=n+i end;return n end", "@nested.lua");
        ScriptBudget::RequestScope outer; int completed = 0;
        for (int i=0; i<100; ++i) {
            ScriptBudget::RequestScope inner;
            const auto result = Call(vm.state, "burn", false);
            if (result.status != LUA_OK) break;
            ++completed;
        }
        Check(completed > 0 && completed < 20 && ScriptBudget::GetFailure() == ScriptBudget::Failure::InstructionLimit,
              "nested RequestScope replenished the outer instruction budget");
        vm.Stack(); std::cout << "nested_request_completed_calls=" << completed << " outer_budget_preserved=1\n";
    }
    Check(ScriptBudget::HeapBytesUsed() == 0, "VM close leaked accounted Lua bytes");
    std::cout << "instruction_pcall_coroutine_limits=1 request_reset=1 close_accounting=0\n";
}
void TestHeapBudgets() {
    constexpr std::size_t cap = 256*1024;
    for (const bool swallowed : {false, true}) {
        VM vm({0, cap});
        Load(vm.state, swallowed ?
            "function exhaust() local ok=pcall(function() return string.rep('x',1024*1024) end);return ok end" :
            "function exhaust() return string.rep('x',1024*1024) end", "@heap.lua");
        {
            ScriptBudget::RequestScope request;
            const auto result = Call(vm.state, "exhaust", false);
            Check(ScriptBudget::GetFailure() == ScriptBudget::Failure::HeapLimit, "heap allocator did not preserve quota failure");
            Check(swallowed ? result.status == LUA_OK && !result.boolean : result.status == LUA_ERRMEM,
                  "unexpected Lua protected OOM behavior");
            Check(ScriptBudget::HeapBytesUsed() <= cap && ScriptBudget::PeakHeapBytes() <= cap, "heap accounting exceeded configured cap");
        }
        vm.Stack();
    }
    Check(ScriptBudget::HeapBytesUsed() == 0, "OOM VM close left accounted allocations");
    {
        VM reset({2000, cap}); ScriptBudget::RequestScope request;
        Check(Run(reset.state, "return 6*7==42", "@reset.lua").boolean && ScriptBudget::GetFailure() == ScriptBudget::Failure::None,
              "close/newState reset failed after OOM"); reset.Stack();
    }
    Check(ScriptBudget::HeapBytesUsed() == 0, "new VM cleanup accounting failed");
    std::cout << "heap_direct_and_swallowed_failure=1 close_new_state_reset=1\n";
}
}
int main() {
    try {
        std::cout << "budget_errors candidate=" << POCKET_BINDING_CANDIDATE << " lua=" << LUA_RELEASE << '\n';
#if POCKET_BINDING_CANDIDATE
        std::cout << "candidate_safe_c_exception_handling=" << LUABRIDGE_SAFE_LUA_C_EXCEPTION_HANDLING << '\n';
        Check(LUABRIDGE_SAFE_LUA_C_EXCEPTION_HANDLING == 1,
              "raw C-function exception probe requires safe C exception handling; refusing unsafe run");
#endif
        TestDefaultCallbackPolicy(); TestHostErrors(); TestInstructionBudgets(); TestHeapBudgets();
        std::cout << "budget_errors PASS\n"; return 0;
    } catch (const std::exception &error) {
        std::cerr << "budget_errors FAIL: " << error.what() << '\n'; return 1;
    }
}
