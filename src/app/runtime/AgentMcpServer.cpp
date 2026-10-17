#include "app/runtime/AgentMcpServer.h"
#include "app/runtime/AgentProtocolSchema.h"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include <array>
#include <iostream>
#include <string>
#include <string_view>

#ifndef POCKET2D_VERSION
#define POCKET2D_VERSION "unknown"
#endif

namespace {
using Document = rapidjson::Document;
using Value = rapidjson::Value;
using Allocator = Document::AllocatorType;

// Modern revisions carry the version on every request; legacy ones negotiate
// it once through initialize. This server speaks both.
constexpr std::string_view kModernVersion = "2026-07-28";
constexpr std::array<std::string_view, 4> kLegacyVersions{"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
constexpr const char *kVersionKey = "io.modelcontextprotocol/protocolVersion";
constexpr std::size_t kMessageLimit = 1024 * 1024;
constexpr const char *kInstructions =
    "Pocket2D gameplay runtime for one project. observe reads the world without advancing time; step and "
    "run_until advance fixed 1/60 s ticks with held keys and mouse; reset restarts the runtime with a seed; "
    "call runs a command the game declares in Lua (OnAgentCommand). Actor uids are decimal strings. A response with requires_reset needs reset before further simulation.";

std::string Serialize(const Value &value) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    value.Accept(writer);
    return buffer.GetString();
}

Value Text(std::string_view text, Allocator &allocator) {
    return Value(text.data(), static_cast<rapidjson::SizeType>(text.size()), allocator);
}

struct Server {
    AgentHost &host;
    const std::set<std::string> &access;
    Document schema;

    void Send(Document &message) { std::cout << Serialize(message) << std::endl; }

    void SendError(const Value *id, int code, const std::string &message, Value *data = nullptr) {
        Document reply(rapidjson::kObjectType);
        auto &allocator = reply.GetAllocator();
        reply.AddMember("jsonrpc", "2.0", allocator);
        Value copy;
        if (id) copy.CopyFrom(*id, allocator);
        reply.AddMember("id", copy, allocator);
        Value error(rapidjson::kObjectType);
        error.AddMember("code", code, allocator);
        error.AddMember("message", Text(message, allocator), allocator);
        if (data) error.AddMember("data", Value(*data, allocator), allocator);
        reply.AddMember("error", error, allocator);
        Send(reply);
    }

    void SendResult(const Value &id, Value &result, Document &owner, bool modern) {
        auto &allocator = owner.GetAllocator();
        if (modern && !result.HasMember("resultType")) result.AddMember("resultType", "complete", allocator);
        Document reply(rapidjson::kObjectType);
        reply.AddMember("jsonrpc", "2.0", reply.GetAllocator());
        reply.AddMember("id", Value(id, reply.GetAllocator()), reply.GetAllocator());
        reply.AddMember("result", Value(result, reply.GetAllocator()), reply.GetAllocator());
        Send(reply);
    }

    const Value *FindCommand(const std::string &name) const {
        for (const auto &command : schema["commands"].GetArray())
            if (name == command["name"].GetString() && name != "quit") return &command;
        return nullptr;
    }

    bool Enabled(const Value &command) const { return access.count(command["access"].GetString()) != 0; }

    Value Tools(Allocator &allocator) const {
        Value tools(rapidjson::kArrayType);
        for (const auto &command : schema["commands"].GetArray()) {
            const std::string name = command["name"].GetString();
            if (name == "quit" || !Enabled(command)) continue;
            const std::string level = command["access"].GetString();
            Value tool(rapidjson::kObjectType);
            tool.AddMember("name", Text(name, allocator), allocator);
            tool.AddMember("title", Text("Pocket2D " + name, allocator), allocator);
            tool.AddMember("description", Text(std::string(command["summary"].GetString()) +
                                                   " (access: " + level + ")", allocator), allocator);
            tool.AddMember("inputSchema", Value(command["params"], allocator), allocator);
            Value annotations(rapidjson::kObjectType);
            annotations.AddMember("readOnlyHint", level == "read", allocator);
            annotations.AddMember("destructiveHint", false, allocator);
            annotations.AddMember("idempotentHint", level == "read", allocator);
            annotations.AddMember("openWorldHint", false, allocator);
            tool.AddMember("annotations", annotations, allocator);
            tools.PushBack(tool, allocator);
        }
        return tools;
    }

    Value Capabilities(Allocator &allocator) const {
        Value capabilities(rapidjson::kObjectType), tools(rapidjson::kObjectType);
        tools.AddMember("listChanged", false, allocator);
        capabilities.AddMember("tools", tools, allocator);
        return capabilities;
    }

    Value ServerInfo(Allocator &allocator) const {
        Value info(rapidjson::kObjectType);
        info.AddMember("name", "pocket2d", allocator);
        info.AddMember("version", POCKET2D_VERSION, allocator);
        return info;
    }

