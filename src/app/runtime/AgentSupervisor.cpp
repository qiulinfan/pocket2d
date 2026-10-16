#include "app/runtime/AgentSupervisor.h"
#include "app/runtime/AgentProtocolSchema.h"
#include "app/runtime/AgentReplay.h"
#include <SDL3/SDL.h>
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
constexpr std::size_t kCommandLimit = 65536;

struct WorkerFailure : std::runtime_error {
    std::string code;
    std::string response;
    WorkerFailure(std::string kind, std::string message, std::string worker_response = {})
        : std::runtime_error(std::move(message)), code(std::move(kind)),
          response(std::move(worker_response)) {}
};

std::string Serialize(const rapidjson::Document &document) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    document.Accept(writer);
    return buffer.GetString();
}

bool HasBoundedJSONDepth(const std::string &text) {
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (char ch : text) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        } else if (ch == '"') quoted = true;
        else if (ch == '{' || ch == '[') {
            if (++depth > 64) return false;
        } else if ((ch == '}' || ch == ']') && depth != 0) --depth;
    }
    return true;
}

void ValidateResponse(const rapidjson::Value &response) {
    if (!response.IsObject() || !response.HasMember("ok") || !response["ok"].IsBool())
        throw WorkerFailure("worker_protocol_error", "worker output was not a protocol JSON object");
    if (response["ok"].GetBool()) {
        if (!response.HasMember("protocol") || !response["protocol"].IsInt() ||
            response["protocol"].GetInt() != PocketAgentProtocol::kVersion)
            throw WorkerFailure("worker_protocol_error", "worker protocol version mismatch");
    } else if (!response.HasMember("error") || !response["error"].IsString()) {
        throw WorkerFailure("worker_protocol_error", "worker error response is missing its diagnostic");
    }
}

rapidjson::Document ParseResponse(const std::string &line) {
    if (!HasBoundedJSONDepth(line))
        throw WorkerFailure("worker_protocol_error", "worker JSON nesting exceeds protocol limit");
    rapidjson::Document response;
    response.Parse<rapidjson::kParseIterativeFlag | rapidjson::kParseValidateEncodingFlag>(line.data(), line.size());
    if (response.HasParseError())
        throw WorkerFailure("worker_protocol_error", "worker output was not valid JSON");
    ValidateResponse(response);
    return response;
}

void ValidateObservation(const rapidjson::Value &response) {
    ValidateResponse(response);
    if (response.HasMember("_pocket_request_id") || response.HasMember("_initial_observation"))
        throw WorkerFailure("worker_protocol_error", "private worker data appeared inside an observation");
    if (!response["ok"].GetBool() || !response.HasMember("frame") || !response["frame"].IsInt() ||
        !response.HasMember("actors") || !response["actors"].IsArray() ||
        !response.HasMember("running") || !response["running"].IsBool() ||
        !response.HasMember("scene") || !response["scene"].IsString())
        throw WorkerFailure("worker_protocol_error", "worker observation has an invalid shape");
}

