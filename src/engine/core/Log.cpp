#include "core/Log.h"
#include "core/World.h"
#include "SDL3/SDL_stdinc.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

namespace {

struct LogState {
    std::deque<Log::Record> recent;
    std::uint64_t written = 0;
};

bool JsonLines() {
    static const bool enabled = [] {
        const char *format = SDL_getenv("POCKET_LOG_FORMAT");
        return format != nullptr && std::strcmp(format, "jsonl") == 0;
    }();
    return enabled;
}

void AppendJsonString(std::string &out, std::string_view text) {
    out += '"';
    for (const unsigned char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                out += escaped;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

// Several Worlds may log from different threads; keep lines whole.
std::mutex &OutputMutex() {
    static std::mutex mutex;
    return mutex;
}

} // namespace

namespace Log {

const char *LevelName(Level level) {
    switch (level) {
    case Level::Debug: return "debug";
    case Level::Info: return "info";
    case Level::Warning: return "warning";
    case Level::Error: return "error";
    }
    return "info";
}

void Write(Level level, std::string_view category, std::string_view message) {
    World &world = World::Current();
    Record record{level, std::string(category), std::string(message), world.frame_number, world.GetId()};

    std::string line;
    if (JsonLines()) {
        line = "{\"level\":";
        AppendJsonString(line, LevelName(level));
        line += ",\"category\":";
        AppendJsonString(line, category);
        line += ",\"world\":" + std::to_string(record.world) + ",\"frame\":" + std::to_string(record.frame) + ",\"message\":";
        AppendJsonString(line, message);
        line += "}\n";
    } else {
        line = std::string(LevelName(level)) + " [" + std::string(category) + "] " + std::string(message) + '\n';
    }
    {
        std::lock_guard lock(OutputMutex());
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fflush(stderr);
    }

    LogState &state = world.State<LogState>();
    state.recent.push_back(std::move(record));
    if (state.recent.size() > kRecentCapacity) state.recent.pop_front();
    ++state.written;
}

std::vector<Record> Recent() {
    const LogState &state = World::Current().State<LogState>();
    return {state.recent.begin(), state.recent.end()};
}

std::uint64_t WrittenCount() {
    return World::Current().State<LogState>().written;
}

} // namespace Log
