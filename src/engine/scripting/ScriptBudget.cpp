#include "scripting/ScriptBudget.h"
#include "core/World.h"
#include "lua.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {
struct BudgetState {
    ScriptBudget::Limits limits;
    lua_State *vm = nullptr;
    std::size_t used = 0;
    std::size_t peak = 0;
    std::uint64_t remaining = 0;
    unsigned request_depth = 0;
    bool warnings_enabled = false;
    bool warning_continues = false;
    ScriptBudget::Failure failure = ScriptBudget::Failure::None;
};
BudgetState &Budget() { return World::Current().State<BudgetState>(); }

int PanicDiagnostic(lua_State *state) {
    const char *message = lua_type(state, -1) == LUA_TSTRING
        ? lua_tostring(state, -1) : "non-string error object";
    std::fprintf(stderr, "PANIC: unprotected Lua error (%s)\n", message);
    return 0; // Let Lua abort the worker; never throw C++ across this boundary.
}

void WarningDiagnostic(void *userdata, const char *message, int continues) {
    auto &state = *static_cast<BudgetState *>(userdata);
    if (!state.warning_continues && !continues && message[0] == '@') {
        if (std::strcmp(message, "@on") == 0) state.warnings_enabled = true;
        else if (std::strcmp(message, "@off") == 0) state.warnings_enabled = false;
        return;
    }
    if (!state.warnings_enabled) return;
    if (!state.warning_continues) std::fputs("Lua warning: ", stderr);
    std::fputs(message, stderr);
    state.warning_continues = continues != 0;
    if (!state.warning_continues) std::fputc('\n', stderr);
}

void *Allocate(void *userdata, void *pointer, std::size_t old_size,
               std::size_t new_size) {
    auto &state = *static_cast<BudgetState *>(userdata);
    // For a new allocation, Lua uses old_size for an object type tag.
    if (pointer == nullptr) old_size = 0;
    if (new_size == 0) {
        std::free(pointer);
        state.used -= old_size;
        return nullptr;
    }
    if (new_size > old_size) {
        const std::size_t growth = new_size - old_size;
        const bool overflow = growth > std::numeric_limits<std::size_t>::max() - state.used;
        const bool exceeds_limit = state.limits.heap_limit_bytes != 0 &&
            (state.used > state.limits.heap_limit_bytes ||
             growth > state.limits.heap_limit_bytes - state.used);
        if (overflow || exceeds_limit) {
            // Sticky even when a script catches the OOM or an emergency GC
            // makes Lua's retry succeed: this request attempted to exceed quota.
            if (state.failure == ScriptBudget::Failure::None)
                state.failure = ScriptBudget::Failure::HeapLimit;
            return nullptr;
        }
    }
    void *result = std::realloc(pointer, new_size);
    if (result != nullptr) {
        state.used = state.used - old_size + new_size;
        state.peak = std::max(state.peak, state.used);
    }
    return result;
}

const char *FailureText(ScriptBudget::Failure failure) {
    switch (failure) {
    case ScriptBudget::Failure::InstructionLimit: return "Lua instruction budget exceeded";
    case ScriptBudget::Failure::HeapLimit: return "Lua heap allocation budget exceeded";
    default: return "";
    }
}

void CountHook(lua_State *state, lua_Debug *) {
    // No C++ objects requiring destruction may span luaL_error's long jump.
    // The allocator userdata is this VM's own budget, independent of the
    // World bound on the calling thread.
    void *userdata = nullptr;
    lua_getallocf(state, &userdata);
    auto &budget = *static_cast<BudgetState *>(userdata);
    if (budget.request_depth == 0 || budget.limits.instruction_limit == 0) return;
    if (budget.failure == ScriptBudget::Failure::None) {
        const auto interval = static_cast<std::uint64_t>(std::max(lua_gethookcount(state), 1));
        if (budget.remaining > interval) budget.remaining -= interval;
        else {
            budget.remaining = 0;
            budget.failure = ScriptBudget::Failure::InstructionLimit;
        }
    }
    if (budget.failure != ScriptBudget::Failure::None)
        luaL_error(state, "%s", FailureText(budget.failure));
}
} // namespace

namespace ScriptBudget {
void Configure(Limits limits) {
    if (Budget().vm != nullptr || Budget().request_depth != 0)
        throw std::logic_error("configure script limits before creating the Lua VM or entering a request");
    Budget().limits = limits;
    Budget().failure = Failure::None;
}

Limits GetLimits() { return Budget().limits; }

lua_State *NewState() {
    if (Budget().vm != nullptr) throw std::logic_error("only one budgeted Lua VM may exist per World");
    Budget().used = 0;
    Budget().peak = 0;
    Budget().warnings_enabled = false;
    Budget().warning_continues = false;
    Budget().vm = lua_newstate(Allocate, &Budget());
    if (Budget().vm != nullptr) {
        // luaL_newstate normally installs these diagnostics. Preserve them
        // when selecting our own allocator through lua_newstate instead.
        lua_atpanic(Budget().vm, PanicDiagnostic);
        lua_setwarnf(Budget().vm, WarningDiagnostic, &Budget());
    }
    if (Budget().vm != nullptr && Budget().limits.instruction_limit != 0)
        // Each coroutine has its own hook countdown. An interval larger than
        // one would leave short-lived coroutines partially uncharged.
        lua_sethook(Budget().vm, CountHook, LUA_MASKCOUNT, 1);
    return Budget().vm;
}

void CloseState(lua_State *state) {
    if (state == nullptr) return;
    lua_close(state);
    if (state == Budget().vm) Budget().vm = nullptr;
}

void BeginRequest() {
    if (Budget().request_depth++ == 0) {
        Budget().remaining = Budget().limits.instruction_limit;
        Budget().failure = Failure::None;
    }
}

void EndRequest() {
    if (Budget().request_depth != 0) --Budget().request_depth;
}

Failure GetFailure() { return Budget().failure; }
const char *FailureMessage() { return FailureText(Budget().failure); }
std::size_t HeapBytesUsed() { return Budget().used; }
std::size_t PeakHeapBytes() { return Budget().peak; }
} // namespace ScriptBudget
