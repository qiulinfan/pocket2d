#include "shared/persistence/SaveStore.h"
#include <SDL3/SDL.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
namespace {
void Check(bool condition, const std::string &message) { if (!condition) throw std::runtime_error(message); }
std::string Bytes(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
void Put(const fs::path &path, const std::string &bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    Check(static_cast<bool>(stream), "fixture write failed");
}
rapidjson::Document Parse(const std::string &json) {
    rapidjson::Document document;
    document.Parse(json.data(), json.size());
    Check(!document.HasParseError(), "invalid test JSON");
    return document;
}
std::string Envelope(const std::string &payload) { return "{\"format_version\":1,\"schema_version\":1,\"data\":" + payload + "}"; }
void Code(const SaveStore::Status &status, const std::string &code) {
    Check(!status.ok && status.code == code && !status.message.empty(), "expected " + code + ", got " + status.code + ": " + status.message);
}
struct TemporaryDirectory {
    fs::path path = fs::temp_directory_path() / ("pocket-save-store-" + std::to_string(SDL_GetPerformanceCounter()));
    TemporaryDirectory() { Check(fs::create_directory(path), "temporary directory collision"); }
    ~TemporaryDirectory() { std::error_code ignored; fs::remove_all(path, ignored); }
};
void Run() {
    TemporaryDirectory temporary;
    const auto directory = temporary.path / "game";
    const auto slot = directory / "slot.save.json";
    auto data = Parse(R"({"nested":[true,false,{"name":"庄园","zero":0,"min":-9223372036854775808,"max":9223372036854775807}],"empty":{},"array":[],"float":1.25,"nul\u0000key":"a\u0000b"})");
    Check(SaveStore::Write(directory, "slot", 42, data).ok, "nested write failed");
    const auto original = Bytes(slot);
    auto loaded = SaveStore::Read(directory, "slot");
    Check(loaded.status.ok && loaded.schema_version == 42 && loaded.data == data, "exact nested/signed64/UTF8/NUL roundtrip failed");
    Check(loaded.data["nested"][2]["min"].GetInt64() == std::numeric_limits<std::int64_t>::min(), "int64 minimum lost");
    Check(SaveStore::Write(directory, "slot", 43, data).ok && SaveStore::Read(directory, "slot").schema_version == 43, "existing slot replacement failed");
    // The replaced version stays as the backup, which a damaged or missing slot falls back to.
    const auto backup = directory / "slot.save.json.bak";
    Check(Bytes(backup) == original, "the replaced version is kept as .bak");
    const auto newest = Bytes(slot);
    Put(slot, newest.substr(0, newest.size() / 2));
    Code(SaveStore::Read(directory, "slot").status, "invalid_data");
    auto recovered = SaveStore::Read(directory, "slot", true);
    Check(recovered.status.ok && recovered.recovered && recovered.schema_version == 42 && recovered.data == data,
          "a truncated slot falls back to its backup");
    fs::remove(slot);
    Check(SaveStore::Read(directory, "slot", true).recovered, "a missing slot falls back to its backup");
    Put(backup, "{");
    Code(SaveStore::Read(directory, "slot", true).status, "not_found");
    fs::remove(backup);
    Put(slot, newest);
    Check(!SaveStore::Read(directory, "slot", true).recovered, "a sound slot is read as it is");
    fs::remove(backup);
    const auto unicode_directory = temporary.path / fs::path(std::u8string(u8"存档"));
    Check(SaveStore::Write(unicode_directory, "slot", 42, data).ok && SaveStore::Read(unicode_directory, "slot").data == data,
          "UTF-8 directory roundtrip failed");
    Put(slot, original);
    for (const std::string &invalid : std::vector<std::string>{"", "../slot", "a/b", "a\\b", ".hidden", "_leading", "a.b", "é", std::string(65, 'a'), std::string("a\0b", 3)}) {
        Check(!SaveStore::IsValidIdentifier(invalid), "invalid identifier accepted");
        Code(SaveStore::Write(directory, invalid, 1, data), "invalid_argument");
        Code(SaveStore::Read(directory, invalid).status, "invalid_argument");
    }
    for (const std::string &valid : std::vector<std::string>{"a", "Slot-1_2", std::string(64, 'z')}) Check(SaveStore::IsValidIdentifier(valid), "valid identifier rejected");
    Code(SaveStore::Write(directory, "slot", 0, data), "invalid_argument");
    Code(SaveStore::Write({}, "slot", 1, data), "invalid_argument");
    Code(SaveStore::Read(directory, "absent").status, "not_found");
    Check(Bytes(slot) == original, "argument errors changed old bytes");

    const auto rejected_write = [&](const rapidjson::Value &bad) {
        Code(SaveStore::Write(directory, "slot", 1, bad), "invalid_data");
        Check(Bytes(slot) == original, "validation failure changed old bytes");
    };
    for (const auto *json : {"null", "true", "123", "\"root\"", "[null]", "{\"x\":1,\"x\":2}", "[9223372036854775808]"}) rejected_write(Parse(json));
    auto number = Parse("[0]"); number[0].SetDouble(std::numeric_limits<double>::infinity()); rejected_write(number);
    number[0].SetDouble(std::numeric_limits<double>::quiet_NaN()); rejected_write(number);
    auto bad_utf8 = Parse("[\"\"]"); bad_utf8[0].SetString("\xc0\xaf", 2, bad_utf8.GetAllocator()); rejected_write(bad_utf8);
    auto bad_key = Parse("{}"); bad_key.AddMember(rapidjson::Value("\xff", 1, bad_key.GetAllocator()), rapidjson::Value(true), bad_key.GetAllocator()); rejected_write(bad_key);
    auto depth32 = Parse(std::string(31, '[') + "true" + std::string(31, ']'));
    Check(SaveStore::Write(directory, "depth", 1, depth32).ok && SaveStore::Read(directory, "depth").status.ok, "depth 32 boundary rejected");
    rejected_write(Parse(std::string(32, '[') + "true" + std::string(32, ']')));
    rapidjson::Document nodes(rapidjson::kArrayType);
    for (std::size_t i = 1; i < SaveStore::kMaxNodes; ++i) nodes.PushBack(true, nodes.GetAllocator());
    const auto nodes_write = SaveStore::Write(directory, "nodes", 1, nodes);
    Check(nodes_write.ok, "100000 node write rejected: " + nodes_write.message);
    const auto nodes_read = SaveStore::Read(directory, "nodes");
    Check(nodes_read.status.ok, "100000 node read rejected: " + nodes_read.status.message);
    nodes.PushBack(true, nodes.GetAllocator()); rejected_write(nodes);

    auto large = Parse("[\"\"]");
    const auto overhead = Envelope("[\"\"]").size();
    std::string text(SaveStore::kMaxFileBytes - overhead, 'x');
    large[0].SetString(text.data(), static_cast<rapidjson::SizeType>(text.size()), large.GetAllocator());
    Check(SaveStore::Write(directory, "size", 1, large).ok && fs::file_size(directory / "size.save.json") == SaveStore::kMaxFileBytes,
          "exact 4MiB encoded boundary rejected");
    Check(SaveStore::Read(directory, "size").status.ok, "exact 4MiB file failed read");
    text.push_back('x'); large[0].SetString(text.data(), static_cast<rapidjson::SizeType>(text.size()), large.GetAllocator()); rejected_write(large);
    // Escapes count toward the encoded limit, even when raw input fits.
    text.assign(SaveStore::kMaxFileBytes / 2, '\n');
    large[0].SetString(text.data(), static_cast<rapidjson::SizeType>(text.size()), large.GetAllocator()); rejected_write(large);

    const auto rejected_read = [&](const std::string &bytes, const char *code = "invalid_data") {
        Put(directory / "bad.save.json", bytes);
        auto result = SaveStore::Read(directory, "bad"); Code(result.status, code);
        Check(result.schema_version == 0 && result.data.IsNull(), "failed read exposed partial data");
        Check(Bytes(directory / "bad.save.json") == bytes && Bytes(slot) == original, "read mutated file bytes");
    };
    for (const auto *json : {"", "{", "[]", "{}", "{\"format_version\":1,\"schema_version\":1,\"data\":{},\"extra\":0}",
                             "{\"format_version\":1,\"format_version\":1,\"data\":{}}", "{\"format_version\":1,\"schema_version\":0,\"data\":{}}"}) rejected_read(json);
    for (const auto *payload : {"[null]", "{\"a\":1,\"a\":2}", "[9223372036854775808]", "[-9223372036854775809]", "[18446744073709551616]", "[1e999]"}) rejected_read(Envelope(payload));
    rejected_read(Envelope("[\"\xff\"]"));
    rejected_read(Envelope("{}") + std::string("\0ignored", 8));
    rejected_read(Envelope(std::string(2000, '[') + "0" + std::string(2000, ']')));
    rejected_read(std::string(SaveStore::kMaxFileBytes + 1, ' '));
    rejected_read("{\"format_version\":2,\"schema_version\":1,\"data\":{}}", "unsupported_format");
    Check(SaveStore::Write(directory, "future_schema", std::numeric_limits<int>::max(), data).ok &&
          SaveStore::Read(directory, "future_schema").schema_version == std::numeric_limits<int>::max(), "game schema must remain game-owned");

    fs::create_directory(directory / "directory.save.json");
    Code(SaveStore::Write(directory, "directory", 1, data), "io_error");
    Put(temporary.path / "not_directory", "keep");
    Code(SaveStore::Write(temporary.path / "not_directory" / "nested", "slot", 1, data), "io_error");
    Check(Bytes(temporary.path / "not_directory") == "keep", "invalid directory modified unrelated file");
    std::error_code symlink_error;
    fs::create_symlink(slot, directory / "linked.save.json", symlink_error);
    if (!symlink_error) {
        Code(SaveStore::Read(directory, "linked").status, "io_error");
        Code(SaveStore::Write(directory, "linked", 1, data), "io_error");
        fs::create_directory_symlink(directory, temporary.path / "linked_dir");
        Code(SaveStore::Write(temporary.path / "linked_dir", "slot", 1, data), "io_error");
        fs::create_symlink(temporary.path / "missing", directory / "dangling.save.json");
        Code(SaveStore::Write(directory, "dangling", 1, data), "io_error");
    } else {
#ifdef _WIN32
        std::cout << "symlink checks skipped: " << symlink_error.message() << '\n';
#else
        Check(false, "could not create symlink fixture: " + symlink_error.message());
#endif
    }
#ifndef _WIN32
    // Deterministic kernel write failure, not a timing race or mock writer.
    const auto child = fork();
    Check(child >= 0, "fork failed");
    if (child == 0) {
        std::signal(SIGXFSZ, SIG_IGN);
        rlimit limit{0, 0};
        if (setrlimit(RLIMIT_FSIZE, &limit) != 0) _exit(2);
        const auto failure = SaveStore::Write(directory, "slot", 1, data);
        _exit(!failure.ok && failure.code == "io_error" && Bytes(slot) == original ? 0 : 3);
    }
    int child_status = 0;
    Check(waitpid(child, &child_status, 0) == child && WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
          "kernel write failure did not preserve old save");
#endif
#ifdef __APPLE__
    // Owner-set immutable flag blocks replacement, while the directory still
    // permits creating and removing our temporary file. No race-based hook.
    if (chflags(slot.c_str(), UF_IMMUTABLE) == 0) {
        const auto rename_failure = SaveStore::Write(directory, "slot", 1, data);
        const int unlocked = chflags(slot.c_str(), 0);
        Check(unlocked == 0, "could not restore test file flags");
        Code(rename_failure, "io_error");
        Check(rename_failure.message.find("replace") != std::string::npos,
              "immutable target did not exercise the rename path: " + rename_failure.message);
        Check(Bytes(slot) == original, "failed atomic replacement changed old bytes");
    } else {
        std::cout << "immutable-target rename failure check skipped: filesystem does not permit flags\n";
    }
#endif
    Check(Bytes(slot) == original, "error-path tests changed previous save");
    for (const auto &entry : fs::directory_iterator(directory))
        Check(entry.path().filename().string().find(".tmp-") == std::string::npos, "temporary file leaked after failure");
}
} // namespace
int main() {
    try { Run(); std::cout << "SaveStore: strict format, bounds, atomic replacement and error preservation passed\n"; return 0; }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
