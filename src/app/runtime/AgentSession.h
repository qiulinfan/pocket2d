#pragma once

#include <cstdint>
#include <filesystem>

// Trusted local gameplay automation. Game scripts retain their normal Lua
// permissions; this process is not an untrusted-code sandbox.
int RunAgentSession(const std::filesystem::path &project, std::uint32_t seed);
