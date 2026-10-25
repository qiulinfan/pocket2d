#ifndef POCKET2D_AGENT_REPLAY_H
#define POCKET2D_AGENT_REPLAY_H

#include "app/runtime/AgentSupervisor.h"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

// A replay is JSON lines: a header with the protocol, engine version, project
// content digest and starting seed; the initial observation; then each
// simulating or observing request with its response. Diagnostics and client
// ids are not recorded because they are not gameplay state.
class AgentReplayRecorder {
public:
    AgentReplayRecorder(const std::filesystem::path &path, const std::filesystem::path &project);
    void Begin(std::uint32_t seed, const std::string &initial_observation);
    void Record(const std::string &request_line, const std::string &response);

private:
    std::ofstream file_;
    std::filesystem::path project_;
};

// FNV-1a 64 over every non-hidden file's relative path and bytes, sorted by
// path. Detects project changes between recording and replay; not a security
// hash.
std::string ProjectContentDigest(const std::filesystem::path &project);

// Replays a recording through a fresh host and compares every response.
// Prints one JSON summary line; returns 0 when every response matched.
int RunAgentReplay(const std::filesystem::path &executable, const std::filesystem::path &project,
                   const std::filesystem::path &replay, const AgentExecutionLimits &limits);

#endif
