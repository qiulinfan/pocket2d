#include "scripting/ScriptBudget.h"
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
BudgetState budget;

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

void CountHook(lua_State *state, lua_Debug *) {
    // No C++ objects requiring destruction may span luaL_error's long jump.
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
        luaL_error(state, "%s", ScriptBudget::FailureMessage());
}
} // namespace

namespace ScriptBudget {
void Configure(Limits limits) {
    if (budget.vm != nullptr || budget.request_depth != 0)
        throw std::logic_error("configure script limits before creating the Lua VM or entering a request");
    budget.limits = limits;
    budget.failure = Failure::None;
}

Limits GetLimits() { return budget.limits; }

lua_State *NewState() {
    if (budget.vm != nullptr) throw std::logic_error("only one budgeted Lua VM may exist per process");
    budget.used = 0;
    budget.peak = 0;
    budget.warnings_enabled = false;
    budget.warning_continues = false;
    budget.vm = lua_newstate(Allocate, &budget);
    if (budget.vm != nullptr) {
        // luaL_newstate normally installs these diagnostics. Preserve them
        // when selecting our own allocator through lua_newstate instead.
        lua_atpanic(budget.vm, PanicDiagnostic);
        lua_setwarnf(budget.vm, WarningDiagnostic, &budget);
    }
    if (budget.vm != nullptr && budget.limits.instruction_limit != 0)
        // Each coroutine has its own hook countdown. An interval larger than
        // one would leave short-lived coroutines partially uncharged.
        lua_sethook(budget.vm, CountHook, LUA_MASKCOUNT, 1);
    return budget.vm;
}

void CloseState(lua_State *state) {
    if (state == nullptr) return;
    lua_close(state);
    if (state == budget.vm) budget.vm = nullptr;
}

void BeginRequest() {
    if (budget.request_depth++ == 0) {
        budget.remaining = budget.limits.instruction_limit;
        budget.failure = Failure::None;
    }
}

void EndRequest() {
    if (budget.request_depth != 0) --budget.request_depth;
}

Failure GetFailure() { return budget.failure; }
const char *FailureMessage() {
    switch (budget.failure) {
    case Failure::InstructionLimit: return "Lua instruction budget exceeded";
    case Failure::HeapLimit: return "Lua heap allocation budget exceeded";
    default: return "";
    }
}
std::size_t HeapBytesUsed() { return budget.used; }
std::size_t PeakHeapBytes() { return budget.peak; }
} // namespace ScriptBudget
