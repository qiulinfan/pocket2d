#ifndef ENGINE_H
#define ENGINE_H

#include "core/World.h"
#include "scene/Actor.h"
#include "shared/physics/PhysicsHierarchyState.h"
#include "shared/config/GameConfig.h"
#include "shared/scene_format/SceneFormat.h"
#include "shared/scene_format/SceneMutation.h"
#include "glm/glm.hpp"
#include <cstddef>
#include <cstdint>
#include <list>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Surface;
union SDL_Event;

class Engine
{
private:
    // The World holding this runtime's mutable subsystem state. Public entry
    // points bind it, so several Engines may coexist on one or more threads.
    World &world_;
    GameConfigData config_;

    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    // SDL subsystems this Engine initialized and must release (SDL_InitFlags).
    std::uint32_t sdl_subsystems_ = 0;
    // Set by InitializeEmbedded: window and renderer belong to another Engine.
    bool borrows_presenter_ = false;
    // Headless offscreen rendering: a software renderer drawing into this surface.
    SDL_Surface *offscreen_surface_ = nullptr;

    // runtime actor container for the current scene
    std::list<Actor> actors;

    bool is_game_running = true;
    bool runtime_initialized_ = false;
    bool runtime_shutdown_ = false;
    std::string current_scene_name = "";

    // queued scene load request, applied at the start of the next frame
    // 延迟到下一帧开头执行的场景切换请求
    bool has_pending_scene_load = false;
    std::string pending_scene_name = "";
    std::string last_scene_load_error_;
    std::string last_runtime_actor_error_;
    std::string last_scene_edit_error_;
    std::uint64_t scene_load_attempt_revision_ = 0;
    bool scene_reset_required_ = false;
    bool scene_transition_active_ = false;

    // runtime camera state
    float runtime_zoom_factor = 1.0f;
    static constexpr float kMinZoomFactor = 0.1f;
    static constexpr float kMaxZoomFactor = 8.0f;
    glm::vec2 camera_position = glm::vec2(0.0f, 0.0f);

    // When the editor is active, runtime rendering is redirected into this
    // texture so Dear ImGui can present the game inside a docked viewport.
    bool render_runtime_to_texture_ = false;
    SDL_Texture *runtime_render_target_ = nullptr;
    int runtime_render_target_width_ = 0;
    int runtime_render_target_height_ = 0;
    int requested_runtime_render_target_width_ = 0;
    int requested_runtime_render_target_height_ = 0;
    SDL_Texture *scene_preview_render_target_ = nullptr;
    int scene_preview_render_target_width_ = 0;
    int scene_preview_render_target_height_ = 0;
    Actor::UID next_runtime_generated_actor_uid_ =
        Actor::kRuntimeGeneratedUIDStart;
    std::unordered_map<Actor::UID, Actor *> runtime_actor_by_uid_;
    mutable bool runtime_physics_hierarchy_cache_dirty_ = true;
    mutable std::unordered_map<std::uint64_t, PhysicsHierarchy::State>
        runtime_physics_hierarchy_state_by_uid_;
    // Sampled runtime counters let the editor compare UI cadence against
    // actual live runtime cadence when diagnosing visible stutter.
    std::uint32_t performance_sample_start_ticks_ = 0;
    int performance_runtime_render_frames_ = 0;
    int performance_gameplay_frames_ = 0;
    float displayed_runtime_render_fps_ = 0.0f;
    float displayed_gameplay_fps_ = 0.0f;

