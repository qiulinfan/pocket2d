#include "app/runtime/AgentReplay.h"
#include "app/runtime/AgentProtocolSchema.h"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>

#ifndef POCKET2D_VERSION
#define POCKET2D_VERSION "unknown"
#endif

namespace {
using Document = rapidjson::Document;
const std::set<std::string> kRecorded{"observe", "step", "run_until", "reset", "reload_scripts", "checkpoint", "restore"};

std::string Serialize(const rapidjson::Value &value) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    value.Accept(writer);
    return buffer.GetString();
}

Document Parse(const std::string &text) {
    Document document;
    document.Parse<rapidjson::kParseIterativeFlag>(text.data(), text.size());
    if (document.HasParseError() || !document.IsObject()) throw std::runtime_error("replay line is not a JSON object");
    return document;
}

// Diagnostics and correlation ids describe the session, not gameplay.
void StripSession(rapidjson::Value &response) {
    response.RemoveMember("id");
    response.RemoveMember("diagnostics");
    response.RemoveMember("diagnostics_dropped");
}

std::string Escape(const std::string &token) {
    std::string out;
    for (char ch : token) {
        if (ch == '~') out += "~0";
        else if (ch == '/') out += "~1";
        else out += ch;
    }
    return out;
}

// First difference as a JSON pointer, or nullopt when equal.
std::optional<std::string> FirstDifference(const rapidjson::Value &expected, const rapidjson::Value &actual,
                                           const std::string &path = {}) {
    if (expected.IsNumber() && actual.IsNumber()) {
        const bool same = expected.IsDouble() || actual.IsDouble()
            ? expected.GetDouble() == actual.GetDouble()
            : expected == actual;
        return same ? std::nullopt : std::optional(path);
    }
    if (expected.GetType() != actual.GetType()) return path;
    if (expected.IsObject()) {
        for (const auto &member : expected.GetObject()) {
            const std::string key(member.name.GetString(), member.name.GetStringLength());
            const auto found = actual.FindMember(member.name);
            if (found == actual.MemberEnd()) return path + "/" + Escape(key);
            if (auto difference = FirstDifference(member.value, found->value, path + "/" + Escape(key)))
                return difference;
        }
        for (const auto &member : actual.GetObject())
            if (!expected.HasMember(member.name))
                return path + "/" + Escape(std::string(member.name.GetString(), member.name.GetStringLength()));
        return std::nullopt;
    }
    if (expected.IsArray()) {
        const auto count = std::min(expected.Size(), actual.Size());
        for (rapidjson::SizeType i = 0; i < count; ++i)
            if (auto difference = FirstDifference(expected[i], actual[i], path + "/" + std::to_string(i)))
                return difference;
        if (expected.Size() != actual.Size()) return path + "/" + std::to_string(count);
        return std::nullopt;
    }
    return expected == actual ? std::nullopt : std::optional(path);
}

const rapidjson::Value *Resolve(const rapidjson::Value &root, const std::string &pointer) {
    const rapidjson::Value *current = &root;
    std::size_t position = 0;
    while (current != nullptr && position < pointer.size()) {
        const std::size_t next = pointer.find('/', position + 1);
        std::string token = pointer.substr(position + 1, next == std::string::npos ? std::string::npos : next - position - 1);
        for (std::size_t at; (at = token.find("~1")) != std::string::npos;) token.replace(at, 2, "/");
        for (std::size_t at; (at = token.find("~0")) != std::string::npos;) token.replace(at, 2, "~");
        if (current->IsObject()) {
            const auto found = current->FindMember(token.c_str());
            current = found == current->MemberEnd() ? nullptr : &found->value;
        } else if (current->IsArray()) {
            const auto index = static_cast<rapidjson::SizeType>(std::stoul(token));
            current = index < current->Size() ? &(*current)[index] : nullptr;
        } else current = nullptr;
        position = next == std::string::npos ? pointer.size() : next;
    }
    return current;
}

