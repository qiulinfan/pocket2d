#ifndef POCKET2D_SCRIPT_BUDGET_H
#define POCKET2D_SCRIPT_BUDGET_H

#include <cstddef>
#include <cstdint>

struct lua_State;

// One Lua VM per process, matching ComponentManager. All calls belong to the
// Lua owner thread. This limits Lua allocations/instructions, not native host
// allocations, OS access or blocking C functions; the agent supervisor supplies
// an independent process deadline.
namespace ScriptBudget {
struct Limits {
    std::uint64_t instruction_limit = 0; // 0 disables instruction accounting.
    std::size_t heap_limit_bytes = 0;    // 0 disables the Lua heap cap.
};

enum class Failure { None, InstructionLimit, HeapLimit };

// Configure before creating a VM. Interactive/editor execution defaults to
// unlimited; agent workers explicitly provide nonzero defaults.
void Configure(Limits limits);
Limits GetLimits();
lua_State *NewState();
void CloseState(lua_State *state);

// Only the outermost scope replenishes the request budget. Coroutines share
// allocation accounting and inherit the count hook from their creating thread.
// Budgeted mode charges every Lua bytecode instruction (hook interval 1), so
// short-lived coroutines cannot discard a partially elapsed hook interval.
void BeginRequest();
void EndRequest();
Failure GetFailure();
const char *FailureMessage();
std::size_t HeapBytesUsed();
std::size_t PeakHeapBytes();

class RequestScope {
public:
    RequestScope() { BeginRequest(); }
    ~RequestScope() { EndRequest(); }
    RequestScope(const RequestScope &) = delete;
    RequestScope &operator=(const RequestScope &) = delete;
};
} // namespace ScriptBudget

#endif
