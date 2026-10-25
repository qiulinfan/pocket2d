#include "scripting/ScriptBudget.h"
#include "lua.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

class State {
public:
    lua_State *vm = nullptr;
    explicit State(ScriptBudget::Limits limits) {
        ScriptBudget::Configure(limits);
        vm = ScriptBudget::NewState();
        Check(vm != nullptr, "budgeted Lua state creation failed");
        luaL_openlibs(vm);
    }
    ~State() { ScriptBudget::CloseState(vm); }
    int Run(const char *source) {
        int status = luaL_loadstring(vm, source);
        if (status == LUA_OK) status = lua_pcall(vm, 0, 0, 0);
        lua_settop(vm, 0);
        return status;
    }
};

void TestInstructionsAndNextRequest() {
    State state({20'000, 4 * 1024 * 1024});
    {
        ScriptBudget::RequestScope request;
        Check(state.Run("local n=0; for i=1,100000000 do n=n+i end") == LUA_ERRRUN,
              "long Lua loop must fail through protected execution");
        Check(ScriptBudget::GetFailure() == ScriptBudget::Failure::InstructionLimit,
              "instruction failure must remain visible outside Lua");
    }
    {
        ScriptBudget::RequestScope request;
        Check(state.Run("assert(2+3 == 5)") == LUA_OK, "fresh request must have a fresh budget");
        Check(ScriptBudget::GetFailure() == ScriptBudget::Failure::None,
              "new request must clear previous failure state");
    }
}

void TestNestedScopesDoNotReplenish() {
    State state({3000, 4 * 1024 * 1024});
    ScriptBudget::RequestScope outer;
    int completed = 0;
    for (; completed < 50; ++completed) {
        ScriptBudget::RequestScope nested;
        if (state.Run("local n=0; for i=1,100 do n=n+i end") != LUA_OK) break;
    }
    Check(completed < 50 && ScriptBudget::GetFailure() == ScriptBudget::Failure::InstructionLimit,
          "nested callbacks must consume the outer request's remaining quota");
}

void TestCoroutineAndSwallowedError() {
    State state({10'000, 4 * 1024 * 1024});
    Check(state.Run("co = coroutine.create(function() while true do end end)") == LUA_OK,
          "coroutine fixture must be created outside the request");
    {
        ScriptBudget::RequestScope request;
        state.Run("local ok, message = coroutine.resume(co)");
        Check(ScriptBudget::GetFailure() == ScriptBudget::Failure::InstructionLimit,
              "existing coroutines must share the request budget even if resume catches the error");
    }
    {
        ScriptBudget::RequestScope request;
        state.Run("pcall(function() while true do end end)");
        Check(ScriptBudget::GetFailure() == ScriptBudget::Failure::InstructionLimit,
              "pcall must not hide a budget failure from the host");
    }
}

void TestShortCoroutinesShareInstructionQuota() {
    State state({1000, 4 * 1024 * 1024});
    // The child body takes fewer than the old 1000-instruction hook interval.
    // Creating many such children must not reset or discard their instruction charge.
    ScriptBudget::RequestScope request;
    state.Run(R"lua(
        completed = 0
        for index = 1, 100 do
            local child = coroutine.create(function()
                local total = 0
                for n = 1, 50 do total = total + n end
                return total
            end)
            local ok = coroutine.resume(child)
            if not ok then break end
            completed = completed + 1
        end
    )lua");
    Check(ScriptBudget::GetFailure() == ScriptBudget::Failure::InstructionLimit,
          "short coroutine instructions must consume the shared quota");
    lua_getglobal(state.vm, "completed");
    const int completed = static_cast<int>(lua_tointeger(state.vm, -1));
    lua_pop(state.vm, 1);
    Check(completed < 15, "child instructions were discarded between coroutine invocations");
}

void TestHeapAndCleanup() {
    constexpr std::size_t limit = 256 * 1024;
    {
        State state({0, limit});
        Check(ScriptBudget::HeapBytesUsed() > 0, "Lua allocations must be accounted");
        ScriptBudget::RequestScope request;
        state.Run("pcall(function() retained = string.rep('x', 1024*1024) end)");
        Check(ScriptBudget::GetFailure() == ScriptBudget::Failure::HeapLimit,
              "caught Lua allocation failure must remain visible to the host");
        Check(ScriptBudget::HeapBytesUsed() <= limit && ScriptBudget::PeakHeapBytes() <= limit,
              "Lua allocator must never commit bytes beyond its cap");
    }
    Check(ScriptBudget::HeapBytesUsed() == 0, "VM cleanup must release accounted allocations");
    ScriptBudget::Configure({0, 1});
    {
        ScriptBudget::RequestScope request;
        Check(ScriptBudget::NewState() == nullptr, "tiny heap cap must reject VM creation");
        Check(ScriptBudget::GetFailure() == ScriptBudget::Failure::HeapLimit,
              "initial allocation failure must preserve its budget cause");
    }
    Check(ScriptBudget::HeapBytesUsed() == 0, "failed VM creation must not leak accounted bytes");
}

void TestUnlimitedInteractiveDefault() {
    State state({0, 0});
    const auto panic = lua_atpanic(state.vm, nullptr);
    Check(panic != nullptr, "custom allocator state must retain panic diagnostics");
    lua_atpanic(state.vm, panic);
    Check(lua_gethook(state.vm) == nullptr, "unlimited mode must not install an instruction hook");
    ScriptBudget::RequestScope request;
    Check(state.Run("local n=0; for i=1,50000 do n=n+i end; assert(n>0)") == LUA_OK,
          "unlimited trusted execution must preserve existing behavior");
}
} // namespace

int main() {
    try {
        TestInstructionsAndNextRequest();
        TestNestedScopesDoNotReplenish();
        TestCoroutineAndSwallowedError();
        TestShortCoroutinesShareInstructionQuota();
        TestHeapAndCleanup();
        TestUnlimitedInteractiveDefault();
        std::cout << "Lua instruction, coroutine, nested-scope and heap budget regressions passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
