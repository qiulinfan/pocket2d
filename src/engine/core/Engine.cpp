#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_ASYNC_JS(void, WaitForAnimationFrame, (), { await new Promise(resolve => requestAnimationFrame(resolve)); });
#endif
#include "core/Engine.h"
#include "rendering/SplashScreen.h"
#include <string_view>
#include <charconv>
#include "input/ActionMap.h"
#include "net/Netplay.h"
#include "shared/resources/ResourcePath.h"
#include "core/Profile.h"
#include "core/Log.h"
#include "audio/AudioManager.h"
#include "core/FrameClock.h"
#include "scripting/ComponentManager.h"
#include "scripting/GameSave.h"
#include "scripting/SceneComponentPlan.h"
#include "scripting/RuntimeActorPlan.h"
#include "scripting/ScriptReloadCandidate.h"
#include "particles/ParticleManager.h"
#include "rendering/Renderer.h"
#include "rendering/SDLRenderHelper.h"
#include "scene/Scene.h"
#include "input/Input.h"
#include "input/SDLEventHelper.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include "SDL3_image/SDL_image.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <iterator>
#include <stdexcept>
#include <unordered_set>
#include <type_traits>
#include <vector>

namespace {

struct SceneTransitionScope {
    bool &active;
    explicit SceneTransitionScope(bool &value):active(value){active=true;}
    ~SceneTransitionScope(){active=false;}
};

struct PreparedScene {
    std::vector<Actor> incoming;
    std::list<Actor> nodes;
    // Reverse destruction order matters: instance refs go before the script
    // plan resumes the live VM's collector.
    std::unique_ptr<SceneScriptPlan> scripts;
    std::unique_ptr<SceneComponentPlan> components;
    ~PreparedScene() {
        components.reset(); scripts.reset();
        ComponentManager::ClearPreparedTilemapAssets();
        ComponentManager::ClearPreparedSpriteAnimationAssets();
    }
};
bool PrepareScene(const SceneFormat::SceneAsset &asset,
                  const std::list<Actor> &current, bool keep_persistent,
                  PreparedScene &prepared, std::string &error) {
    try {
        if (!SceneFormat::ValidateSceneAssetForRuntime(asset,&error)) {error=ScriptReloadCandidate::SanitizeDiagnosticText(error);return false;}
        prepared.incoming=SceneFormat::BuildRuntimeActors(asset);
        std::unordered_set<Actor::UID> incoming_uids;
        for(const auto &actor:prepared.incoming)incoming_uids.insert(actor.uid);
        for(const auto &actor:prepared.incoming)if(actor.parent_uid!=Actor::kInvalidUID && !incoming_uids.contains(actor.parent_uid)) {
            error="incoming actor "+std::to_string(actor.uid)+" references a parent outside the incoming scene";return false;
        }
        std::vector<Actor::UID> persistent;
        std::unordered_set<Actor::UID> retained;
        if (keep_persistent) {
            for (const auto &actor:current) if (!actor.runtime_destroyed&&actor.dont_destroy_on_scene_load) {
                retained.insert(actor.uid);persistent.push_back(actor.uid);
            }
            for (const auto &actor:current) if(retained.contains(actor.uid)&&actor.parent_uid!=Actor::kInvalidUID&&
                                               !retained.contains(actor.parent_uid)) {
                error="persistent actor "+std::to_string(actor.uid)+" has a nonpersistent parent; retain its parent or detach it before scene load";
                return false;
            }
            for (const auto &actor:prepared.incoming) if(retained.contains(actor.uid)) {
                error="incoming scene actor uid "+std::to_string(actor.uid)+" conflicts with a persistent actor";return false;
            }
        }
        prepared.scripts=ComponentManager::PrepareSceneScripts(asset.scene_subdirectory,prepared.incoming,persistent,error);
        if(!prepared.scripts)return false;
        prepared.components=PrepareSceneComponents(prepared.incoming,*prepared.scripts,error);
        if(!prepared.components){error=ScriptReloadCandidate::SanitizeDiagnosticText(error);return false;}
        ComponentManager::PrepareTilemapAssets(prepared.incoming);
        ComponentManager::PrepareSpriteAnimationAssets(prepared.incoming);
        // Allocate actor nodes before the commit boundary. Splicing them later
        // preserves addresses without an allocation after the old world is gone.
        for(auto &actor:prepared.incoming)prepared.nodes.emplace_back(std::move(actor));
        return true;
    }catch(const std::exception &failure){error=ScriptReloadCandidate::SanitizeDiagnosticText(failure.what());return false;}
}

bool ShouldLogSceneDiagnostics() {
    return std::getenv("ENGINE_LOG_SCENE_STATS") != nullptr;
}

bool ShouldLogPerformanceDiagnostics() {
    return std::getenv("ENGINE_LOG_PERF") != nullptr;
}

bool ShouldLogRendererDiagnostics() {
    return std::getenv("ENGINE_LOG_RENDERER") != nullptr;
}

std::string ToLowerASCII(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string ReadEnvironmentString(const char *name) {
    const char *value = std::getenv(name);
    return value == nullptr ? "" : value;
}

// POCKET_WINDOW_DISPLAY picks the display for new windows: a 0-based index
// into SDL's display list or a case-insensitive part of a display's name
// (for example an external monitor's model). Unset or unmatched: primary.
SDL_DisplayID ChooseWindowDisplay() {
    const std::string wanted = ToLowerASCII(ReadEnvironmentString("POCKET_WINDOW_DISPLAY"));
    const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    if (wanted.empty()) return primary;
    int count = 0;
    SDL_DisplayID *displays = SDL_GetDisplays(&count);
    SDL_DisplayID chosen = 0;
    int index = -1;
    const auto parsed = std::from_chars(wanted.data(), wanted.data() + wanted.size(), index);
    const bool numeric = parsed.ec == std::errc{} && parsed.ptr == wanted.data() + wanted.size();
    for (int i = 0; displays != nullptr && i < count && chosen == 0; ++i) {
        const char *name = SDL_GetDisplayName(displays[i]);
        if (numeric ? i == index : (name != nullptr && ToLowerASCII(name).find(wanted) != std::string::npos))
            chosen = displays[i];
    }
    SDL_free(displays);
    if (chosen == 0) Log::Warning("render", "POCKET_WINDOW_DISPLAY matched no display; using the primary display");
    return chosen != 0 ? chosen : primary;
}

SDL_Renderer *CreateConfiguredRenderer(SDL_Window *window) {
    const std::string requested_driver =
        ToLowerASCII(ReadEnvironmentString("POCKET_RENDER_DRIVER"));
    const char *driver = requested_driver.empty() || requested_driver == "auto"
                             ? nullptr : requested_driver.c_str();
    SDL_Renderer *renderer = SDLRenderHelper::SDL_CreateRenderer(window, driver);
    if (renderer == nullptr) {
        throw std::runtime_error("renderer '" +
            (driver == nullptr ? std::string("auto") : requested_driver) +
            "' unavailable: " + SDL_GetError());
    }
    const std::string vsync = ToLowerASCII(ReadEnvironmentString("POCKET_RENDER_VSYNC"));
    if (!vsync.empty()) {
        const bool enabled = vsync == "1" || vsync == "true" || vsync == "on";
        if (!SDL_SetRenderVSync(renderer, enabled ? 1 : 0)) {
            const std::string error = SDL_GetError();
            SDL_DestroyRenderer(renderer);
            throw std::runtime_error("requested renderer VSync unavailable: " + error);
        }
    }
    if (ShouldLogRendererDiagnostics()) {
        int interval = 0;
        SDL_GetRenderVSync(renderer, &interval);
        Log::Info("render", std::string("driver=") + SDL_GetRendererName(renderer) +
                                " vsync=" + std::to_string(interval));
    }
    return renderer;
}

std::string BuildUniqueRuntimeActorName(const std::list<Actor> &actors,
                                        const std::string &base_name) {
    const std::string safe_base_name =
        base_name.empty() ? "New Actor" : base_name;
    std::unordered_set<std::string> used_names;
    used_names.reserve(actors.size());
    for (const Actor &actor : actors) {
        if (actor.runtime_destroyed) continue;
        used_names.insert(actor.actor_name);
    }

    if (used_names.find(safe_base_name) == used_names.end()) {
        return safe_base_name;
    }

    int suffix = 1;
    while (true) {
        const std::string candidate = safe_base_name + " (" + std::to_string(suffix) + ")";
        if (used_names.find(candidate) == used_names.end()) {
            return candidate;
        }
        ++suffix;
    }
}

bool TryReadRuntimePropertyAsBool(
    const std::vector<Actor::ComponentProperty> &properties,
    const std::string &property_name, bool &out_value) {
    for (const Actor::ComponentProperty &property : properties) {
        if (property.name != property_name) continue;
        if (const bool *typed_value = std::get_if<bool>(&property.value)) {
            out_value = *typed_value;
            return true;
        }
        return false;
    }
    return false;
}

bool TryReadRuntimePropertyAsString(
    const std::vector<Actor::ComponentProperty> &properties,
    const std::string &property_name, std::string &out_value) {
    for (const Actor::ComponentProperty &property : properties) {
        if (property.name != property_name) continue;
        if (const std::string *typed_value =
                std::get_if<std::string>(&property.value)) {
            out_value = *typed_value;
            return true;
        }
        return false;
    }
    return false;
}

/*
Read only the actor-local Rigidbody request from live runtime component state.
Hierarchy-derived fields such as nearest dynamic ancestor are layered on later
when the runtime physics cache walks parent links.
*/
PhysicsHierarchy::State BuildRuntimeActorPhysicsSelfState(
    Actor::UID actor_uid,
    const std::vector<Actor::ComponentSpec> &component_specs) {
    PhysicsHierarchy::State state;
    for (const Actor::ComponentSpec &component_spec : component_specs) {
        if (component_spec.type != "Rigidbody") continue;

        state.has_rigidbody_self = true;
        state.rigidbody_component_key = component_spec.key;

        const std::vector<Actor::ComponentProperty> properties =
            ComponentManager::GetRuntimeComponentProperties(actor_uid,
                                                           component_spec.key);
        bool enabled = true;
        TryReadRuntimePropertyAsBool(properties, "enabled", enabled);
        state.rigidbody_enabled_self = enabled;

        std::string requested_body_type = "dynamic";
        TryReadRuntimePropertyAsString(properties, "body_type",
                                       requested_body_type);
        state.requested_body_type = requested_body_type;
        state.has_dynamic_rigidbody_self = enabled && requested_body_type == "dynamic";
        state.effective_body_type = enabled ? requested_body_type : PhysicsHierarchy::kNoBodyType;
        break;
    }
    return state;
}

} // namespace

Engine::Engine() : Engine(World::Current()) {}

Engine::Engine(World &world) : world_(world) {
    WorldScope world_scope(world_);
    /*
    Engine owns both standalone runtime and editor-embedded runtime state.
    Construction loads config and prepares the initial actor/component world,
    but SDL/window resources are created later by InitializeRuntime().
    */
    try {
        FrameClock::Reset();
        Scene::SetActiveSceneSubdirectory({});
        GameSave::Configure({});
        config_ = GameConfig::Read();
        ActionMap::Load(ResourcePath::ResourcesRootPath());
        GameSave::Configure(config_.save_namespace);
        // `game --scene NAME` starts elsewhere than the project's initial scene.
        if (const char *start = SDL_getenv("POCKET_INITIAL_SCENE"); start != nullptr && *start != '\0')
            config_.initial_scene_name = start;
        current_scene_name = config_.initial_scene_name;
        SceneFormat::SceneAsset initial;
        if(!config_.initial_scene_name.empty())
            initial=SceneFormat::LoadSceneAsset(config_.initial_scene_name,{});
        Scene::SetActiveSceneSubdirectory(initial.scene_subdirectory);
        ComponentManager::Initialize();
        ComponentManager::BootstrapComponentTypes(initial.scene_subdirectory);
        ComponentManager::BindEngine(this);
        ComponentManager::ClearActorComponents();
        if(!config_.initial_scene_name.empty())LoadSceneAsset(initial);
        if(!scene_reset_required_){
            ComponentManager::BindActorsForScene(actors);
            rebuildRuntimeActorUIDMap();
        }
    } catch (...) {
        // Constructors do not run ~Engine on failure. Release the Lua VM and
        // partially constructed physics/components before the host can retry.
        ShutdownRuntime();
        throw;
    }
}

Engine::~Engine() {
    ShutdownRuntime();
}

void Engine::GameLoop() {
    WorldScope world_scope(world_);
    InitializeRuntime();
    if (config_.splash_screen && !SplashScreen::Show(renderer, config_.splash_seconds)) is_game_running = false;
    frame_rate_overlay_.visible = config_.show_fps;
    if (const char *hot_reload = SDL_getenv("POCKET_HOT_RELOAD"); hot_reload && std::string_view(hot_reload) == "1")
        SetAssetHotReload(true);

    // Gameplay ticks 60 times a second of real time, whatever the frame rate
    // (a fixed step keeps physics and replays deterministic). Frames are drawn
    // as often as possible between ticks unless game.config caps them
    // (max_fps) or waits for the display (vsync). A frame between two ticks
    // shows the camera and keyed draws (actor sprites, Lua draws given a key)
    // part of the way from the previous tick to the latest, by how far real
    // time is towards the next tick; game.config's frame_interpolation: false
    // shows the latest tick instead. Either way only presentation changes.
    // Input that arrives between ticks waits for the next tick.
    if (config_.vsync && ReadEnvironmentString("POCKET_RENDER_VSYNC").empty() && !SDL_SetRenderVSync(renderer, 1))
        Log::Warning("render", std::string("vsync unavailable: ") + SDL_GetError());
    constexpr Uint64 kTickNs = 1000000000ULL / 60;
    constexpr Uint64 kMaxTicksPerFrame = 5;   // after a stall, drop the backlog instead of racing
    const Uint64 frame_budget_ns = config_.max_fps > 0 ? 1000000000ULL / config_.max_fps : 0;
    const bool interpolate = config_.frame_interpolation;
    glm::vec2 previous_camera = camera_position;
    float previous_zoom = runtime_zoom_factor;
    Uint64 previous = SDL_GetTicksNS();
    Uint64 lag = kTickNs;   // tick at once, so the first frame has something to draw
    while (IsRunning()) {
        const Uint64 frame_start = SDL_GetTicksNS();
        lag = std::min(lag + (frame_start - previous), kTickNs * kMaxTicksPerFrame);
        previous = frame_start;

        bool quit_requested = false;
        SDL_Event event;
        while (SDLEventHelper::SDL_PollEvent(&event)) ProcessSDLEvent(event, quit_requested);
        Netplay::Pump();
        int ticks = 0;
        for (; lag >= kTickNs && IsRunning(); lag -= kTickNs, ++ticks) {
            previous_camera = camera_position;
            previous_zoom = runtime_zoom_factor;
            const std::uint64_t scene_revision = scene_load_attempt_revision_;
            if (Netplay::Running()) {
                // Netplay decides whether the tick runs (lockstep waits for the
                // peer's input) and first resimulates corrected ticks (rollback),
                // whose draws are dropped.
                Netplay::Pump();
                const bool ran = Netplay::Tick([&](bool resimulating) {
                    advanceGameplay();
                    if (scene_reset_required_) return;
                    finishGameplayFrame();
                    if (resimulating) Renderer::DiscardFrameRequests();
                });
                if (!ran) {
                    lag = std::min(lag, kTickNs * 2);
                    break;
                }
            } else {
                advanceGameplay();
                if (scene_reset_required_) break;
                finishGameplayFrame();
            }
            if (scene_reset_required_) break;
            // A new scene starts afresh instead of moving from the old one.
            const bool same_scene = scene_revision == scene_load_attempt_revision_;
            if (!same_scene) {
                previous_camera = camera_position;
                previous_zoom = runtime_zoom_factor;
            }
            if (interpolate) {
                // Actor sprites are held with the tick, so they pair up with
                // the previous tick's by actor.
                ComponentManager::ResolveTransformHierarchy();
                ComponentManager::QueueBuiltinRenderers();
            }
            Renderer::HoldFrameRequests(interpolate && same_scene);
        }
        if (quit_requested) is_game_running = false;
        if (!IsRunning()) break;
        if (!scene_reset_required_) {
            const float alpha = interpolate ? static_cast<float>(lag) / static_cast<float>(kTickNs) : 1.0f;
            glm::vec2 camera = camera_position;
            float zoom = runtime_zoom_factor;
            // Moving more than a screen in one tick is a cut, not motion.
            const float screen = static_cast<float>(std::max(config_.window_width, config_.window_height)) /
                                 (100.0f * std::clamp(zoom, kMinZoomFactor, kMaxZoomFactor));
            if (alpha < 1.0f) {
                const glm::vec2 moved = camera - previous_camera;
                if (std::max(std::abs(moved.x), std::abs(moved.y)) <= screen) camera = previous_camera + moved * alpha;
                zoom = previous_zoom + (zoom - previous_zoom) * alpha;
            }
            Renderer::RequeueHeldFrameRequests(alpha, screen);
            render(camera, zoom, !interpolate);
        }
        const Uint64 work_end = SDL_GetTicksNS();
        frame_rate_overlay_.RecordFrame(work_end - frame_start, ticks, work_end);
        frame_rate_overlay_.Draw(renderer);
        PresentFrame();
#ifdef __EMSCRIPTEN__
        // The browser shows a frame per display refresh: yield until the next
        // one (ASYNCIFY), which also keeps the page responsive.
        WaitForAnimationFrame();
#endif
        if (frame_budget_ns > 0) {
            const Uint64 elapsed = SDL_GetTicksNS() - frame_start;
            if (elapsed < frame_budget_ns) SDL_DelayPrecise(frame_budget_ns - elapsed);
        }
    }

    ShutdownRuntime();
}

/*
Editor can call this without entering GameLoop()
so the host can bootstrap runtime services on demand
*/
void Engine::InitializeRuntime() {
    WorldScope world_scope(world_);
    if(scene_reset_required_)throw std::runtime_error("runtime requires reset after failed publication");
    if (runtime_initialized_ || runtime_shutdown_) return;
    is_game_running = true;
    initialize();
    runtime_initialized_ = true;
}

void Engine::InitializeEmbedded(SDL_Window *host_window, SDL_Renderer *host_renderer) {
    WorldScope world_scope(world_);
    if(scene_reset_required_)throw std::runtime_error("runtime requires reset after failed publication");
    if (runtime_initialized_ || runtime_shutdown_) return;
    if (host_window == nullptr || host_renderer == nullptr)
        throw std::invalid_argument("embedded runtime requires a host window and renderer");
    window = host_window;
    renderer = host_renderer;
    borrows_presenter_ = true;
    FrameClock::Reset();
    ::Input::Init();
    ::Input::SetExternalInputEnabled(false);
    runtime_zoom_factor = (config_.zoom_factor > 0.0f)
                              ? std::clamp(config_.zoom_factor, kMinZoomFactor,
                                           kMaxZoomFactor)
                              : 1.0f;
    ResetPerformanceCounters();
    is_game_running = true;
    runtime_initialized_ = true;
}

void Engine::InitializeHeadless() {
    WorldScope world_scope(world_);
    if(scene_reset_required_)throw std::runtime_error("runtime requires reset after failed publication");
    if (runtime_initialized_ || runtime_shutdown_) return;
    if (!SDL_Init(0)) {
        throw std::runtime_error(std::string("headless SDL initialization failed: ") +
                                 SDL_GetError());
    }
    FrameClock::Reset();
    ::Input::Init();
    ::Input::SetExternalInputEnabled(true);
    world_.audio_output_enabled = false;
    runtime_zoom_factor = (config_.zoom_factor > 0.0f)
                              ? std::clamp(config_.zoom_factor, kMinZoomFactor,
                                           kMaxZoomFactor)
                              : 1.0f;
    ResetPerformanceCounters();
    is_game_running = true;
    runtime_initialized_ = true;
}

/*
Tear down SDL objects and runtime only caches
They need to be cleared once actors and components are destroyed.
*/
void Engine::ShutdownRuntime() {
    WorldScope world_scope(world_);
    
    if (runtime_shutdown_) return;
    runtime_shutdown_ = true;
    is_game_running = false;
    if (Netplay::GetStatus().state != Netplay::State::Idle) Netplay::Leave();

    destroyRuntimeRenderTarget();
    destroyScenePreviewRenderTarget();
    runtime_actor_by_uid_.clear();
    ComponentManager::ClearPreparedTilemapAssets();
    ComponentManager::ClearPreparedSpriteAnimationAssets();
    ComponentManager::Shutdown();
    Scene::SetActiveSceneSubdirectory({});
    ParticleManager::Clear();
    // Renderer caches, the SDL video subsystem and the event/render recording
    // helpers are process-wide; only the Engine that created a window releases
    // them, so a headless Engine cannot tear down another Engine's window.
    // An embedded Engine borrows its host's window and renderer.
    const bool presented = !borrows_presenter_ && (window != nullptr || renderer != nullptr);
    if (presented && renderer != nullptr) Renderer::Shutdown();
    AudioManager::Shutdown();
    if (borrows_presenter_) {
        renderer = nullptr;
        window = nullptr;
    }
    if (renderer != nullptr) {
        SDL_DestroyRenderer(renderer);
        renderer = nullptr;
    }
    if (window != nullptr) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }
    if (offscreen_surface_ != nullptr) {
        SDL_DestroySurface(offscreen_surface_);
        offscreen_surface_ = nullptr;
    }
    if (sdl_subsystems_ != 0) {
        SDL_QuitSubSystem(sdl_subsystems_);
        sdl_subsystems_ = 0;
    }
    if (presented) {
        SDLEventHelper::Reset();
        SDLRenderHelper::Reset();
    }
    ::Input::Init();
    ::Input::SetExternalInputEnabled(false);
    world_.audio_output_enabled = true;
    runtime_initialized_ = false;
}