void ValidateShape(const std::string &command, const rapidjson::Value &response) {
    if (!response["ok"].GetBool()) return;
    if (command == "describe" || command == "_bootstrap") {
        if (!response.HasMember("commands") || !response["commands"].IsArray())
            throw WorkerFailure("worker_protocol_error", "worker describe response has an invalid shape");
        if (command == "_bootstrap") {
            if (!response.HasMember("_initial_observation"))
                throw WorkerFailure("worker_protocol_error", "worker bootstrap omitted its observation");
            ValidateObservation(response["_initial_observation"]);
        }
    } else if (command == "observe" || command == "step" || command == "reset") {
        ValidateObservation(response);
    } else if (command == "reload_scripts") {
        ValidateObservation(response);
        if (!response.HasMember("reload") || !response["reload"].IsObject())
            throw WorkerFailure("worker_protocol_error", "worker reload response omitted its result");
        const auto &reload = response["reload"];
        if (!reload.HasMember("types") || !reload["types"].IsArray() || reload["types"].Empty() ||
            reload["types"].Size() > 64 || !reload.HasMember("instances_updated") ||
            !reload["instances_updated"].IsInt() || reload["instances_updated"].GetInt() < 0 ||
            !reload.HasMember("diagnostics") || !reload["diagnostics"].IsArray() || !reload["diagnostics"].Empty())
            throw WorkerFailure("worker_protocol_error", "worker reload result has an invalid shape");
        for (const auto &type : reload["types"].GetArray())
            if (!type.IsString() || type.GetStringLength() == 0)
                throw WorkerFailure("worker_protocol_error", "worker reload result has an invalid type name");
    } else if (command == "run_until") {
        ValidateObservation(response);
        if (!response.HasMember("run_until") || !response["run_until"].IsObject() ||
            !response["run_until"].HasMember("reason") || !response["run_until"]["reason"].IsString() ||
            !response["run_until"].HasMember("frames") || !response["run_until"]["frames"].IsInt())
            throw WorkerFailure("worker_protocol_error", "worker run_until result has an invalid shape");
    } else if (command == "quit") {
        if (!response.HasMember("frame") || !response["frame"].IsInt() ||
            !response.HasMember("running") || !response["running"].IsBool() ||
            response["running"].GetBool())
            throw WorkerFailure("worker_protocol_error", "worker quit response has an invalid shape");
    } else {
        throw WorkerFailure("worker_protocol_error", "worker unexpectedly accepted an unknown command");
    }
}

void SetBool(rapidjson::Document &document, const char *name, bool value) {
    if (document.HasMember(name)) document[name].SetBool(value);
    else document.AddMember(rapidjson::Value(name, document.GetAllocator()).Move(),
                            value, document.GetAllocator());
}

void AddExecutionLimits(rapidjson::Document &document, const AgentExecutionLimits &limits) {
    auto &allocator = document.GetAllocator();
    rapidjson::Value settings(rapidjson::kObjectType);
    const std::string instructions = std::to_string(limits.script_limits.instruction_limit);
    const std::string heap = std::to_string(limits.script_limits.heap_limit_bytes);
    settings.AddMember("instruction_limit", rapidjson::Value(instructions.c_str(), allocator), allocator);
    settings.AddMember("lua_heap_limit_bytes", rapidjson::Value(heap.c_str(), allocator), allocator);
    settings.AddMember("request_timeout_ms", limits.request_timeout_ms, allocator);
    settings.AddMember("max_response_bytes", static_cast<std::uint64_t>(limits.max_response_bytes), allocator);
    settings.AddMember("max_request_bytes", static_cast<std::uint64_t>(kCommandLimit), allocator);
    settings.AddMember("lua_heap_scope", "Lua allocator live requested bytes; excludes native resources and process RSS", allocator);
    settings.AddMember("filesystem_sandbox", false, allocator);
    settings.AddMember("instruction_scope", "One request including nested callbacks and coroutines; startup and bootstrap share a scope", allocator);
    document.RemoveMember("execution_limits");
    document.AddMember("execution_limits", settings.Move(), allocator);
}

std::string Error(const std::string &code, const std::string &message, int frame,
                  bool requires_reset, const std::string &worker_response = {}) {
    rapidjson::Document document;
    if (!worker_response.empty()) document.Parse(worker_response.data(), worker_response.size());
    if (!document.IsObject()) {
        document.SetObject();
        document.AddMember("ok", false, document.GetAllocator());
        document.AddMember("error", rapidjson::Value(message.c_str(), document.GetAllocator()),
                           document.GetAllocator());
        document.AddMember("frame", frame, document.GetAllocator());
        if (requires_reset)
            document.AddMember("frame_is_last_observed", true, document.GetAllocator());
    }
    if (!document.HasMember("code"))
        document.AddMember("code", rapidjson::Value(code.c_str(), document.GetAllocator()),
                           document.GetAllocator());
    SetBool(document, "requires_reset", requires_reset);
    return Serialize(document);
}

