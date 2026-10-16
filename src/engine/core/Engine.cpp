#include "core/Engine.h"
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
        std::cout << "renderer: driver=" << SDL_GetRendererName(renderer)
                  << " vsync=" << interval << std::endl;
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
        GameSave::Configure(config_.save_namespace);
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

    while (IsRunning()) {
        RunSingleFrame();
        PresentFrame();
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

    destroyRuntimeRenderTarget();
    destroyScenePreviewRenderTarget();
    runtime_actor_by_uid_.clear();
    ComponentManager::ClearPreparedTilemapAssets();
    ComponentManager::ClearPreparedSpriteAnimationAssets();
    ComponentManager::Shutdown();
    Scene::SetActiveSceneSubdirectory({});
    ParticleManager::Clear();
    Renderer::Shutdown();
    AudioManager::Shutdown();
    if (renderer != nullptr) {
        SDL_DestroyRenderer(renderer);
        renderer = nullptr;
    }
    if (window != nullptr) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }
    SDL_Quit();
    SDLEventHelper::Reset();
    SDLRenderHelper::Reset();
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
    RecordGameplayFrame();
    ::Input::LateUpdate();
    FrameClock::AdvanceFrame();
}

void Engine::StepSimulation() {
    if (!IsRunning() || !runtime_initialized_) return;
    advanceGameplay();
    if(scene_reset_required_)return;
    // The visible path resolves final transforms in render(). Headless observers
    // need the same post-mutation world values even in scenes without physics.
    ComponentManager::ResolveTransformHierarchy();
    Renderer::DiscardFrameRequests();
    finishGameplayFrame();
}



/*
renders a frozen preview frame:
no gameplay update (component updates, physics step, or any frame advance).
Edit mode stays static by default; live preview may explicitly flush OnStart.
*/
void Engine::RunRenderFrozenFrame(bool quit_requested_this_frame,
                                  bool process_pending_on_start) {
    WorldScope world_scope(world_);
    
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
    if (event.type == SDL_EVENT_QUIT) {
        quit_requested_this_frame = true;
    }
}

/* Present the current backbuffer to the window once drawing is complete. */
void Engine::PresentFrame(bool advance_gameplay_frame) {
    WorldScope world_scope(world_);
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
        std::cerr << "actor duplication rejected: " << last_runtime_actor_error_ << '\n';
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
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw std::runtime_error(std::string("SDL video initialization failed: ") + SDL_GetError());
    }

    window = SDLRenderHelper::SDL_CreateWindow(
        config_.game_title.c_str(), config_.window_width, config_.window_height,
        SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window == nullptr) {
        throw std::runtime_error(std::string("window creation failed: ") + SDL_GetError());
    }
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_RaiseWindow(window);
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
        if(ShouldLogSceneDiagnostics())std::cout<<"scene_load: "<<current_scene_name<<" live_actors="<<CountLiveActors()<<" stored_actors="<<actors.size()<<std::endl;
    }catch(const std::exception &failure){failSceneCommit(failure.what());}
}

void Engine::SetCameraPosition(float x, float y) {camera_position.x = x; camera_position.y = y;}
float Engine::GetCameraPositionX() const {return camera_position.x;}
float Engine::GetCameraPositionY() const {return camera_position.y;}
void Engine::SetCameraZoom(float zoom_factor) {runtime_zoom_factor = zoom_factor;}
float Engine::GetCameraZoom() const {return runtime_zoom_factor;}


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
        std::cout << "perf: scene=" << current_scene_name
                  << " gameplay_fps=" << displayed_gameplay_fps_
                  << " render_fps=" << displayed_runtime_render_fps_
                  << " live_actors=" << CountLiveActors()
                  << " stored_actors=" << actors.size()
                  << " audio_failures=" << AudioManager::GetAudioPlayFailureCount()
                  << " music_failures=" << AudioManager::GetMusicPlayFailureCount()
                  << std::endl;
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