/*
Just wrapper, checking if quit is requested this frame,
then call the real RunSingleFrame(bool quit_requested_this_frame)
*/
void Engine::RunSingleFrame() {
    WorldScope world_scope(world_);
    if (!IsRunning()) return;

    bool quit_requested_this_frame = false;
    SDL_Event event;
    /* Check whether this frame requests a runtime quit. */
    while (SDLEventHelper::SDL_PollEvent(&event)) {
        ProcessSDLEvent(event, quit_requested_this_frame);
    }
    RunSingleFrame(quit_requested_this_frame);
}

void Engine::RunSingleFrame(bool quit_requested_this_frame) {
    WorldScope world_scope(world_);
    if (!IsRunning()) return;
    /* Run one full gameplay frame: scene load, starts, update, render, input late update. */
    advanceGameplay();
    if(scene_reset_required_)return;
    render();
    finishGameplayFrame();

    /* Apply deferred quit after the frame finishes. */
    if (quit_requested_this_frame) {
        is_game_running = false;
    }
}

void Engine::advanceGameplay() {
    POCKET_ZONE("Engine::advanceGameplay");
    if(scene_transition_active_)throw std::logic_error("cannot advance gameplay during a scene transaction");
    processPendingSceneLoad();
    if(scene_reset_required_)return;
    ::Input::BeginFrame();
    ComponentManager::ApplyEffectiveRigidbodyBodyTypes();
    ComponentManager::ProcessPendingOnStart();
    if (scene_reset_required_) return;
    update();
}

