#include "core/FrameClock.h"
#include "app/runtime/RuntimeApp.h"
#include "app/runtime/AgentSession.h"
#include "app/runtime/AgentSupervisor.h"
#include "app/runtime/AgentReplay.h"
#include "app/runtime/AgentMcpServer.h"
#include "engine/scripting/ComponentManager.h"
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <algorithm>
#include <charconv>
#include <set>
#include <stdexcept>
#include "engine/core/Engine.h"
#include "shared/resources/ResourcePath.h"
#include "rapidjson/document.h"
#include "rapidjson/filereadstream.h"
#include <cstdio>
#include <filesystem>
#include <iostream>

namespace {

bool TryReadJsonFile(const std::filesystem::path &path,
                     rapidjson::Document &out_document) {
    FILE *file_pointer = nullptr;
#ifdef _WIN32
    fopen_s(&file_pointer, path.string().c_str(), "rb");
#else
    file_pointer = fopen(path.string().c_str(), "rb");
#endif
    if (file_pointer == nullptr) return false;

    char buffer[65536];
    rapidjson::FileReadStream stream(file_pointer, buffer, sizeof(buffer));
    out_document.ParseStream(stream);
    std::fclose(file_pointer);
    return !out_document.HasParseError() && out_document.IsObject();
}

bool TryReadProjectRootFromDocument(const rapidjson::Document &document,
                                    std::filesystem::path &out_project_root) {
    const char *project_root_key = nullptr;
    if (document.HasMember("current_project_root") &&
        document["current_project_root"].IsString()) {
        project_root_key = "current_project_root";
    } else if (document.HasMember("current_project_resources_root") &&
               document["current_project_resources_root"].IsString()) {
        project_root_key = "current_project_resources_root";
    }
    if (project_root_key == nullptr) return false;

    const std::filesystem::path project_root =
        ResourcePath::NormalizeProjectRoot(document[project_root_key].GetString());
    if (!std::filesystem::exists(project_root) ||
        !std::filesystem::is_directory(project_root)) {
        return false;
    }

    out_project_root = project_root;
    return true;
}

bool TryReadEditorProjectRoot(std::filesystem::path &out_project_root) {
    rapidjson::Document project_config;
    if (TryReadJsonFile(ResourcePath::EditorProjectsConfigPath(),
                        project_config) &&
        TryReadProjectRootFromDocument(project_config, out_project_root)) {
        return true;
    }

    rapidjson::Document legacy_editor_config;
    if (TryReadJsonFile(ResourcePath::EditorConfigPath(),
                        legacy_editor_config) &&
        TryReadProjectRootFromDocument(legacy_editor_config,
                                       out_project_root)) {
        return true;
    }

    return false;
}

// A packaged game (`pocket package`) ships its project as "project" beside
// the runtime: next to the executable on Windows and Linux, in the app's
// Resources on macOS (SDL's base path), and preloaded at /project on the web.
// The runtime then starts it directly, from that folder, where the package
// also keeps the engine's .engine/system resources.
std::filesystem::path PackagedProjectRoot() {
    const char *base = SDL_GetBasePath();
    if (base == nullptr) return {};
    const std::filesystem::path project = std::filesystem::path(base) / "project";
    std::error_code error;
    return std::filesystem::is_regular_file(project / "game.config", error) ? project : std::filesystem::path();
}

void ApplyEditorSelectedProjectRootForRuntime() {
    std::filesystem::path project_root;
    if (!TryReadEditorProjectRoot(project_root)) return;
    ResourcePath::SetResourcesRootPath(project_root);
}

} // namespace

