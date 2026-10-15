#ifndef POCKET2D_SCRIPT_RELOAD_CANDIDATE_H
#define POCKET2D_SCRIPT_RELOAD_CANDIDATE_H

#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ScriptReloadCandidate {
struct Source {
    std::string type;
    std::string resource;
    std::string code;
};
struct Diagnostic {
    std::string type;
    std::string resource;
    std::string stage;
    std::string message;
    int line = 0;
};
struct Batch {
    bool ok = false;
    std::vector<Diagnostic> diagnostics;
    std::unordered_map<std::string, luabridge::LuaRef> exports;
};

// UTF-8 diagnostic text capped at 4096 bytes, including any truncation marker.
std::string SanitizeDiagnosticText(std::string_view text);

// Stages in a separate bounded Lua VM. No live global/registry commit occurs.
// Main-VM allocation failure remains the runtime supervisor's reset boundary.
Batch Stage(lua_State *live, const std::vector<Source> &sources);
} // namespace ScriptReloadCandidate

#endif