void Engine::finishGameplayFrame() {
    POCKET_FRAME_MARK();
    RecordGameplayFrame();
    ::Input::LateUpdate();
    FrameClock::AdvanceFrame();
}

void Engine::StepSimulation() {
    WorldScope world_scope(world_);
    POCKET_ZONE("Engine::StepSimulation");
    if (!IsRunning() || !runtime_initialized_) return;
    // Draw requests from the previous tick stay queued until here so a
    // headless observer can render the frame it just observed.
    Renderer::DiscardFrameRequests();
    auto run_tick = [&](bool resimulating) {
        advanceGameplay();
        if (scene_reset_required_) return;
        // The visible path resolves final transforms in render(). Headless observers
        // need the same post-mutation world values even in scenes without physics.
        ComponentManager::ResolveTransformHierarchy();
        finishGameplayFrame();
        if (resimulating) Renderer::DiscardFrameRequests();
    };
    // Headless netplay: wait briefly until the session can tick. A stalled
    // tick returns without advancing (well inside the agent's request
    // deadline), so a peer that only pumps while it steps can catch up.
    Netplay::Pump();
    if (Netplay::Running()) {
        const Uint64 deadline = SDL_GetTicks() + 500;
        while (Netplay::Running() && !Netplay::CanTick() && SDL_GetTicks() < deadline) {
            SDL_Delay(1);
            Netplay::Pump();
        }
        if (Netplay::Running()) {
            Netplay::Tick(run_tick);
            return;
        }
    }
    run_tick(false);
}