// Boot the standalone runtime executable and stay in the traditional game
// loop until the engine requests quit.
int RuntimeApp::Run(int argc, char **argv) {
    std::ios::sync_with_stdio(false);
    try {
        bool headless = false, agent = false, agent_worker = false, mcp = false;
        std::set<std::string> mcp_access{"read", "simulate", "modify"};
        AgentExecutionLimits limits;
        bool seed_provided = false;
        int frame_limit = 0;
        std::filesystem::path capture;
        std::filesystem::path save_directory;
        std::filesystem::path project;
        std::filesystem::path record, replay;
        std::string start_scene;
        std::uint32_t seed = 0;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--headless") headless = true;
            else if (arg == "--agent") agent = true;
            else if (arg == "--agent-worker") agent_worker = true;
            else if (arg == "--mcp") mcp = true;
            else if (arg == "--mcp-access" && i + 1 < argc) {
                mcp_access.clear();
                const std::string value = argv[++i];
                for (std::size_t start = 0; start <= value.size();) {
                    const std::size_t end = std::min(value.find(',', start), value.size());
                    const std::string level = value.substr(start, end - start);
                    if (level != "read" && level != "simulate" && level != "modify")
                        throw std::runtime_error("--mcp-access takes a comma list of read, simulate, modify");
                    mcp_access.insert(level);
                    start = end + 1;
                }
            }
            else if ((arg == "--instruction-budget" || arg == "--lua-heap-budget" ||
                      arg == "--request-timeout-ms") && i + 1 < argc) {
                const std::string value = argv[++i];
                std::uint64_t amount = 0;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), amount);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || amount == 0)
                    throw std::runtime_error(arg + " expects a positive integer");
                if (arg == "--instruction-budget") limits.script_limits.instruction_limit = amount;
                else if (arg == "--lua-heap-budget") limits.script_limits.heap_limit_bytes = amount;
                else {
                    if (amount < 100 || amount > 60000)
                        throw std::runtime_error("request timeout must be in [100, 60000] ms");
                    limits.request_timeout_ms = static_cast<std::uint32_t>(amount);
                }
            }
            else if (arg == "--project" && i + 1 < argc) project = argv[++i];
            else if (arg == "--save-dir" && i + 1 < argc) {
                const std::string value = argv[++i];
                if (value.empty()) throw std::runtime_error("--save-dir must not be empty");
                save_directory = std::filesystem::path(std::u8string(value.begin(), value.end()));
            }
            else if (arg == "--scene" && i + 1 < argc) {
                start_scene = argv[++i];
                if (start_scene.empty()) throw std::runtime_error("--scene must not be empty");
            }
            else if (arg == "--capture" && i + 1 < argc) capture = argv[++i];
            else if (arg == "--record" && i + 1 < argc) record = argv[++i];
            else if (arg == "--replay" && i + 1 < argc) replay = argv[++i];
            else if (arg == "--frames" && i + 1 < argc) {
                const std::string value = argv[++i];
                auto result = std::from_chars(value.data(), value.data() + value.size(), frame_limit);
                if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
                    frame_limit < 1 || frame_limit > 36000)
                    throw std::runtime_error("--frames expects an integer in [1, 36000]");
            }
            else if (arg == "--seed" && i + 1 < argc) {
                seed_provided = true;
                const std::string value = argv[++i];
                auto result = std::from_chars(value.data(), value.data() + value.size(), seed);
                if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
                    throw std::runtime_error("--seed expects uint32");
            } else if (arg == "--help") {
                std::cout << "game [--project PATH] [--scene NAME] [--seed UINT32] [--frames N [--capture PNG]]\n"
                             "game --project PATH --headless --agent [--scene NAME] [--seed UINT32] [--save-dir ROOT] [--record REPLAY]\n"
                             "game --project PATH --headless --replay REPLAY [--save-dir ROOT]\n"
                             "game --project PATH --headless --mcp [--mcp-access read,simulate,modify] [--seed UINT32]\n";
                return 0;
            } else throw std::runtime_error("unknown or incomplete option: " + arg);
        }
        if (!save_directory.empty()) {
            const auto path = std::filesystem::absolute(save_directory).generic_u8string();
            const std::string value(path.begin(), path.end());
            if (SDL_setenv_unsafe("POCKET_SAVE_ROOT", value.c_str(), 1) != 0)
                throw std::runtime_error("unable to configure save directory");
        }
        // The engine reads it in place of game.config's initial_scene; agent
        // workers inherit it, so a reset starts there too.
        if (!start_scene.empty() && SDL_setenv_unsafe("POCKET_INITIAL_SCENE", start_scene.c_str(), 1) != 0)
            throw std::runtime_error("unable to configure the start scene");
        if (int(agent) + int(agent_worker) + int(!replay.empty()) + int(mcp) > 1)
            throw std::runtime_error("choose one of --agent, --agent-worker, --replay and --mcp");
        if (!record.empty() && !agent) throw std::runtime_error("--record requires --agent");
        const bool automation = agent || agent_worker || !replay.empty() || mcp;
        if (headless != automation) throw std::runtime_error("--headless and --agent must be used together");
        if (automation && (frame_limit != 0 || !capture.empty()))
            throw std::runtime_error("--frames/--capture require graphical mode");
        if (!capture.empty() && frame_limit == 0)
            throw std::runtime_error("--capture requires --frames");
        if (!project.empty()) {
            project = std::filesystem::absolute(project);
            if (!std::filesystem::is_regular_file(project / "game.config"))
                throw std::runtime_error("project must contain game.config");
            ResourcePath::SetResourcesRootPath(project);
        } else if (automation) throw std::runtime_error("agent mode requires explicit --project PATH");
        else if (const auto packaged = PackagedProjectRoot(); !packaged.empty()) {
            std::filesystem::current_path(packaged.parent_path());
            project = packaged;
            ResourcePath::SetResourcesRootPath(project);
        } else ApplyEditorSelectedProjectRootForRuntime();
        if (agent_worker) {
            ScriptBudget::Configure(limits.script_limits);
            return RunAgentSession(project, seed);
        }
        if (agent || mcp || !replay.empty()) {
            std::filesystem::path executable = argv[0];
            if (!std::filesystem::exists(executable))
                executable = std::filesystem::path(SDL_GetBasePath()) / executable.filename();
            executable = std::filesystem::canonical(executable);
            if (!replay.empty()) return RunAgentReplay(executable, project, replay, limits);
            if (mcp) return RunAgentMcpServer(executable, project, seed, limits, mcp_access);
            if (record.empty()) return RunAgentSupervisor(executable, project, seed, limits);
            AgentReplayRecorder recorder(record, project);
            return RunAgentSupervisor(executable, project, seed, limits, &recorder);
        }
        if (seed_provided) ComponentManager::SetRandomSeed(seed);
        Engine engine;
        if (!engine.GetLastSceneLoadError().empty())
            throw std::runtime_error(engine.GetLastSceneLoadError());
        if (frame_limit == 0) engine.GameLoop();
        else {
            engine.InitializeRuntime();
            for (int frame = 0; frame < frame_limit && engine.IsRunning(); ++frame) {
                engine.RunSingleFrame();
                if (frame == frame_limit - 1 && !capture.empty()) {
                    SDL_Surface *surface = SDL_RenderReadPixels(engine.GetRenderer(), nullptr);
                    if (surface == nullptr) throw std::runtime_error(SDL_GetError());
                    const bool saved = IMG_SavePNG(surface, capture.string().c_str());
                    SDL_DestroySurface(surface);
                    if (!saved) throw std::runtime_error(SDL_GetError());
                }
                engine.PresentFrame();
                FrameClock::DelayToMaintain60FPS();   // one tick per frame
            }
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "pocket2d: " << error.what() << std::endl;
        return 1;
    }
}