    // Returns false when the session should end.
    bool Handle(const std::string &line) {
        Document message;
        message.Parse<rapidjson::kParseIterativeFlag | rapidjson::kParseValidateEncodingFlag>(line.data(), line.size());
        if (message.HasParseError()) { SendError(nullptr, -32700, "Parse error"); return true; }
        if (!message.IsObject() || !message.HasMember("jsonrpc") || !message["jsonrpc"].IsString() ||
            std::string_view(message["jsonrpc"].GetString()) != "2.0" ||
            !message.HasMember("method") || !message["method"].IsString()) {
            const Value *id = message.IsObject() && message.HasMember("id") ? &message["id"] : nullptr;
            // Responses from the client (no method) need no reply.
            if (message.IsObject() && !message.HasMember("method") && (message.HasMember("result") || message.HasMember("error")))
                return true;
            SendError(id, -32600, "Invalid Request");
            return true;
        }
        const bool notification = !message.HasMember("id");
        if (notification) return true; // initialized, cancelled and other notifications need no reply
        const Value &id = message["id"];
        if (!id.IsString() && !id.IsInt64() && !id.IsUint64()) { SendError(nullptr, -32600, "id must be a string or integer"); return true; }
        const std::string method = message["method"].GetString();
        Value empty(rapidjson::kObjectType);
        const Value &params = message.HasMember("params") && message["params"].IsObject() ? message["params"] : empty;
        bool modern = false;
        if (params.HasMember("_meta") && params["_meta"].IsObject() && params["_meta"].HasMember(kVersionKey)) {
            const auto &version = params["_meta"][kVersionKey];
            if (!version.IsString() || std::string_view(version.GetString()) != kModernVersion) {
                Document data(rapidjson::kObjectType);
                Value supported(rapidjson::kArrayType);
                supported.PushBack(Text(kModernVersion, data.GetAllocator()), data.GetAllocator());
                data.AddMember("supported", supported, data.GetAllocator());
                data.AddMember("requested", Value(version, data.GetAllocator()), data.GetAllocator());
                SendError(&id, -32022, "Unsupported protocol version", &data);
                return true;
            }
            modern = true;
        }

        Document result(rapidjson::kObjectType);
        auto &allocator = result.GetAllocator();
        if (method == "initialize") {
            std::string_view chosen = kLegacyVersions.front();
            if (params.HasMember("protocolVersion") && params["protocolVersion"].IsString())
                for (const auto version : kLegacyVersions)
                    if (version == params["protocolVersion"].GetString()) chosen = version;
            result.AddMember("protocolVersion", Text(chosen, allocator), allocator);
            result.AddMember("capabilities", Capabilities(allocator), allocator);
            result.AddMember("serverInfo", ServerInfo(allocator), allocator);
            result.AddMember("instructions", rapidjson::StringRef(kInstructions), allocator);
        } else if (method == "server/discover") {
            Value versions(rapidjson::kArrayType);
            versions.PushBack(Text(kModernVersion, allocator), allocator);
            result.AddMember("supportedVersions", versions, allocator);
            result.AddMember("capabilities", Capabilities(allocator), allocator);
            Value meta(rapidjson::kObjectType);
            meta.AddMember("io.modelcontextprotocol/serverInfo", ServerInfo(allocator), allocator);
            result.AddMember("_meta", meta, allocator);
            result.AddMember("instructions", rapidjson::StringRef(kInstructions), allocator);
        } else if (method == "ping") {
            // Empty result.
        } else if (method == "tools/list") {
            result.AddMember("tools", Tools(allocator), allocator);
        } else if (method == "tools/call") {
            if (!params.HasMember("name") || !params["name"].IsString()) {
                SendError(&id, -32602, "tools/call requires a tool name");
                return true;
            }
            const std::string name = params["name"].GetString();
            const Value *command = FindCommand(name);
            if (command == nullptr || !Enabled(*command)) {
                SendError(&id, -32602, command == nullptr ? "Unknown tool: " + name : "Tool not enabled: " + name);
                return true;
            }
            Document request(rapidjson::kObjectType);
            request.AddMember("cmd", Text(name, request.GetAllocator()), request.GetAllocator());
            if (params.HasMember("arguments")) {
                if (!params["arguments"].IsObject()) {
                    SendError(&id, -32602, "tools/call arguments must be an object");
                    return true;
                }
                for (const auto &member : params["arguments"].GetObject()) {
                    const std::string_view key(member.name.GetString(), member.name.GetStringLength());
                    if (key == "cmd" || key == "id" || key.starts_with('_')) {
                        SendError(&id, -32602, "argument name is reserved: " + std::string(key));
                        return true;
                    }
                    request.AddMember(Value(member.name, request.GetAllocator()),
                                      Value(member.value, request.GetAllocator()), request.GetAllocator());
                }
            }
            const auto reply = host.Handle(Serialize(request));
            Document response;
            response.Parse(reply.response.data(), reply.response.size());
            Value content(rapidjson::kArrayType), text(rapidjson::kObjectType);
            text.AddMember("type", "text", allocator);
            text.AddMember("text", Text(reply.response, allocator), allocator);
            content.PushBack(text, allocator);
            result.AddMember("content", content, allocator);
            const bool ok = response.IsObject() && response.HasMember("ok") && response["ok"].IsBool() && response["ok"].GetBool();
            if (response.IsObject()) result.AddMember("structuredContent", Value(response, allocator), allocator);
            result.AddMember("isError", !ok, allocator);
            SendResult(id, result, result, modern);
            return !reply.finished;
        } else {
            SendError(&id, -32601, "Method not found: " + method);
            return true;
        }
        SendResult(id, result, result, modern);
        return true;
    }
};
} // namespace

int RunAgentMcpServer(const std::filesystem::path &executable, const std::filesystem::path &project,
                      std::uint32_t seed, const AgentExecutionLimits &limits,
                      const std::set<std::string> &access) {
    AgentHost host(executable, project, seed, limits);
    std::string failure;
    // A failed start is reported to the client through tool results; reset
    // can start the runtime again after the project is fixed.
    if (!host.Start(failure)) std::cerr << "pocket2d mcp: runtime failed to start: " << failure << std::endl;
    Server server{host, access, {}};
    server.schema.Parse(PocketAgentProtocol::kSchemaJson.data(), PocketAgentProtocol::kSchemaJson.size());
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.size() > kMessageLimit) { server.SendError(nullptr, -32600, "message exceeds 1 MiB"); continue; }
        if (!server.Handle(line)) return 0;
    }
    host.Handle(R"({"cmd":"quit"})");
    return 0;
}