/*
renders a frozen preview frame:
no gameplay update (component updates, physics step, or any frame advance).
Edit mode stays static by default; live preview may explicitly flush OnStart.
*/
void Engine::RunRenderFrozenFrame(bool quit_requested_this_frame,
                                  bool process_pending_on_start) {
    WorldScope world_scope(world_);
    POCKET_ZONE("Engine::RunRenderFrozenFrame");
    
    if (!IsRunning()) return;

    /*
    Deferred scene loads still need to become visible in the frozen preview.
    Startup lifecycle is separate so edit mode does not accidentally run game
    logic unless live preview is enabled.
    */
    processPendingSceneLoad();
    if(scene_reset_required_)return;
    ComponentManager::ApplyEffectiveRigidbodyBodyTypes();
    if (process_pending_on_start) {
        ComponentManager::ProcessPendingOnStart();
    }
    ComponentManager::CommitTilemaps();
    render();
    ::Input::LateUpdate();

    if (quit_requested_this_frame) {
        is_game_running = false;
    }
}

/*
Pause-mode path:
keep the last runtime render target exactly as it was on the previous gameplay
frame, and only clear the editor host backbuffer so ImGui can compose over it.
*/
void Engine::RunPresentPausedFrame(bool quit_requested_this_frame) {
    WorldScope world_scope(world_);
    if (!IsRunning()) return;

    if (render_runtime_to_texture_) {
        SDL_SetRenderTarget(renderer, nullptr);
        SDL_SetRenderViewport(renderer, nullptr);
        clearEditorHostFrame();
    }

    ::Input::LateUpdate();
    if (quit_requested_this_frame) {
        is_game_running = false;
    }
}

/*
Notice: this is the input processor for runtime.
The input processor for editor is in EditorApp::Run(), with a single overlay_.ProcessEvent()
But: both standalone runtime and editor embedding feed the same input path
*/
void Engine::ProcessSDLEvent(const SDL_Event &event,
                             bool &quit_requested_this_frame) {
    WorldScope world_scope(world_);
    ::Input::ProcessEvent(event);
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_F3 && !event.key.repeat)
        frame_rate_overlay_.visible = !frame_rate_overlay_.visible;
    if (event.type == SDL_EVENT_QUIT) {
        quit_requested_this_frame = true;
    }
}

/* Present the current backbuffer to the window once drawing is complete. */
void Engine::PresentFrame(bool advance_gameplay_frame) {
    WorldScope world_scope(world_);
    POCKET_ZONE("Engine::PresentFrame");
    if (renderer == nullptr) return;
    SDLRenderHelper::SDL_RenderPresent(renderer, advance_gameplay_frame);
    RefreshPerformanceCounters();
}

/* Report whether the runtime loop is still active. */
bool Engine::IsRunning() const {return is_game_running;}
/* Ask the runtime loop to stop after the current frame. */
void Engine::RequestQuit() {is_game_running = false;}
SDL_Window *Engine::GetWindow() const {return window;}
SDL_Renderer *Engine::GetRenderer() const {return renderer;}


int Engine::GetWindowWidth() const {
    if (window == nullptr) return config_.window_width;
    int width = config_.window_width;
    int height = config_.window_height;
    SDL_GetWindowSize(window, &width, &height);
    return width;
}
int Engine::GetWindowHeight() const {
    if (window == nullptr) return config_.window_height;
    int width = config_.window_width;
    int height = config_.window_height;
    SDL_GetWindowSize(window, &width, &height);
    return height;
}

const GameConfigData &Engine::GetConfig() const {return config_;}
const std::string &Engine::GetLastSceneLoadError() const {
    return last_scene_load_error_;
}
std::size_t Engine::CountLiveActors() const {
    return static_cast<std::size_t>(
        std::count_if(actors.begin(), actors.end(),
                      [](const Actor &actor) { return !actor.runtime_destroyed; }));
}
std::size_t Engine::GetActorCount() const {return CountLiveActors();}
const std::list<Actor> &Engine::GetRuntimeActors() const {return actors;}
const Actor *Engine::GetRuntimeActorByUID(Actor::UID actor_uid) const {
    return findRuntimeActorByUID(actor_uid);
}

/*
Expose the derived physics-hierarchy classification for one live actor.
The cache is rebuilt lazily so editor UI can inspect the latest runtime tree
without forcing every mutation site to eagerly recompute ancestry state.
*/
PhysicsHierarchy::State Engine::GetRuntimePhysicsHierarchyStateByUID(
    Actor::UID actor_uid) const {
    WorldScope world_scope(world_);
    if (actor_uid == PhysicsHierarchy::kInvalidActorUID) return {};
    rebuildRuntimePhysicsHierarchyCache();
    auto state_it = runtime_physics_hierarchy_state_by_uid_.find(actor_uid);
    if (state_it == runtime_physics_hierarchy_state_by_uid_.end()) return {};
    return state_it->second;
}

/*
Runtime-generated actors use a separate UID cursor from scene-backed actors.
The cursor is advanced from the highest live UID whenever runtime actors are
rebound, so instantiate/duplicate can stay O(1) during play.
*/
Actor::UID Engine::AllocateRuntimeGeneratedActorUID() {
    if (next_runtime_generated_actor_uid_ <
        Actor::kRuntimeGeneratedUIDStart) {
        next_runtime_generated_actor_uid_ =
            Actor::kRuntimeGeneratedUIDStart;
    }
    const Actor::UID start = next_runtime_generated_actor_uid_;
    auto advance = [this] {
        next_runtime_generated_actor_uid_ =
            next_runtime_generated_actor_uid_ == std::numeric_limits<Actor::UID>::max()
                ? Actor::kRuntimeGeneratedUIDStart : next_runtime_generated_actor_uid_ + 1;
    };
    while (runtime_actor_by_uid_.count(next_runtime_generated_actor_uid_) != 0) {
        advance();
        if (next_runtime_generated_actor_uid_ == start)
            throw std::runtime_error("runtime actor UID space exhausted");
    }
    const Actor::UID allocated = next_runtime_generated_actor_uid_;
    advance();
    return allocated;
}

/*
Duplicate a live actor entirely inside the runtime world.
This path intentionally does not write back to the authoring scene document;
it is the transient play-mode equivalent of scene-backed duplication.
*/
void Engine::NotifyRuntimeActorPublished(Actor &actor) {
    runtime_actor_by_uid_.insert_or_assign(actor.uid, &actor);
    invalidateRuntimePhysicsHierarchyCache();
}

