#ifndef ENGINE_CORE_LOG_H
#define ENGINE_CORE_LOG_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Engine diagnostics. Each record carries the World, its frame and a category,
// goes to stderr, and is kept in a bounded per-World buffer that agents and the
// editor can read. POCKET_LOG_FORMAT=jsonl writes one JSON object per line
// instead of text. Lua's print and warning channels are separate.
namespace Log {

enum class Level { Debug, Info, Warning, Error };

struct Record {
    Level level = Level::Info;
    std::string category;
    std::string message;
    int frame = 0;
    std::uint64_t world = 0;
};

void Write(Level level, std::string_view category, std::string_view message);
inline void Info(std::string_view category, std::string_view message) { Write(Level::Info, category, message); }
inline void Warning(std::string_view category, std::string_view message) { Write(Level::Warning, category, message); }
inline void Error(std::string_view category, std::string_view message) { Write(Level::Error, category, message); }

const char *LevelName(Level level);

// Records of the current World, oldest first; at most kRecentCapacity.
constexpr std::size_t kRecentCapacity = 256;
std::vector<Record> Recent();
// Number of records written in the current World, including evicted ones.
std::uint64_t WrittenCount();

} // namespace Log

#endif