std::string QuitResponse(int frame) {
    rapidjson::Document document;
    document.SetObject();
    document.AddMember("ok", true, document.GetAllocator());
    document.AddMember("protocol", PocketAgentProtocol::kVersion, document.GetAllocator());
    document.AddMember("frame", frame, document.GetAllocator());
    document.AddMember("running", false, document.GetAllocator());
    return Serialize(document);
}

bool ReadCommand(std::istream &input, std::string &line, bool &oversized) {
    line.clear();
    oversized = false;
    bool read_any = false;
    char ch;
    while (input.get(ch)) {
        read_any = true;
        if (ch == '\n') return true;
        if (line.size() < kCommandLimit) line.push_back(ch);
        else oversized = true; // Drain the rest without growing the buffer.
    }
    return read_any;
}

class Worker {
public:
    explicit Worker(std::size_t response_limit) : response_limit_(response_limit) {
        std::random_device random;
        request_prefix_ = std::to_string(random()) + "-" + std::to_string(random()) + "-";
    }
    ~Worker() { Stop(); }
    Worker(const Worker &) = delete;
    Worker &operator=(const Worker &) = delete;
    bool Exists() const { return process_ != nullptr; }

    void Start(const std::filesystem::path &executable, const std::filesystem::path &project,
               std::uint32_t seed, const AgentExecutionLimits &limits) {
        if (!Stop()) throw WorkerFailure("worker_cleanup_failed", "previous worker could not be reaped");
        std::vector<std::string> arguments{
            executable.string(), "--headless", "--agent-worker", "--project", project.string(),
            "--seed", std::to_string(seed), "--instruction-budget",
            std::to_string(limits.script_limits.instruction_limit), "--lua-heap-budget",
            std::to_string(limits.script_limits.heap_limit_bytes)};
        std::vector<const char *> pointers;
        for (const auto &argument : arguments) pointers.push_back(argument.c_str());
        pointers.push_back(nullptr);
        SDL_PropertiesID properties = SDL_CreateProperties();
        if (properties == 0) throw WorkerFailure("worker_start_failed", SDL_GetError());
        const bool configured =
            SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, pointers.data()) &&
            SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_APP) &&
            SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP) &&
            SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_INHERITED);
        if (configured) process_ = SDL_CreateProcessWithProperties(properties);
        SDL_DestroyProperties(properties);
        if (process_ == nullptr) throw WorkerFailure("worker_start_failed", SDL_GetError());
        input_ = SDL_GetProcessInput(process_);
        output_ = SDL_GetProcessOutput(process_);
        if (input_ == nullptr || output_ == nullptr)
            throw WorkerFailure("worker_start_failed", "worker pipes are unavailable");
    }

    std::string Exchange(const std::string &command, Deadline deadline, bool startup = false) {
        if (!process_) throw WorkerFailure("worker_unavailable", "runtime unavailable; reset required");
        // Never silently carry an unsolicited response into the next request.
        Pump();
        if (!startup && !buffer_.empty())
            throw WorkerFailure("worker_protocol_error", "worker produced unsolicited stdout");
        rapidjson::Document request;
        request.Parse<rapidjson::kParseIterativeFlag | rapidjson::kParseValidateEncodingFlag>(command.data(), command.size());
        if (request.HasParseError() || !request.IsObject() || !request.HasMember("cmd") ||
            !request["cmd"].IsString())
            throw WorkerFailure("worker_protocol_error", "internal command was not valid JSON");
        const std::string command_name(request["cmd"].GetString(), request["cmd"].GetStringLength());
        const std::string request_id = request_prefix_ + std::to_string(++request_sequence_);
        const auto closing_brace = command.find_last_not_of(" \t\r\n");
        if (closing_brace == std::string::npos || command[closing_brace] != '}')
            throw WorkerFailure("worker_protocol_error", "internal command was not a JSON object");
        // Preserve the already validated payload (including numeric spelling).
        // The ID contains only generated decimal digits and hyphens. Public
        // callers cannot supply this reserved field; cmd makes the object nonempty.
        const std::string wire = command.substr(0, closing_brace) +
            ",\"_pocket_request_id\":\"" + request_id + "\"}" +
            command.substr(closing_brace + 1) + '\n';
        std::size_t sent = 0;
        for (;;) {
            if (Clock::now() >= deadline)
                throw WorkerFailure("deadline_exceeded", "agent request exceeded its process deadline");
            if (sent < wire.size()) {
                const std::size_t written = SDL_WriteIO(input_, wire.data() + sent, wire.size() - sent);
                sent += written;
                if (written == 0 && SDL_GetIOStatus(input_) != SDL_IO_STATUS_NOT_READY) {
                    Pump();
                    if (buffer_.find('\n') == std::string::npos)
                        throw WorkerFailure("worker_exit", "worker exited or closed its command pipe");
                }
            }
            Pump();
            const auto newline = buffer_.find('\n');
            if (newline != std::string::npos) {
                std::string response = buffer_.substr(0, newline);
                buffer_.erase(0, newline + 1);
                Pump();
                if (!buffer_.empty())
                    throw WorkerFailure("worker_protocol_error", "worker produced more than one response");
                auto document = ParseResponse(response);
                if (!document.HasMember("_pocket_request_id")) {
                    // Startup can fail before the worker reads its first command.
                    if (!(startup && !document["ok"].GetBool()))
                        throw WorkerFailure("worker_protocol_error", "worker response lacks request correlation");
                } else {
                    const auto &id = document["_pocket_request_id"];
                    if (!id.IsString() || std::string(id.GetString(), id.GetStringLength()) != request_id)
                        throw WorkerFailure("worker_protocol_error", "worker response correlation mismatch");
                    document.RemoveMember("_pocket_request_id");
                }
                ValidateShape(command_name, document);
                if (command_name != "_bootstrap" && document.HasMember("_initial_observation"))
                    throw WorkerFailure("worker_protocol_error", "worker emitted unexpected bootstrap data");
                if (Clock::now() >= deadline)
                    throw WorkerFailure("deadline_exceeded", "agent request exceeded its process deadline");
                return Serialize(document);
            }
            int exit_code = 0;
            if (SDL_WaitProcess(process_, false, &exit_code)) {
                Pump();
                if (buffer_.find('\n') != std::string::npos) continue;
                throw WorkerFailure("worker_exit", "worker exited before responding (exit " +
                                      std::to_string(exit_code) + ")");
            }
            SDL_Delay(1);
        }
    }

    bool WaitForExit(Deadline deadline) {
        while (process_ != nullptr) {
            Pump();
            if (!buffer_.empty())
                throw WorkerFailure("worker_protocol_error", "worker produced stdout during shutdown");
            if (SDL_WaitProcess(process_, false, nullptr)) return true;
            if (Clock::now() >= deadline) return false;
            SDL_Delay(1);
        }
        return true;
    }

    bool Stop() noexcept {
        if (process_ == nullptr) return true;
        bool exited = SDL_WaitProcess(process_, false, nullptr);
        if (!exited) SDL_KillProcess(process_, true);
        // Keep cleanup bounded as well. Native sleeps are interruptible by
        // forced process termination; do not wait on the child's Lua teardown.
        const Deadline reap_deadline = Clock::now() + std::chrono::seconds(1);
        while (!exited && Clock::now() < reap_deadline) {
            exited = SDL_WaitProcess(process_, false, nullptr);
            if (!exited) SDL_Delay(1);
        }
        SDL_DestroyProcess(process_);
        process_ = nullptr;
        input_ = output_ = nullptr;
        buffer_.clear();
        return exited;
    }