void Engine::ReportRuntimeActorPublicationFailure(const std::string &message) {
    last_scene_edit_error_.clear();
    last_runtime_actor_error_ = ScriptReloadCandidate::SanitizeDiagnosticText(
        "runtime actor publication failed; reset required: " + message);
    runtime_actor_by_uid_.clear();
    runtime_physics_hierarchy_state_by_uid_.clear();
    runtime_physics_hierarchy_cache_dirty_ = true;
    scene_reset_required_ = true;
    is_game_running = false;
    has_pending_scene_load = false;
    pending_scene_name.clear();
}

bool Engine::DuplicateRuntimeActorByUID(Actor::UID actor_uid,
                                        Actor::UID *out_new_actor_uid) {
    WorldScope world_scope(world_);
    if (scene_reset_required_) return false;
    last_runtime_actor_error_.clear();
    const Actor *source_actor = GetRuntimeActorByUID(actor_uid);
    if (source_actor == nullptr) return false;
    std::unique_ptr<PreparedRuntimeActor> prepared;
    try {
        Actor actor = *source_actor;
        actor.actor_name = BuildUniqueRuntimeActorName(actors,
            source_actor->actor_name.empty() ? "New Actor" : source_actor->actor_name + " (copy)");
        actor.uid = AllocateRuntimeGeneratedActorUID();
        actor.runtime_generation = 0;
        actor.scene_backed = false;
        actor.parent_uid = Actor::kInvalidUID;
        actor.runtime_destroyed = false;
        prepared = PrepareRuntimeActor(std::move(actor), last_runtime_actor_error_);
        if (!prepared) return false;
        Actor *published = PublishRuntimeActor(*prepared, actors, false);
        NotifyRuntimeActorPublished(*published);
        if (out_new_actor_uid) *out_new_actor_uid = published->uid;
        return true;
    } catch (const std::exception &failure) {
        if (prepared && prepared->CommitStarted()) ReportRuntimeActorPublicationFailure(failure.what());
        else last_runtime_actor_error_ = ScriptReloadCandidate::SanitizeDiagnosticText(failure.what());
        Log::Warning("actor", "duplication rejected: " + last_runtime_actor_error_);
        return false;
    }
}
/*
Remove one runtime actor and immediately flush the component-system destruction
queue so editor views do not keep dangling selections or stale hierarchy data.
*/
bool Engine::DeleteRuntimeActorByUID(Actor::UID actor_uid) {
    WorldScope world_scope(world_);
    for (Actor &actor : actors) {
        if (actor.runtime_destroyed) continue;
        if (actor.uid != actor_uid) continue;

        ComponentManager::DestroyActor(&actor);
        ComponentManager::FinalizeFrameMutations();
        PruneDestroyedRuntimeActors();
        return true;
    }
    return false;
}

/*
Runtime-only parenting mirrors the same preserve-world semantics as the scene
document path, but it operates directly on live actors and component state.
This is the backend used by the Lua SetParent API.
*/
bool Engine::SetRuntimeActorParentByUID(Actor::UID actor_uid,
                                        Actor::UID parent_uid) {
    WorldScope world_scope(world_);
    /*
    Runtime-generated actors are appended directly by the scripting layer, so
    the UID cache may lag behind until we explicitly resync it here.
    */
    rebuildRuntimeActorUIDMap();

    const Actor *actor = GetRuntimeActorByUID(actor_uid);
    if (actor == nullptr) return false;
    if (actor->uid == Actor::kInvalidUID) return false;

    SceneFormat::SetActorParentMutation mutation;
    mutation.actor_uid = actor->uid;

    if (parent_uid != Actor::kInvalidUID) {
        const Actor *parent_actor = GetRuntimeActorByUID(parent_uid);
        if (parent_actor == nullptr) return false;
        if (parent_actor->uid == Actor::kInvalidUID) {
            return false;
        }
        mutation.parent_uid = parent_actor->uid;
    }

    return ApplySceneEditCommand(SceneFormat::SceneEditCommand::Single({mutation}));
}

/*
Reload the runtime from a shared scene asset snapshot. (instead of re-reading from disk)

Notice: Play/Stop transitions can leave transient input/audio state from the
previous runtime snapshot, so clear those caches before rebind.
*/
void Engine::failSceneCommit(const std::string &message) {
    last_scene_edit_error_.clear();
    last_runtime_actor_error_.clear();
    // No stale Actor* may survive a failed destructive commit. Partial new state
    // remains owned for shutdown, but must not resume gameplay or rendering.
    runtime_actor_by_uid_.clear();
    runtime_physics_hierarchy_state_by_uid_.clear();
    runtime_physics_hierarchy_cache_dirty_=true;
    scene_reset_required_=true;is_game_running=false;
    has_pending_scene_load=false;pending_scene_name.clear();
    last_scene_load_error_=ScriptReloadCandidate::SanitizeDiagnosticText("scene commit failed; reset required: "+message);
}

bool Engine::LoadSceneAsset(const SceneFormat::SceneAsset &scene_asset,
                           const std::function<bool()> &before_commit) {
    WorldScope world_scope(world_);
    ++scene_load_attempt_revision_;
    if(scene_transition_active_){last_scene_load_error_="another scene transaction is already active";return false;}
    SceneTransitionScope transition(scene_transition_active_);
    if(scene_reset_required_){last_scene_load_error_="runtime requires reset after failed publication";return false;}
    PreparedScene prepared;std::string error;
    if(!PrepareScene(scene_asset,actors,false,prepared,error)){
        last_scene_load_error_=error;return false;
    }
    try {
        if(before_commit&&!before_commit()){
            last_scene_load_error_="scene pre-commit host action failed; current world preserved";return false;
        }
    }catch(const std::exception &failure){last_scene_load_error_=ScriptReloadCandidate::SanitizeDiagnosticText(failure.what());return false;}
    try {
        ::Input::ResetForSceneChange();AudioManager::StopAllPlayback();
        for(const auto event:{SDL_EVENT_KEY_DOWN,SDL_EVENT_KEY_UP,SDL_EVENT_TEXT_EDITING,SDL_EVENT_TEXT_INPUT,
                              SDL_EVENT_MOUSE_MOTION,SDL_EVENT_MOUSE_BUTTON_DOWN,SDL_EVENT_MOUSE_BUTTON_UP,SDL_EVENT_MOUSE_WHEEL})
            SDL_FlushEvent(event);
        const auto errors_before_teardown=ComponentManager::GetRuntimeErrorCount();
        ComponentManager::ClearActorComponents();
        if(ComponentManager::GetRuntimeErrorCount()!=errors_before_teardown)
            throw std::runtime_error("scene teardown callback failed: "+ComponentManager::GetLastRuntimeError());
        runtime_actor_by_uid_.clear();invalidateRuntimePhysicsHierarchyCache();
        actors.clear();
        ComponentManager::CommitSceneScripts(*prepared.scripts);
        Scene::SetActiveSceneSubdirectory(scene_asset.scene_subdirectory);
        actors.splice(actors.end(),prepared.nodes);
        ComponentManager::BindActorsForScene(actors);
        InstallSceneComponents(*prepared.components,actors,0);
        ComponentManager::BindActorsForScene(actors);rebuildRuntimeActorUIDMap();
        current_scene_name=scene_asset.scene_name;
        has_pending_scene_load=false;pending_scene_name.clear();last_scene_load_error_.clear();last_runtime_actor_error_.clear();last_scene_edit_error_.clear();
        return true;
    }catch(const std::exception &failure){failSceneCommit(failure.what());return false;}
}