void Report(rapidjson::Writer<rapidjson::StringBuffer> &out, const char *key, const rapidjson::Value *value) {
    out.Key(key);
    if (value == nullptr) out.Null();
    else value->Accept(out);
}
} // namespace

std::string ProjectContentDigest(const std::filesystem::path &project) {
    std::vector<std::filesystem::path> files;
    for (auto it = std::filesystem::recursive_directory_iterator(project);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (it->path().filename().string().starts_with('.')) {
            if (it->is_directory()) it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file()) files.push_back(std::filesystem::relative(it->path(), project));
    }
    std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) {
        return a.generic_string() < b.generic_string();
    });
    std::uint64_t hash = 14695981039346656037ull;
    const auto mix = [&](const char *data, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i) { hash ^= static_cast<unsigned char>(data[i]); hash *= 1099511628211ull; }
    };
    for (const auto &relative : files) {
        const std::string name = relative.generic_string();
        mix(name.data(), name.size() + 1); // include the terminator as a separator
        std::ifstream file(project / relative, std::ios::binary);
        char chunk[8192];
        while (file.read(chunk, sizeof(chunk)) || file.gcount() > 0) mix(chunk, static_cast<std::size_t>(file.gcount()));
        mix("", 1);
    }
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return std::string("fnv1a64:") + text;
}

AgentReplayRecorder::AgentReplayRecorder(const std::filesystem::path &path, const std::filesystem::path &project)
    : file_(path, std::ios::binary | std::ios::trunc), project_(project) {
    if (!file_) throw std::runtime_error("cannot write replay file " + path.string());
}

void AgentReplayRecorder::Begin(std::uint32_t seed, const std::string &initial_observation) {
    Document header;
    header.SetObject();
    auto &allocator = header.GetAllocator();
    header.AddMember("pocket_replay", 1, allocator);
    header.AddMember("protocol", PocketAgentProtocol::kVersion, allocator);
    header.AddMember("engine", POCKET2D_VERSION, allocator);
    const std::string digest = ProjectContentDigest(project_);
    header.AddMember("project_digest", rapidjson::Value(digest.c_str(), allocator), allocator);
    header.AddMember("seed", seed, allocator);
    file_ << Serialize(header) << '\n';
    Document initial = Parse(initial_observation);
    StripSession(initial);
    Document entry;
    entry.SetObject();
    entry.AddMember("initial", rapidjson::Value(initial, entry.GetAllocator()), entry.GetAllocator());
    file_ << Serialize(entry) << '\n';
    file_.flush();
}

void AgentReplayRecorder::Record(const std::string &request_line, const std::string &response) {
    Document request;
    request.Parse(request_line.data(), request_line.size());
    if (request.HasParseError() || !request.IsObject() || !request.HasMember("cmd") || !request["cmd"].IsString() ||
        !kRecorded.count(request["cmd"].GetString()))
        return;
    request.RemoveMember("id");
    Document result = Parse(response);
    StripSession(result);
    Document entry;
    entry.SetObject();
    auto &allocator = entry.GetAllocator();
    entry.AddMember("request", rapidjson::Value(request, allocator), allocator);
    entry.AddMember("response", rapidjson::Value(result, allocator), allocator);
    file_ << Serialize(entry) << '\n';
    file_.flush();
}