private:
    void Pump() {
        if (output_ == nullptr) return;
        char chunk[8192];
        // A noisy child cannot keep this loop running past the outer deadline.
        for (int iteration = 0; iteration < 8; ++iteration) {
            const std::size_t count = SDL_ReadIO(output_, chunk, sizeof(chunk));
            if (count == 0) {
                const SDL_IOStatus status = SDL_GetIOStatus(output_);
                if (status == SDL_IO_STATUS_ERROR)
                    throw WorkerFailure("worker_io_error", "failed to read worker output");
                return;
            }
            if (count > response_limit_ - std::min(response_limit_, buffer_.size()))
                throw WorkerFailure("worker_response_limit", "worker response exceeded its size limit");
            buffer_.append(chunk, count);
        }
    }

    SDL_Process *process_ = nullptr;
    SDL_IOStream *input_ = nullptr;
    SDL_IOStream *output_ = nullptr;
    std::string buffer_;
    std::size_t response_limit_;
    std::string request_prefix_;
    std::uint64_t request_sequence_ = 0;
};
} // namespace

struct AgentHost::Impl {
    std::filesystem::path executable, project;
    std::uint32_t seed;
    AgentExecutionLimits limits;
    Worker worker;
    int frame = 0;
    std::string description, initial_observation;