/*
load a scene edit command from the editor and apply it to the live runtime world.
this is API for the editor to manipulate the runtime world (atomically), 
can be used for hot reloading.
(兼容 hot reloading, 把一个 editor-side 的 scene edit command 加载到 runtime)

It applys a batch of scene mutations,
if any single mutation fails to apply then the whole command is rejected and rolled back.
(apply 一系列的 scene mutations, 如果其中一个无法 apply 就拒绝整个 command 并 rollback)
*/
/* Toggle offscreen runtime rendering for the docked editor viewport. */
void Engine::SetRenderRuntimeToTexture(bool enabled) {
    WorldScope world_scope(world_);
    render_runtime_to_texture_ = enabled;
    if (!enabled) {
        destroyRuntimeRenderTarget();
        return;
    }
    ensureRuntimeRenderTarget();
}

void Engine::SetRuntimeRenderTargetSize(int width, int height) {
    WorldScope world_scope(world_);
    const int safe_width = std::max(1, width);
    const int safe_height = std::max(1, height);
    if (requested_runtime_render_target_width_ == safe_width &&
        requested_runtime_render_target_height_ == safe_height) {
        return;
    }

    requested_runtime_render_target_width_ = safe_width;
    requested_runtime_render_target_height_ = safe_height;
}

/* Return the latest runtime render target for ImGui::Image(). */
SDL_Texture *Engine::GetRuntimeRenderTarget() const {return runtime_render_target_;}
int Engine::GetRuntimeRenderTargetWidth() const {return runtime_render_target_width_;}
int Engine::GetRuntimeRenderTargetHeight() const {return runtime_render_target_height_;}
SDL_Texture *Engine::RenderScenePreview(float camera_x, float camera_y,
                                        float zoom_factor, int preview_width,
                                        int preview_height,
                                        bool scene_backed_only) {
    WorldScope world_scope(world_);
    if (renderer == nullptr) return nullptr;

    ensureScenePreviewRenderTarget(preview_width, preview_height);
    if (scene_preview_render_target_ == nullptr) return nullptr;

    SDL_SetRenderTarget(renderer, scene_preview_render_target_);
    SDL_SetRenderViewport(renderer, nullptr);
    clearFrame();

    /*
    Rebuild draw requests for an editor-only scene preview pass. This keeps the
    scene camera independent without disturbing the live runtime view.
    */
    ComponentManager::ResolveTransformHierarchy();
    ComponentManager::QueueBuiltinRenderers(scene_backed_only);
    ParticleManager::QueueRenderBatches();

    const float clamped_zoom = std::clamp(zoom_factor, kMinZoomFactor, kMaxZoomFactor);
    if (scene_backed_only) ComponentManager::CommitTilemaps();
    const float half_width = scene_preview_render_target_width_ / (200.0f * clamped_zoom);
    const float half_height = scene_preview_render_target_height_ / (200.0f * clamped_zoom);
    ComponentManager::QueueTilemaps(camera_x - half_width, camera_y - half_height,
                                    camera_x + half_width, camera_y + half_height,
                                    scene_backed_only);
    Renderer::RenderFrame(renderer, camera_x, camera_y, clamped_zoom,
                          scene_preview_render_target_width_,
                          scene_preview_render_target_height_);

    SDL_SetRenderTarget(renderer, nullptr);
    SDL_SetRenderViewport(renderer, nullptr);
    return scene_preview_render_target_;
}
int Engine::GetScenePreviewRenderTargetWidth() const {
    return scene_preview_render_target_width_;
}
int Engine::GetScenePreviewRenderTargetHeight() const {
    return scene_preview_render_target_height_;
}
float Engine::GetRuntimeRenderFPS() const {return displayed_runtime_render_fps_;}
float Engine::GetGameplayFPS() const {return displayed_gameplay_fps_;}
void Engine::SetAudioPlaybackEnabled(bool enabled) {
    WorldScope world_scope(world_);
    AudioManager::SetPlaybackEnabled(enabled);
}
bool Engine::IsAudioPlaybackEnabled() const {
    WorldScope world_scope(world_);
    return AudioManager::IsPlaybackEnabled();
}


/*
SDL objects are created lazily so the same Engine can exist before the
runtime window/renderer are needed by either the game or the editor host.
*/
void Engine::initialize() {
    FrameClock::Reset();
    SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "1");
    // POCKET_WINDOW_BACKGROUND=1 shows the window without activating the app,
    // so automated runs never take keyboard focus from other work.
    const bool background = ReadEnvironmentString("POCKET_WINDOW_BACKGROUND") == "1";
    if (background) {
        SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw std::runtime_error(std::string("SDL video initialization failed: ") + SDL_GetError());
    }
    sdl_subsystems_ |= SDL_INIT_VIDEO;

    window = SDLRenderHelper::SDL_CreateWindow(
        config_.game_title.c_str(), config_.window_width, config_.window_height,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (window == nullptr) {
        throw std::runtime_error(std::string("window creation failed: ") + SDL_GetError());
    }
    const SDL_DisplayID display = ChooseWindowDisplay();
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_ShowWindow(window);
    if (!background) SDL_RaiseWindow(window);
    renderer = CreateConfiguredRenderer(window);

    Renderer::Init();
    ::Input::Init();
    ::Input::SetExternalInputEnabled(false);

    runtime_zoom_factor = (config_.zoom_factor > 0.0f)
                              ? std::clamp(config_.zoom_factor, kMinZoomFactor,
                                           kMaxZoomFactor)
                              : 1.0f;

    clearFrame();
    ResetPerformanceCounters();
}

void Engine::update() {
    POCKET_ZONE("Engine::update");
    /* Gameplay update is component-driven and always runs in this order. */
    ComponentManager::ProcessSpriteAnimations();
    if (scene_reset_required_) return;
    ComponentManager::ProcessOnUpdate();
    if (scene_reset_required_) return;
    ComponentManager::ProcessOnLateUpdate();
    if (scene_reset_required_) return;

    /*
    Only pre-physics destruction/removal work is flushed here. Other
    end-of-frame mutations still wait until after the Box2D step.
    */
    ComponentManager::FinalizePrePhysicsDestructions();
    if (scene_reset_required_) return;
    ComponentManager::CommitTilemaps();
    if (scene_reset_required_) return;
    ComponentManager::StepPhysics();
    if (scene_reset_required_) return;
    ParticleManager::Update(1.0f / 60.0f);

    /* Full end-of-frame reconciliation happens once, after physics/callbacks. */
    ComponentManager::FinalizeFrameMutations();
    if (scene_reset_required_) return;
    PruneDestroyedRuntimeActors();
}


/*
Scene changes requested from gameplay are deferred to the next frame
boundary so actor destruction/creation never races the current update.
(gameplay 请求的场景切换被延迟到下一帧边界, 这样 actor 销毁/创建就永远不会和当前 update 竞争)
*/
void Engine::processPendingSceneLoad() {
    POCKET_ZONE("Engine::processPendingSceneLoad");
    if(!has_pending_scene_load||scene_reset_required_||scene_transition_active_)return;
    SceneTransitionScope transition(scene_transition_active_);
    const auto requested=pending_scene_name;has_pending_scene_load=false;pending_scene_name.clear();
    ++scene_load_attempt_revision_;
    PreparedScene prepared;SceneFormat::SceneAsset asset;std::string error;
    try { asset=SceneFormat::LoadSceneAsset(requested,Scene::GetActiveSceneSubdirectory()); }
    catch(const std::exception &failure){last_scene_load_error_=ScriptReloadCandidate::SanitizeDiagnosticText(failure.what());return;}
    if(!PrepareScene(asset,actors,true,prepared,error)){last_scene_load_error_=error;return;}
    try {
        const auto errors_before_teardown=ComponentManager::GetRuntimeErrorCount();
        for(auto &actor:actors)if(!actor.runtime_destroyed&&!actor.dont_destroy_on_scene_load)
            ComponentManager::DestroyActor(&actor);
        ComponentManager::FinalizeFrameMutations();
        if(ComponentManager::GetRuntimeErrorCount()!=errors_before_teardown)
            throw std::runtime_error("scene teardown callback failed: "+ComponentManager::GetLastRuntimeError());
        PruneDestroyedRuntimeActors();
        // User destruction callbacks have run after the commit boundary. If they
        // introduce an identity conflict, classify it as fatal rather than bind
        // a persistent component to an unrelated actor.
        std::unordered_set<Actor::UID> surviving;
        for(const auto &actor:actors)if(!actor.runtime_destroyed)surviving.insert(actor.uid);
        for(const auto &actor:prepared.nodes)if(surviving.contains(actor.uid))
            throw std::runtime_error("destruction callback introduced an incoming actor UID conflict");
        ComponentManager::CommitSceneScripts(*prepared.scripts);
        Scene::SetActiveSceneSubdirectory(asset.scene_subdirectory);
        const auto first=actors.size();actors.splice(actors.end(),prepared.nodes);
        ComponentManager::BindActorsForScene(actors);
        InstallSceneComponents(*prepared.components,actors,first);
        ComponentManager::BindActorsForScene(actors);rebuildRuntimeActorUIDMap();
        current_scene_name=asset.scene_name;last_scene_load_error_.clear();
        if(ShouldLogSceneDiagnostics())Log::Info("scene","loaded "+current_scene_name+" live_actors="+std::to_string(CountLiveActors())+" stored_actors="+std::to_string(actors.size()));
    }catch(const std::exception &failure){failSceneCommit(failure.what());}
}