int RunAgentReplay(const std::filesystem::path &executable, const std::filesystem::path &project,
                   const std::filesystem::path &replay, const AgentExecutionLimits &limits) {
    const auto fail = [](const std::string &code, const std::string &message) {
        Document out;
        out.SetObject();
        out.AddMember("ok", false, out.GetAllocator());
        out.AddMember("code", rapidjson::Value(code.c_str(), out.GetAllocator()), out.GetAllocator());
        out.AddMember("error", rapidjson::Value(message.c_str(), out.GetAllocator()), out.GetAllocator());
        std::cout << Serialize(out) << std::endl;
        return 1;
    };
    std::ifstream file(replay, std::ios::binary);
    if (!file) return fail("replay_unreadable", "cannot read replay file " + replay.string());
    std::vector<Document> lines;
    try {
        for (std::string line; std::getline(file, line);)
            if (!line.empty()) lines.push_back(Parse(line));
    } catch (const std::exception &error) {
        return fail("replay_invalid", error.what());
    }
    if (lines.size() < 2 || !lines[0].HasMember("pocket_replay") || !lines[0]["pocket_replay"].IsInt() ||
        lines[0]["pocket_replay"].GetInt() != 1 || !lines[0].HasMember("seed") || !lines[0]["seed"].IsUint() ||
        !lines[1].HasMember("initial"))
        return fail("replay_invalid", "replay must start with a version 1 header and the initial observation");
    if (!lines[0].HasMember("protocol") || !lines[0]["protocol"].IsInt() ||
        lines[0]["protocol"].GetInt() != PocketAgentProtocol::kVersion)
        return fail("replay_version", "replay was recorded with a different agent protocol version");
    const std::string digest = ProjectContentDigest(project);
    const bool project_changed = !lines[0].HasMember("project_digest") || !lines[0]["project_digest"].IsString() ||
                                 digest != lines[0]["project_digest"].GetString();

    AgentHost host(executable, project, lines[0]["seed"].GetUint(), limits);
    std::string failure;
    if (!host.Start(failure)) {
        std::cout << failure << std::endl;
        return 1;
    }
    std::size_t records = 0;
    int frame = 0;
    const auto compare = [&](std::size_t index, const char *command, const rapidjson::Value &expected,
                             const std::string &actual_text) -> bool {
        Document actual = Parse(actual_text);
        StripSession(actual);
        const auto difference = FirstDifference(expected, actual);
        if (actual.HasMember("frame") && actual["frame"].IsInt()) frame = actual["frame"].GetInt();
        if (!difference) return true;
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> out(buffer);
        out.StartObject();
        out.Key("ok"); out.Bool(false);
        out.Key("code"); out.String("replay_diverged");
        out.Key("error"); out.String("replay diverged from the recording");
        out.Key("record"); out.Uint64(index);
        out.Key("command"); out.String(command);
        out.Key("frame");
        if (expected.HasMember("frame") && expected["frame"].IsInt()) out.Int(expected["frame"].GetInt());
        else out.Null();
        out.Key("path"); out.String(difference->c_str());
        Report(out, "expected", Resolve(expected, *difference));
        Report(out, "actual", Resolve(actual, *difference));
        out.Key("project_changed"); out.Bool(project_changed);
        out.EndObject();
        std::cout << buffer.GetString() << std::endl;
        return false;
    };
    bool matched = compare(0, "initial", lines[1]["initial"], host.InitialObservation());
    for (std::size_t i = 2; matched && i < lines.size(); ++i) {
        if (!lines[i].HasMember("request") || !lines[i]["request"].IsObject() || !lines[i].HasMember("response")) {
            host.Handle(R"({"cmd":"quit"})");
            return fail("replay_invalid", "replay record " + std::to_string(i - 1) + " lacks request or response");
        }
        const auto &request = lines[i]["request"];
        const std::string command = request.HasMember("cmd") && request["cmd"].IsString() ? request["cmd"].GetString() : "";
        const auto reply = host.Handle(Serialize(request));
        matched = compare(i - 1, command.c_str(), lines[i]["response"], reply.response);
        ++records;
        if (reply.finished) break;
    }
    host.Handle(R"({"cmd":"quit"})");
    if (!matched) return 1;
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> out(buffer);
    out.StartObject();
    out.Key("ok"); out.Bool(true);
    out.Key("protocol"); out.Int(PocketAgentProtocol::kVersion);
    out.Key("records"); out.Uint64(records);
    out.Key("frame"); out.Int(frame);
    out.Key("project_changed"); out.Bool(project_changed);
    out.EndObject();
    std::cout << buffer.GetString() << std::endl;
    return 0;
}