    Impl(std::filesystem::path executable_in, std::filesystem::path project_in, std::uint32_t seed_in,
         AgentExecutionLimits limits_in)
        : executable(std::move(executable_in)), project(std::move(project_in)), seed(seed_in),
          limits(limits_in), worker(limits_in.max_response_bytes) {}

    Deadline deadline() const { return Clock::now() + std::chrono::milliseconds(limits.request_timeout_ms); }

    std::string start(Deadline end) {
        worker.Start(executable, project, seed, limits);
        const std::string response = worker.Exchange(R"({"cmd":"_bootstrap"})", end, true);
        auto document = ParseResponse(response);
        if (!document["ok"].GetBool()) {
            if (document.HasMember("frame") && document["frame"].IsInt()) frame = document["frame"].GetInt();
            throw WorkerFailure("worker_start_failed", document["error"].GetString(), response);
        }
        // Startup code, discovery and the first observation share one worker
        // RequestScope; external reset needs no second observe command.
        rapidjson::Document initial;
        initial.CopyFrom(document["_initial_observation"], initial.GetAllocator());
        document.RemoveMember("_initial_observation");
        AddExecutionLimits(document, limits);
        description = Serialize(document);
        frame = initial["frame"].GetInt();
        return Serialize(initial);
    }
};

AgentHost::AgentHost(std::filesystem::path executable, std::filesystem::path project, std::uint32_t seed,
                     AgentExecutionLimits limits)
    : impl_(std::make_unique<Impl>(std::move(executable), std::move(project), seed, limits)) {}
AgentHost::~AgentHost() = default;

bool AgentHost::Start(std::string &failure_response) {
    try {
        impl_->initial_observation = impl_->start(impl_->deadline());
        return true;
    } catch (const WorkerFailure &error) {
        impl_->worker.Stop();
        failure_response = Error(error.code, error.what(), impl_->frame, true, error.response);
        return false;
    }
}

const std::string &AgentHost::InitialObservation() const { return impl_->initial_observation; }
std::uint32_t AgentHost::Seed() const { return impl_->seed; }
bool AgentHost::Stop() { return impl_->worker.Stop(); }