void Engine::SetCameraPosition(float x, float y) {camera_position.x = x; camera_position.y = y;}
float Engine::GetCameraPositionX() const {return camera_position.x;}
float Engine::GetCameraPositionY() const {return camera_position.y;}
void Engine::SetCameraZoom(float zoom_factor) {runtime_zoom_factor = zoom_factor;}
float Engine::GetCameraZoom() const {return runtime_zoom_factor;}

namespace {
// Window logical size, or the configured size without a window (headless).
std::pair<float, float> ScreenExtent(SDL_Window *window, float fallback_width, float fallback_height) {
    int width = 0, height = 0;
    if (window != nullptr && SDL_GetWindowSize(window, &width, &height) && width > 0 && height > 0)
        return {static_cast<float>(width), static_cast<float>(height)};
    return {fallback_width, fallback_height};
}
} // namespace

Vector2 Engine::GetScreenSize() const {
    return {static_cast<float>(std::max(1, config_.window_width)), static_cast<float>(std::max(1, config_.window_height))};
}
Vector2 Engine::GetViewSize() const {
    const float zoom = std::clamp(runtime_zoom_factor, kMinZoomFactor, kMaxZoomFactor);
    const Vector2 screen = GetScreenSize();
    return {screen.x / (100.0f * zoom), screen.y / (100.0f * zoom)};
}
Vector2 Engine::ScreenToWorld(float screen_x, float screen_y) const {
    const Vector2 render = GetScreenSize();
    const auto [width, height] = ScreenExtent(window, render.x, render.y);
    const float zoom = std::clamp(runtime_zoom_factor, kMinZoomFactor, kMaxZoomFactor);
    const float rx = screen_x * render.x / width, ry = screen_y * render.y / height;
    return {camera_position.x + (rx - render.x * 0.5f) / (100.0f * zoom),
            camera_position.y + (ry - render.y * 0.5f) / (100.0f * zoom)};
}
Vector2 Engine::WorldToScreen(float world_x, float world_y) const {
    const Vector2 render = GetScreenSize();
    const auto [width, height] = ScreenExtent(window, render.x, render.y);
    const float zoom = std::clamp(runtime_zoom_factor, kMinZoomFactor, kMaxZoomFactor);
    const float rx = (world_x - camera_position.x) * 100.0f * zoom + render.x * 0.5f;
    const float ry = (world_y - camera_position.y) * 100.0f * zoom + render.y * 0.5f;
    return {rx * width / render.x, ry * height / render.y};
}


void Engine::RequestSceneLoad(const std::string &scene_name) {
    has_pending_scene_load = true;
    pending_scene_name = scene_name;
}
std::string Engine::GetCurrentSceneName() const {return current_scene_name;}

void Engine::ResetPerformanceCounters() {
    performance_sample_start_ticks_ = SDL_GetTicks();
    performance_runtime_render_frames_ = 0;
    performance_gameplay_frames_ = 0;
    displayed_runtime_render_fps_ = 0.0f;
    displayed_gameplay_fps_ = 0.0f;
}

void Engine::RecordRuntimeRenderFrame() {
    ++performance_runtime_render_frames_;
}

void Engine::RecordGameplayFrame() {
    ++performance_gameplay_frames_;
}

void Engine::RefreshPerformanceCounters() {
    if (performance_sample_start_ticks_ == 0) {
        performance_sample_start_ticks_ = SDL_GetTicks();
        return;
    }

    const std::uint32_t now = SDL_GetTicks();
    const std::uint32_t elapsed_ticks = now - performance_sample_start_ticks_;
    if (elapsed_ticks < 1000) return;

    const float elapsed_seconds = static_cast<float>(elapsed_ticks) / 1000.0f;
    displayed_runtime_render_fps_ =
        static_cast<float>(performance_runtime_render_frames_) /
        elapsed_seconds;
    displayed_gameplay_fps_ =
        static_cast<float>(performance_gameplay_frames_) / elapsed_seconds;

    if (ShouldLogPerformanceDiagnostics()) {
        Log::Info("perf", "scene=" + current_scene_name +
                              " gameplay_fps=" + std::to_string(displayed_gameplay_fps_) +
                              " render_fps=" + std::to_string(displayed_runtime_render_fps_) +
                              " live_actors=" + std::to_string(CountLiveActors()) +
                              " stored_actors=" + std::to_string(actors.size()) +
                              " audio_failures=" + std::to_string(AudioManager::GetAudioPlayFailureCount()) +
                              " music_failures=" + std::to_string(AudioManager::GetMusicPlayFailureCount()));
    }

    performance_sample_start_ticks_ = now;
    performance_runtime_render_frames_ = 0;
    performance_gameplay_frames_ = 0;
}

/*
This flag is consumed by processPendingSceneLoad() to preserve actors
across runtime-initiated scene switches.
*/
void Engine::MarkActorDontDestroy(Actor *actor) {
    if (actor == nullptr) return;
    actor->dont_destroy_on_scene_load = true;
}

void Engine::invalidateRuntimePhysicsHierarchyCache() {
    runtime_physics_hierarchy_cache_dirty_ = true;
}

