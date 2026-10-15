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
};

bool IsValidIdentifier(const std::string &value);
Status Write(const std::filesystem::path &directory, const std::string &slot,
             int schema_version, const rapidjson::Value &data);
ReadResult Read(const std::filesystem::path &directory, const std::string &slot);
} // namespace SaveStore

#endif
