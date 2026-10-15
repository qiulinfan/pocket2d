#include "shared/persistence/SaveStore.h"
#include <SDL3/SDL.h>
#include <rapidjson/memorystream.h>
#include <rapidjson/reader.h>
#include <rapidjson/writer.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string_view>
#include <stdexcept>
#include <unordered_set>
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#endif

namespace SaveStore {
namespace {
constexpr std::size_t kMaxBytes = kMaxFileBytes;
struct BoundedBuffer {
    using Ch = char;
    std::string bytes;
    void Put(char value) {
        if (bytes.size() == kMaxBytes) throw std::length_error("save exceeds 4 MiB");
        bytes.push_back(value);
    }
    void Flush() {}
};
Status Fail(const char *code, std::string message) { return {false, code, std::move(message)}; }
Status Success() { return {true, {}, {}}; }
std::string UTF8Path(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}
bool ValidUTF8(const char *text, std::size_t length) {
    rapidjson::MemoryStream stream(text, length);
    while (stream.Tell() < length) {
        unsigned codepoint = 0;
        if (!rapidjson::UTF8<>::Decode(stream, &codepoint)) return false;
    }
    return true;
}
bool ValidateValue(const rapidjson::Value &value, unsigned depth, std::size_t &nodes, std::string &error) {
    if (depth > kMaxDepth || ++nodes > kMaxNodes) { error = "payload exceeds depth 32 or 100000 nodes"; return false; }
    if (value.IsNull()) { error = "null is not supported"; return false; }
    if (value.IsString()) {
        if (value.GetStringLength() > kMaxBytes || !ValidUTF8(value.GetString(), value.GetStringLength())) {
            error = "invalid UTF-8 or oversized string"; return false;
        }
    } else if (value.IsObject()) {
        std::unordered_set<std::string_view> names;
        for (auto member = value.MemberBegin(); member != value.MemberEnd(); ++member) {
            const auto &key = member->name;
            if (key.GetStringLength() > kMaxBytes || !ValidUTF8(key.GetString(), key.GetStringLength()) ||
                !names.emplace(key.GetString(), key.GetStringLength()).second) {
                error = "invalid UTF-8 or duplicate object key"; return false;
            }
            if (!ValidateValue(member->value, depth + 1, nodes, error)) return false;
        }
    } else if (value.IsArray()) {
        for (const auto &entry : value.GetArray()) if (!ValidateValue(entry, depth + 1, nodes, error)) return false;
    } else if (value.IsNumber()) {
        if (value.IsUint64() && !value.IsInt64()) { error = "integer exceeds signed 64-bit range"; return false; }
        if (value.IsDouble() && !std::isfinite(value.GetDouble())) { error = "number must be finite"; return false; }
    }
    return true;
}
Status ValidatePayload(const rapidjson::Value &value) {
    if (!value.IsObject() && !value.IsArray()) return Fail("invalid_data", "payload root must be object or array");
    std::size_t nodes = 0;
    std::string error;
    return ValidateValue(value, 1, nodes, error) ? Success() : Fail("invalid_data", error);
}
// SAX guard runs before DOM allocation, including lexical integer range checks:
// otherwise RapidJSON can silently represent out-of-range integer tokens as doubles.
struct ParseGuard : rapidjson::BaseReaderHandler<rapidjson::UTF8<>, ParseGuard> {
    unsigned depth = 0;
    std::size_t nodes = 0;
    bool Node() { return ++nodes <= kMaxNodes + 3 && depth + 1 <= kMaxDepth + 1; }
    bool Null() { return Node(); }
    bool Bool(bool) { return Node(); }
    bool String(const char *, rapidjson::SizeType, bool) { return Node(); }
    bool Key(const char *, rapidjson::SizeType, bool) { return true; }
    bool RawNumber(const char *text, rapidjson::SizeType size, bool) {
        if (!Node()) return false;
        const std::string_view number(text, size);
        if (number.find_first_of(".eE") != std::string_view::npos) return true;
        std::int64_t parsed = 0;
        const auto result = std::from_chars(text, text + size, parsed);
        return result.ec == std::errc() && result.ptr == text + size;
    }
    bool StartObject() { if (!Node()) return false; ++depth; return true; }
    bool StartArray() { return StartObject(); }
    bool EndObject(rapidjson::SizeType) { --depth; return true; }
    bool EndArray(rapidjson::SizeType) { --depth; return true; }
};
Status CheckPath(const std::filesystem::path &directory, const std::string &slot,
                 std::filesystem::path &target) {
    if (directory.empty() || !IsValidIdentifier(slot)) return Fail("invalid_argument", "save directory or slot is invalid");
    if (UTF8Path(directory).find('\0') != std::string::npos) return Fail("invalid_argument", "save directory contains NUL");
    std::error_code error;
    // Refuse a symlink at the configured directory and slot. Canonicalize trusted
    // ancestors (macOS temporary paths commonly pass through /var -> /private/var).
    auto current = std::filesystem::absolute(directory, error);
    if (error) return Fail("io_error", "cannot resolve save directory: " + error.message());
    const auto directory_status = std::filesystem::symlink_status(current, error);
    if (error && error != std::errc::no_such_file_or_directory) return Fail("io_error", "cannot inspect save path: " + error.message());
    error.clear();
    if (std::filesystem::is_symlink(directory_status)) return Fail("io_error", "symlink save directory is not supported");
    current = std::filesystem::weakly_canonical(current, error);
    if (error) return Fail("io_error", "cannot resolve save directory: " + error.message());
    target = current / (slot + ".save.json");
    const auto status = std::filesystem::symlink_status(target, error);
    if (error && error != std::errc::no_such_file_or_directory) return Fail("io_error", "cannot inspect save slot: " + error.message());
    if (std::filesystem::is_symlink(status)) return Fail("io_error", "symlink save slot is not supported");
    if (std::filesystem::exists(status) && !std::filesystem::is_regular_file(status)) return Fail("io_error", "save slot is not a regular file");
    return Success();
}
int CloseFD(int fd) {
#ifdef _WIN32
    return _close(fd);
#else
    return close(fd);
#endif
}
struct TemporaryFile {
    std::filesystem::path path;
    int fd = -1;
    ~TemporaryFile() {
        if (fd >= 0) CloseFD(fd);
        if (!path.empty()) { std::error_code ignored; std::filesystem::remove(path, ignored); }
    }
};
Status AtomicWrite(const std::filesystem::path &target, const char *bytes, std::size_t size) {
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    if (error) return Fail("io_error", "cannot create save directory: " + error.message());
    static std::atomic<std::uint64_t> serial{0};
    TemporaryFile temporary;
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        auto candidate = target;
        candidate += ".tmp-" + std::to_string(SDL_GetPerformanceCounter()) + "-" + std::to_string(++serial);
#ifdef _WIN32
        temporary.fd = _wopen(candidate.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
        temporary.fd = open(candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
#endif
        if (temporary.fd >= 0) { temporary.path = std::move(candidate); break; }
        if (errno != EEXIST) return Fail("io_error", "cannot create temporary save: " + std::string(std::strerror(errno)));
    }
    if (temporary.fd < 0) return Fail("io_error", "cannot create exclusive temporary save");
    std::size_t offset = 0;
    while (offset < size) {
#ifdef _WIN32
        const auto count = _write(temporary.fd, bytes + offset, static_cast<unsigned>(size - offset));
#else
        const auto count = write(temporary.fd, bytes + offset, size - offset);
#endif
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return Fail("io_error", "cannot write temporary save: " + std::string(std::strerror(errno)));
        offset += static_cast<std::size_t>(count);
    }
#ifdef _WIN32
    const int synced = _commit(temporary.fd);
#else
    const int synced = fsync(temporary.fd);
#endif
    if (synced != 0) return Fail("io_error", "cannot sync temporary save: " + std::string(std::strerror(errno)));
    const int closed = CloseFD(temporary.fd);
    temporary.fd = -1;
    if (closed != 0) return Fail("io_error", "cannot close temporary save: " + std::string(std::strerror(errno)));
    // Recheck the destination after preparation; this is not an adversarial-FS sandbox.
    std::filesystem::path checked;
    const auto checked_status = CheckPath(target.parent_path(), target.stem().stem().string(), checked);
    if (!checked_status.ok) return checked_status;
    if (!SDL_RenamePath(UTF8Path(temporary.path).c_str(), UTF8Path(target).c_str()))
        return Fail("io_error", "cannot replace save slot: " + std::string(SDL_GetError()));
    temporary.path.clear();
    return Success();
}
} // namespace

bool IsValidIdentifier(const std::string &value) {
    if (value.empty() || value.size() > 64) return false;
    const auto alnum = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    if (!alnum(value.front())) return false;
    for (const unsigned char c : value) if (!alnum(c) && c != '_' && c != '-') return false;
    return true;
}
Status Write(const std::filesystem::path &directory, const std::string &slot,
             int schema_version, const rapidjson::Value &data) {
    try {
        if (schema_version <= 0) return Fail("invalid_argument", "schema_version must be a positive int");
        std::filesystem::path target;
        auto status = CheckPath(directory, slot, target);
        if (!status.ok) return status;
        status = ValidatePayload(data);
        if (!status.ok) return status;
        BoundedBuffer buffer;
        rapidjson::Writer<BoundedBuffer> writer(buffer);
        writer.StartObject(); writer.Key("format_version"); writer.Int(1);
        writer.Key("schema_version"); writer.Int(schema_version); writer.Key("data");
        if (!data.Accept(writer) || !writer.EndObject()) return Fail("invalid_data", "cannot encode save payload");
        return AtomicWrite(target, buffer.bytes.data(), buffer.bytes.size());
    } catch (const std::length_error &error) { return Fail("invalid_data", error.what()); }
      catch (const std::exception &error) { return Fail("io_error", error.what()); }
}
ReadResult Read(const std::filesystem::path &directory, const std::string &slot) {
    ReadResult result;
    try {
        std::filesystem::path target;
        result.status = CheckPath(directory, slot, target);
        if (!result.status.ok) return result;
        std::error_code error;
        if (!std::filesystem::exists(target, error)) {
            result.status = Fail(error ? "io_error" : "not_found", error ? error.message() : "save slot does not exist"); return result;
        }
        std::ifstream stream(target, std::ios::binary);
        if (!stream) { result.status = Fail("io_error", "cannot open save slot"); return result; }
        std::string bytes;
        std::array<char, 8192> block;
        while (stream) {
            stream.read(block.data(), block.size());
            const auto count = static_cast<std::size_t>(stream.gcount());
            if (bytes.size() + count > kMaxBytes) { result.status = Fail("invalid_data", "save exceeds 4 MiB"); return result; }
            bytes.append(block.data(), count);
        }
        if (!stream.eof()) { result.status = Fail("io_error", "cannot read save slot"); return result; }
        rapidjson::MemoryStream input(bytes.data(), bytes.size());
        rapidjson::Reader reader;
        ParseGuard guard;
        if (!reader.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag | rapidjson::kParseNumbersAsStringsFlag>(input, guard) || input.Tell() != bytes.size()) {
            result.status = Fail("invalid_data", "malformed JSON, invalid UTF-8, integer range or structural limits"); return result;
        }
        rapidjson::Document envelope;
        envelope.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag>(bytes.data(), bytes.size());
        if (envelope.HasParseError() || !envelope.IsObject() || envelope.MemberCount() != 3 ||
            !envelope.HasMember("format_version") || !envelope.HasMember("schema_version") || !envelope.HasMember("data") ||
            !envelope["format_version"].IsInt() || !envelope["schema_version"].IsInt() || envelope["schema_version"].GetInt() <= 0) {
            result.status = Fail("invalid_data", "invalid save envelope"); return result;
        }
        if (envelope["format_version"].GetInt() != 1) { result.status = Fail("unsupported_format", "unsupported engine save format"); return result; }
        result.status = ValidatePayload(envelope["data"]);
        if (!result.status.ok) return result;
        result.schema_version = envelope["schema_version"].GetInt();
        result.data.CopyFrom(envelope["data"], result.data.GetAllocator());
    } catch (const std::exception &error) {
        result.schema_version = 0;
        result.data.SetNull();
        result.status = Fail("io_error", error.what());
    }
    return result;
}
} // namespace SaveStore