AgentHost::Reply AgentHost::Handle(const std::string &line, bool oversized) {
    Impl &host = *impl_;
    Worker &worker = host.worker;
    int &frame = host.frame;
    if (oversized) return {Error("invalid_request", "command exceeds 64 KiB", frame, false)};
    rapidjson::Document request;
    if (HasBoundedJSONDepth(line))
        request.Parse<rapidjson::kParseIterativeFlag | rapidjson::kParseValidateEncodingFlag>(line.data(), line.size());
    const bool valid_command = !request.HasParseError() && request.IsObject() &&
        request.HasMember("cmd") && request["cmd"].IsString();
    if (!valid_command)
        return {Error("invalid_request", "expected a JSON object with string cmd and nesting at most 64", frame, false)};
    // v2 clients may correlate responses with any string or integer id.
    rapidjson::Value client_id;
    if (request.HasMember("id") && (request["id"].IsString() || request["id"].IsInt64() || request["id"].IsUint64()))
        client_id.CopyFrom(request["id"], request.GetAllocator());
    const auto reply = [&](const std::string &response, bool finished = false, int exit_code = 0) {
        Reply result{response, finished, exit_code};
        if (client_id.IsNull()) return result;
        rapidjson::Document document;
        document.Parse(response.data(), response.size());
        if (!document.IsObject()) return result;
        document.RemoveMember("id");
        rapidjson::Value id;
        id.CopyFrom(client_id, document.GetAllocator());
        document.AddMember("id", id, document.GetAllocator());
        result.response = Serialize(document);
        return result;
    };
    const std::string name(request["cmd"].GetString(), request["cmd"].GetStringLength());
    if (name.find('\0') != std::string::npos)
        return reply(Error("invalid_request", "cmd must not contain a NUL byte", frame, false));
    if (name == "_bootstrap" || request.HasMember("_pocket_request_id") ||
        request.HasMember("_initial_observation"))
        return reply(Error("invalid_request", "private worker protocol fields are reserved", frame, false));
    if (name == "quit") {
        if (worker.Exists()) {
            const Deadline end = host.deadline();
            try {
                worker.Exchange(line, end);
                worker.WaitForExit(end);
            } catch (const WorkerFailure &) {
                // Quit can always dispose of an unhealthy worker.
            }
        }
        const bool reaped = worker.Stop();
        return reply(reaped ? QuitResponse(frame)
                            : Error("worker_cleanup_failed", "worker did not terminate", frame, true),
                     true, reaped ? 0 : 1);
    }
    if (name == "describe" && !worker.Exists()) {
        auto cached = ParseResponse(host.description);
        SetBool(cached, "runtime_available", false);
        SetBool(cached, "requires_reset", true);
        return reply(Serialize(cached));
    }
    try {
        const Deadline end = host.deadline();
        std::string response;
        if (name == "reset") {
            if (request.HasMember("seed") && !request["seed"].IsUint())
                return reply(Error("invalid_request", "seed must be uint32", frame, false));
            if (request.HasMember("seed")) host.seed = request["seed"].GetUint();
            // Rebuild a process, not just an Engine: OOM/panic and swallowed
            // budget errors cannot contaminate the new Lua VM.
            if (worker.Exists()) {
                try {
                    const Deadline grace = std::min(end, Clock::now() + std::chrono::milliseconds(100));
                    worker.Exchange(R"({"cmd":"quit"})", grace);
                    worker.WaitForExit(grace);
                } catch (const WorkerFailure &) {}
            }
            response = host.start(end);
        } else if (!worker.Exists()) {
            return reply(Error("worker_unavailable", "runtime unavailable; reset required", frame, true));
        }
        if (name != "reset") response = worker.Exchange(line, end);
        auto document = ParseResponse(response);
        if (document.HasMember("frame") && document["frame"].IsInt()) frame = document["frame"].GetInt();
        if (name == "describe" && document["ok"].GetBool()) {
            AddExecutionLimits(document, host.limits);
            host.description = Serialize(document);
            response = host.description;
        }
        if (document.HasMember("requires_reset") && document["requires_reset"].IsBool() &&
            document["requires_reset"].GetBool()) {
            if (!worker.Stop())
                return reply(Error("worker_cleanup_failed", "worker did not terminate", frame, true), true, 1);
        }
        return reply(response);
    } catch (const WorkerFailure &error) {
        const bool reaped = worker.Stop();
        return reply(Error(error.code, error.what(), frame, true, error.response), !reaped, reaped ? 0 : 1);
    }
}

int RunAgentSupervisor(const std::filesystem::path &executable,
                       const std::filesystem::path &project, std::uint32_t seed,
                       const AgentExecutionLimits &limits, AgentReplayRecorder *recorder) {
    AgentHost host(executable, project, seed, limits);
    std::string failure;
    if (!host.Start(failure)) {
        std::cout << failure << std::endl;
        return 1;
    }
    if (recorder) recorder->Begin(host.Seed(), host.InitialObservation());
    std::string line;
    bool oversized = false;
    while (ReadCommand(std::cin, line, oversized)) {
        const auto reply = host.Handle(line, oversized);
        std::cout << reply.response << std::endl;
        if (recorder && !oversized) recorder->Record(line, reply.response);
        if (reply.finished) return reply.exit_code;
    }
    return host.Stop() ? 0 : 1;
}
