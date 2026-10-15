#ifndef POCKET2D_AGENT_SUPERVISOR_H
#define POCKET2D_AGENT_SUPERVISOR_H

#include "scripting/ScriptBudget.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>

struct AgentExecutionLimits {
    ScriptBudget::Limits script_limits{50'000'000, 64 * 1024 * 1024};
    std::uint32_t request_timeout_ms = 5000;
    std::size_t max_response_bytes = 16 * 1024 * 1024;
};

// Runs only the protocol host. Gameplay executes in a disposable same-binary
// --agent-worker child; SDL process APIs provide nonblocking pipes and reaping.
// A killed/failed worker requires an explicit reset before world access.
int RunAgentSupervisor(const std::filesystem::path &executable,
                       const std::filesystem::path &project, std::uint32_t seed,
                       const AgentExecutionLimits &limits = {});

#endif