    void update();
    void advanceGameplay();
    void finishGameplayFrame();
    void render();
    void initialize();
    void processPendingSceneLoad();
    void failSceneCommit(const std::string &message);
    void clearFrame();
    void clearEditorHostFrame();
    void destroyRuntimeRenderTarget();
    void ensureRuntimeRenderTarget();
    void destroyScenePreviewRenderTarget();
    void ensureScenePreviewRenderTarget(int width, int height);
    void invalidateRuntimePhysicsHierarchyCache();
    void rebuildRuntimePhysicsHierarchyCache() const;
    void rebuildRuntimeActorUIDMap();
    Actor *findRuntimeActorByUID(Actor::UID actor_uid);
    const Actor *findRuntimeActorByUID(Actor::UID actor_uid) const;
    void ResetPerformanceCounters();
    void RecordRuntimeRenderFrame();
    void RecordGameplayFrame();
    void RefreshPerformanceCounters();
    std::size_t CountLiveActors() const;
    void PruneDestroyedRuntimeActors();

public:
    // Uses the World current on this thread at construction.
    Engine();
    explicit Engine(World &world);
    ~Engine();
    World &GetWorld() const { return world_; }
    // Start SDL/runtime systems without entering the blocking game loop.
    // 初始化 SDL/运行时系统, 但不进入游戏循环.
    void InitializeRuntime();
    // Initialize simulation services without a window, renderer, or audio device.
    void InitializeHeadless();
    // Run inside another Engine's window and renderer, e.g. the editor's play
    // World. The host keeps ownership; shutdown leaves both intact.
    void InitializeEmbedded(SDL_Window *host_window, SDL_Renderer *host_renderer);
    // Advance the real gameplay pipeline by exactly one 1/60-second tick.
    // Input comes from ProcessSDLEvent; no SDL polling, rendering, or pacing.
    void StepSimulation();
    // Tear down SDL/runtime systems created by InitializeRuntime.
    // 关闭由 InitializeRuntime 创建的 SDL/运行时系统.
    void ShutdownRuntime();
    // Feed one SDL event into the runtime input and quit handling path.
    // 将一个 SDL 事件传递给运行时输入和退出处理路径.
    void ProcessSDLEvent(const SDL_Event &event,
                         bool &quit_requested_this_frame);
    // Poll events internally and execute one runtime frame.
    // 内部轮询事件并执行一个运行时帧.
    void RunSingleFrame();
    void RunSingleFrame(bool quit_requested_this_frame);
    // Execute one frozen preview frame for editor edit mode.
    // It renders without advancing gameplay simulation. Editor live preview may
    // opt into flushing pending OnStart work for components in the mirror.
    void RunRenderFrozenFrame(bool quit_requested_this_frame,
                              bool process_pending_on_start);
    // Keep showing the last runtime texture while paused, without re-rendering
    // or mutating runtime state.
    void RunPresentPausedFrame(bool quit_requested_this_frame);
    // Present the current backbuffer to the window once drawing is complete.
    // The legacy flag is retained for hosts; simulation now owns frame advance.
    void PresentFrame(bool advance_gameplay_frame = true);
    // Run the traditional standalone runtime loop until quit.
    // 运行传统的独立运行时循环直到退出.
    void GameLoop();
    bool IsRunning() const;
    // Ask the runtime loop to stop after the current frame.
    // 请求运行时循环在当前帧结束后停止.
    void RequestQuit();
    // Expose the SDL window, renderer for editor/platform integration.
    SDL_Window *GetWindow() const;
    SDL_Renderer *GetRenderer() const;
    // Return the current host window width in screen pixels.
    int GetWindowWidth() const;
    // Return the current host window height in screen pixels.
    int GetWindowHeight() const;
    // Return the loaded game configuration data.
    const GameConfigData &GetConfig() const;
    const std::string &GetLastSceneLoadError() const;
    const std::string &GetLastSceneEditError() const { return last_scene_edit_error_; }
    const std::string &GetLastRuntimeActorError() const { return last_runtime_actor_error_; }
    // Internal publication hooks; failure after commit requires a fresh runtime.
    void NotifyRuntimeActorPublished(Actor &actor);
    // Lua authoring flags affect hierarchy decisions immediately; Box2D changes
    // are applied by the lifecycle bridge at an unlocked simulation boundary.
    void NotifyRuntimePhysicsConfigurationChanged() { invalidateRuntimePhysicsHierarchyCache(); }
    void ReportRuntimeActorPublicationFailure(const std::string &message);
    std::uint64_t GetSceneLoadAttemptRevision() const { return scene_load_attempt_revision_; }
    bool RequiresSceneReset() const { return scene_reset_required_; }
    std::size_t GetActorCount() const;
    // Return read-only access to the current live runtime actor container.
    // Editors can inspect this during play mode without owning runtime state.
    const std::list<Actor> &GetRuntimeActors() const;
    // Look up one live runtime actor by runtime id / stable editor uid.
    const Actor *GetRuntimeActorByUID(Actor::UID actor_uid) const;
    PhysicsHierarchy::State GetRuntimePhysicsHierarchyStateByUID(Actor::UID actor_uid) const;
    // Duplicate/delete one live runtime actor without touching on-disk scene files.
    bool DuplicateRuntimeActorByUID(Actor::UID actor_uid,
                                    Actor::UID *out_new_actor_uid = nullptr);
    bool DeleteRuntimeActorByUID(Actor::UID actor_uid);
    bool SetRuntimeActorParentByUID(Actor::UID actor_uid,
                                    Actor::UID parent_uid);
    // Runtime-only actors receive transient stable UIDs from a dedicated
    // allocator so they never depend on SceneDocument's authoring counter.
    Actor::UID AllocateRuntimeGeneratedActorUID();
    // Reload the runtime from a shared scene asset snapshot.
    // 使用共享场景资源快照重新装载运行时.
    // Optional host gate runs after all preparation, before world mutation.
    // The editor uses it to save the outgoing draft only for a valid target.
    bool LoadSceneAsset(const SceneFormat::SceneAsset &scene_asset,
                        const std::function<bool()> &before_commit = {});
    bool ApplySceneEditCommand(const SceneFormat::SceneEditCommand &command);
    // Toggle offscreen runtime rendering for the docked editor viewport.
    // 切换用于编辑器嵌入视口的离屏运行时渲染.
    void SetRenderRuntimeToTexture(bool enabled);
    // Request the editor runtime target size for the next runtime render pass.
    void SetRuntimeRenderTargetSize(int width, int height);
    // Return the latest runtime render target for ImGui::Image().
    // 返回给 ImGui::Image() 使用的最新运行时渲染目标.
    SDL_Texture *GetRuntimeRenderTarget() const;
    // Return the physical pixel size of the embedded runtime render target.
    int GetRuntimeRenderTargetWidth() const;
    int GetRuntimeRenderTargetHeight() const;
    // Render one editor-only scene preview using an independent camera.
    // Headless engines only: draws the current world through the software
    // renderer into a width x height surface and returns an RGBA32 copy the
    // caller destroys, or nullptr with SDL_GetError(). Draw requests queued by
    // the last tick are preserved, so observing never changes the next frame.
    SDL_Surface *RenderOffscreen(int width, int height);
    SDL_Texture *RenderScenePreview(float camera_x, float camera_y,
                                    float zoom_factor, int preview_width,
                                    int preview_height,
                                    bool scene_backed_only = false);
    int GetScenePreviewRenderTargetWidth() const;
    int GetScenePreviewRenderTargetHeight() const;
    // Return sampled runtime cadence metrics for editor diagnostics.
    float GetRuntimeRenderFPS() const;
    float GetGameplayFPS() const;
    void SetAudioPlaybackEnabled(bool enabled);
    bool IsAudioPlaybackEnabled() const;

    // camera API exposed to Lua
    void SetCameraPosition(float x, float y);
    float GetCameraPositionX() const;
    float GetCameraPositionY() const;
    void SetCameraZoom(float zoom_factor);
    float GetCameraZoom() const;

    // scene API exposed to Lua
    // Queue a scene change to be applied at the start of the next frame.
    void RequestSceneLoad(const std::string &scene_name);
    std::string GetCurrentSceneName() const;
    void MarkActorDontDestroy(Actor *actor);
};

#endif
