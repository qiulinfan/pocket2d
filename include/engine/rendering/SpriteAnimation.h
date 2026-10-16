#ifndef POCKET2D_SPRITE_ANIMATION_H
#define POCKET2D_SPRITE_ANIMATION_H

#include "scene/ActorHandle.h"
#include "shared/animation/SpriteAnimationAsset.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class SpriteRenderer;

class SpriteAnimation {
public:
    struct Event {
        std::string type;
        std::string clip;
        std::string marker;
        int frame = 0;
        std::uint64_t playback_id = 0;
    };

    SpriteAnimation();
    // Copies authoring configuration, never a running component's ownership.
    SpriteAnimation(const SpriteAnimation &other);
    SpriteAnimation &operator=(const SpriteAnimation &) = delete;

    Actor *actor = nullptr;
    ActorHandle actor_handle;
    std::string key;
    bool enabled = true;
    std::string animation_resource;
    std::string renderer_key = "sprite";
    std::string clip;

    void Initialize(const SpriteAnimationAsset::Data &data);
    void Initialize(std::shared_ptr<const SpriteAnimationAsset::Data> data);
    // Commit-only transfer from a fresh detached prepared value. Preserves this
    // component's identity; consumes prepared. No resource I/O or asset validation.
    void AdoptPreparedState(SpriteAnimation &prepared);
    // Editor configuration reset; keeps this component live for later Initialize.
    void ClearAsset();
    bool Play(const std::string &name, bool restart = false);
    bool Restart(const std::string &name);
    void Pause();
    void Resume();
    void Stop();
    std::vector<Event> AdvanceTick();
    void ApplyPose();
    // Same as AdvanceTick/ApplyPose for a component whose caller has already
    // verified it is bound to its actor and resolved the renderer named by
    // renderer_key (nullptr when absent), skipping both lookups.
    std::vector<Event> AdvanceBoundTick(SpriteRenderer *target);
    void ApplyBoundPose(SpriteRenderer *target);
    void OnStart();
    void OnDestroy();

    bool HasAsset() const { return static_cast<bool>(data_); }
    std::string GetCurrentClip() const;
    int GetFrame() const;
    int GetElapsedTicks() const { return elapsed_ticks_; }
    bool IsPlaying() const;
    bool IsPaused() const { return paused_; }
    bool IsCompleted() const { return completed_; }
    std::uint64_t GetPlaybackId() const { return playback_id_; }

private:
    bool HasLiveBinding() const;
    SpriteRenderer *ResolveTarget(const ActorHandle &owner, const std::string &target_key) const;
    void ClearOwnedPose();
    void BeginPlayback(std::size_t index);
    Event FrameEvent() const;
    const SpriteAnimationAsset::Clip *CurrentClip() const;

    std::shared_ptr<const SpriteAnimationAsset::Data> data_;
    std::size_t clip_index_ = 0;
    std::size_t frame_index_ = 0;
    int elapsed_ticks_ = 0;
    bool selected_ = false;
    bool paused_ = false;
    bool completed_ = false;
    bool destroyed_ = false;
    std::uint64_t playback_id_ = 0;
    std::uint64_t pose_owner_;
    std::vector<Event> pending_events_;
    // The pose image path for pose_image_data_, built once per asset.
    std::string pose_image_;
    const SpriteAnimationAsset::Data *pose_image_data_ = nullptr;
    const SpriteAnimationAsset::Clip *pose_image_clip_ = nullptr;
    ActorHandle posed_actor_;
    std::string posed_target_;
};

#endif
