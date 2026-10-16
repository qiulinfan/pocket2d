#ifndef POCKET2D_AGENT_MCP_SERVER_H
#define POCKET2D_AGENT_MCP_SERVER_H

#include "app/runtime/AgentSupervisor.h"
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>

// Model Context Protocol server over stdio: newline-delimited JSON-RPC 2.0.
// Each agent command except quit is a tool whose input schema comes from the
// command schema; calls go through the same supervised AgentHost as JSONL.
// Tools are offered only for the enabled access levels (read, simulate,
// modify). initialize is answered when a client sends it but not required.
int RunAgentMcpServer(const std::filesystem::path &executable, const std::filesystem::path &project,
                      std::uint32_t seed, const AgentExecutionLimits &limits,
                      const std::set<std::string> &access);

#endif
