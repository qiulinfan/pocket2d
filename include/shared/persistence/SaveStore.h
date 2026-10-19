#ifndef POCKET2D_SAVE_STORE_H
#define POCKET2D_SAVE_STORE_H

#include <filesystem>
#include <cstddef>
#include <string>
#include <rapidjson/document.h>

namespace SaveStore {
inline constexpr std::size_t kMaxDepth = 32;
inline constexpr std::size_t kMaxNodes = 100000;
inline constexpr std::size_t kMaxFileBytes = 4 * 1024 * 1024;
struct Status {
    bool ok = false;
    std::string code;
    std::string message;
};
struct ReadResult {
    Status status;
    int schema_version = 0;
    rapidjson::Document data;
    bool recovered = false; // read from the slot's previous version (the slot itself was missing or damaged)
};

bool IsValidIdentifier(const std::string &value);
// A write goes to a temporary file that is flushed to the disk (F_FULLFSYNC on
// macOS) and renamed over the slot, and the directory is flushed too, so after
// a crash the slot holds the old or the new save, never a mix. The replaced
// version stays beside it as <slot>.save.json.bak; Read with `fallback` returns
// it (marked recovered) when the slot is missing or does not parse.
Status Write(const std::filesystem::path &directory, const std::string &slot,
             int schema_version, const rapidjson::Value &data);
ReadResult Read(const std::filesystem::path &directory, const std::string &slot, bool fallback = false);
} // namespace SaveStore

#endif