/*
Refresh the derived rigidbody hierarchy state used by inspector/status panels
and by runtime effective-body-type resolution. The cache only depends on
parent links plus live Rigidbody requests, so rebuilding it on demand keeps the
mutation paths simpler than trying to eagerly propagate every dirty subtree.
*/
void Engine::rebuildRuntimePhysicsHierarchyCache() const {
    if (!runtime_physics_hierarchy_cache_dirty_) return;

    runtime_physics_hierarchy_state_by_uid_.clear();
    runtime_physics_hierarchy_state_by_uid_.reserve(actors.size());

    std::unordered_map<Actor::UID, const Actor *> runtime_actor_by_uid;
    runtime_actor_by_uid.reserve(actors.size());
    for (const Actor &actor : actors) {
        if (actor.runtime_destroyed) continue;
        runtime_actor_by_uid[actor.uid] = &actor;
    }

    std::unordered_map<Actor::UID, PhysicsHierarchy::State> state_by_actor_uid;
    state_by_actor_uid.reserve(actors.size());
    std::unordered_set<Actor::UID> resolved_actor_uids;
    std::unordered_set<Actor::UID> resolving_actor_uids;

    std::function<void(const Actor &)> resolve_actor_state =
        [&](const Actor &actor) {
            if (actor.runtime_destroyed) return;
            if (resolved_actor_uids.find(actor.uid) != resolved_actor_uids.end()) {
                return;
            }
            if (!resolving_actor_uids.insert(actor.uid).second) {
                return;
            }

            /*
            First read the actor's own Rigidbody request from live component
            state, then layer ancestry-derived ownership rules on top.
            */
            PhysicsHierarchy::State state = BuildRuntimeActorPhysicsSelfState(
                actor.uid, ComponentManager::GetRuntimeComponentSpecs(actor.uid));

            std::uint64_t nearest_dynamic_ancestor_uid =
                PhysicsHierarchy::kInvalidActorUID;
            if (actor.parent_uid != Actor::kInvalidUID &&
                actor.parent_uid != actor.uid) {
                auto parent_it = runtime_actor_by_uid.find(actor.parent_uid);
                if (parent_it != runtime_actor_by_uid.end() &&
                    parent_it->second != nullptr) {
                    /*
                    Resolve parents first so each child can inherit nearest
                    dynamic ancestor / physics-root state in one pass.
                    */
                    resolve_actor_state(*parent_it->second);
                    const PhysicsHierarchy::State &parent_state =
                        state_by_actor_uid[parent_it->second->uid];
                    if (parent_state.has_dynamic_rigidbody_self) {
                        nearest_dynamic_ancestor_uid =
                            parent_it->second->uid;
                    } else {
                        nearest_dynamic_ancestor_uid =
                            parent_state.nearest_dynamic_body_ancestor_uid;
                    }
                }
            }

            state.nearest_dynamic_body_ancestor_uid = nearest_dynamic_ancestor_uid;
            state.is_under_dynamic_hierarchy =
                nearest_dynamic_ancestor_uid != PhysicsHierarchy::kInvalidActorUID;
            if (state.has_dynamic_rigidbody_self) {
                state.physics_root_uid = actor.uid;
            } else if (state.is_under_dynamic_hierarchy) {
                state.physics_root_uid = nearest_dynamic_ancestor_uid;
            }
            if (state.rigidbody_enabled_self &&
                state.requested_body_type == "dynamic" &&
                state.is_under_dynamic_hierarchy) {
                /*
                The authored component still requests dynamic, but runtime
                consumes the derived kinematic override so a subtree has only
                one dynamic owner.
                */
                state.effective_body_type = "kinematic";
            }

            state_by_actor_uid[actor.uid] = state;
            if (actor.uid != PhysicsHierarchy::kInvalidActorUID) {
                runtime_physics_hierarchy_state_by_uid_[actor.uid] = state;
            }

            resolving_actor_uids.erase(actor.uid);
            resolved_actor_uids.insert(actor.uid);
        };

    for (const Actor &actor : actors) {
        resolve_actor_state(actor);
    }

    runtime_physics_hierarchy_cache_dirty_ = false;
}

/*
Editor-authored actor UIDs stay stable across runtime rebuilds. This map lets
live edit commands, runtime reparenting, and scene-backed selection mapping
find the current live actor after reloads or component-system mutations.
*/
void Engine::rebuildRuntimeActorUIDMap() {
    invalidateRuntimePhysicsHierarchyCache();
    runtime_actor_by_uid_.clear();
    runtime_actor_by_uid_.reserve(actors.size());
    std::uint64_t max_existing_uid = 0;
    for (Actor &actor : actors) {
        if (actor.runtime_destroyed) continue;
        if (actor.uid == SceneFormat::kInvalidSceneActorUID) continue;
        max_existing_uid = std::max(max_existing_uid, actor.uid);
        runtime_actor_by_uid_[actor.uid] = &actor;
    }
    /*
    Runtime-only actors allocate from a monotonic cursor after the highest live
    UID so instantiate/duplicate stay O(1) during play mode.
    */
    // Do not wrap max UID to zero or rewind over UIDs allocated earlier in
    // this world. The allocator skips authored IDs when the cursor wraps.
    next_runtime_generated_actor_uid_ =
        std::max(next_runtime_generated_actor_uid_, Actor::kRuntimeGeneratedUIDStart);
    if (max_existing_uid != std::numeric_limits<Actor::UID>::max())
        next_runtime_generated_actor_uid_ =
            std::max(next_runtime_generated_actor_uid_, max_existing_uid + 1);
}

void Engine::PruneDestroyedRuntimeActors() {
    if (CountLiveActors() == actors.size()) return;

    // Lua userdata and Box2D fixture metadata hold Actor pointers. Erase only
    // dead nodes so live actors retain their addresses across arbitrary churn.
    actors.remove_if([](const Actor &actor) { return actor.runtime_destroyed; });
    ComponentManager::BindActorsForScene(actors);
    rebuildRuntimeActorUIDMap();
}

Actor* Engine::findRuntimeActorByUID(Actor::UID actor_uid) {
    auto actor_it = runtime_actor_by_uid_.find(actor_uid);
    if (actor_it == runtime_actor_by_uid_.end()) return nullptr;
    if (actor_it->second == nullptr || actor_it->second->runtime_destroyed) {
        return nullptr;
    }
    return actor_it->second;
}
const Actor* Engine::findRuntimeActorByUID(Actor::UID actor_uid) const {
    auto actor_it = runtime_actor_by_uid_.find(actor_uid);
    if (actor_it == runtime_actor_by_uid_.end()) return nullptr;
    if (actor_it->second == nullptr || actor_it->second->runtime_destroyed) {
        return nullptr;
    }
    return actor_it->second;
}

/*
The editor-owned embedded viewport is backed by this SDL texture. Destroy
it whenever embedding is disabled or the renderer/runtime is shutting down.
*/
void Engine::destroyRuntimeRenderTarget() {
    if (runtime_render_target_ != nullptr) {
        SDL_DestroyTexture(runtime_render_target_);
        runtime_render_target_ = nullptr;
    }
    runtime_render_target_width_ = 0;
    runtime_render_target_height_ = 0;
}

/*
Allocate or resize the offscreen texture that receives runtime rendering
before ImGui displays it inside the editor viewport.
*/
void Engine::ensureRuntimeRenderTarget() {
    if (!render_runtime_to_texture_) return;
    if (renderer == nullptr) return;

    const int desired_width =
        std::max(1, requested_runtime_render_target_width_ > 0
                        ? requested_runtime_render_target_width_
                        : config_.window_width);
    const int desired_height =
        std::max(1, requested_runtime_render_target_height_ > 0
                        ? requested_runtime_render_target_height_
                        : config_.window_height);

    if (runtime_render_target_ != nullptr &&
        runtime_render_target_width_ == desired_width &&
        runtime_render_target_height_ == desired_height) {
        return;
    }

    destroyRuntimeRenderTarget();
    runtime_render_target_ = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
        desired_width, desired_height);
    if (runtime_render_target_ == nullptr) {
        render_runtime_to_texture_ = false;
        return;
    }

    SDL_SetTextureBlendMode(runtime_render_target_, SDL_BLENDMODE_BLEND);
    runtime_render_target_width_ = desired_width;
    runtime_render_target_height_ = desired_height;
}

/*
Scene panel owns a separate preview texture so its editor camera can diverge
from the live runtime camera without stretching/cropping the game viewport.
*/
void Engine::destroyScenePreviewRenderTarget() {
    if (scene_preview_render_target_ != nullptr) {
        SDL_DestroyTexture(scene_preview_render_target_);
        scene_preview_render_target_ = nullptr;
    }
    scene_preview_render_target_width_ = 0;
    scene_preview_render_target_height_ = 0;
}

void Engine::ensureScenePreviewRenderTarget(int width, int height) {
    if (renderer == nullptr) return;
    const int safe_width = std::max(1, width);
    const int safe_height = std::max(1, height);

    if (scene_preview_render_target_ != nullptr &&
        scene_preview_render_target_width_ == safe_width &&
        scene_preview_render_target_height_ == safe_height) {
        return;
    }

    destroyScenePreviewRenderTarget();
    scene_preview_render_target_ = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
        safe_width, safe_height);
    if (scene_preview_render_target_ == nullptr) {
        return;
    }

    SDL_SetTextureBlendMode(scene_preview_render_target_, SDL_BLENDMODE_BLEND);
    scene_preview_render_target_width_ = safe_width;
    scene_preview_render_target_height_ = safe_height;
}
