#ifndef POCKET2D_AGENT_SUPERVISOR_H
#define POCKET2D_AGENT_SUPERVISOR_H

#include "scripting/ScriptBudget.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

struct AgentExecutionLimits {
    ScriptBudget::Limits script_limits{50'000'000, 64 * 1024 * 1024};
    std::uint32_t request_timeout_ms = 5000;
    std::size_t max_response_bytes = 16 * 1024 * 1024;
};

class AgentReplayRecorder;

// The public protocol host. Gameplay executes in a disposable same-binary
// --agent-worker child; SDL process APIs provide nonblocking pipes and reaping.
// A killed/failed worker requires an explicit reset before world access. The
// JSONL loop, replay verification and the MCP server all drive one host.
class AgentHost {
public:
    AgentHost(std::filesystem::path executable, std::filesystem::path project, std::uint32_t seed,
              AgentExecutionLimits limits);
    ~AgentHost();
    AgentHost(const AgentHost &) = delete;
    AgentHost &operator=(const AgentHost &) = delete;

    // Starts the worker; on failure returns false and the error response.
    bool Start(std::string &failure_response);
    const std::string &InitialObservation() const;
    std::uint32_t Seed() const;

    struct Reply {
        std::string response;
        bool finished = false; // the session ended (quit or unrecoverable host failure)
        int exit_code = 0;
    };
    // One public request line (without its newline) to one response line.
    Reply Handle(const std::string &line, bool oversized = false);
    // Stops the worker; false when it could not be reaped.
    bool Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Runs the JSONL protocol on stdin/stdout, optionally recording a replay.
int RunAgentSupervisor(const std::filesystem::path &executable,
                       const std::filesystem::path &project, std::uint32_t seed,
                       const AgentExecutionLimits &limits = {},
                       AgentReplayRecorder *recorder = nullptr);

#endif
