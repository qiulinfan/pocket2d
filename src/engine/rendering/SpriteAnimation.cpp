#include "scripting/LuaBoundary.h"
#include "rendering/SpriteAnimation.h"
#include "rendering/SpriteRenderer.h"
#include "scripting/ComponentManager.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <atomic>
#include <stdexcept>
#include <utility>
#include <limits>

namespace {
// Distinct even when a Lua allocation reuses a destroyed animation's address.
std::atomic<std::uint64_t> next_pose_owner{1};
}

SpriteAnimation::SpriteAnimation() : pose_owner_(next_pose_owner.fetch_add(1)) {}

SpriteAnimation::SpriteAnimation(const SpriteAnimation &other) : SpriteAnimation() {
    key = other.key;
    enabled = other.enabled;
    animation_resource = other.animation_resource;
    renderer_key = other.renderer_key;
    clip = other.clip;
    if (other.data_) Initialize(other.data_);
}

bool SpriteAnimation::HasLiveBinding() const {
    if (destroyed_) return false;
    // Unbound native authoring/test objects can play, but never apply poses.
    if (actor_handle.generation == 0) return true;
    if (!actor_handle.IsValid()) return false;
    return ComponentManager::FindBuiltinComponent(actor_handle.uid, key) == this;
}

SpriteRenderer *SpriteAnimation::ResolveTarget(const ActorHandle &owner, const std::string &target_key) const {
    if (owner.generation == 0 || !owner.IsValid()) return nullptr;
    return ComponentManager::GetSpriteRenderer(owner.uid, target_key);
}

void SpriteAnimation::ClearOwnedPose() {
    if (auto *target = ResolveTarget(posed_actor_, posed_target_)) target->ClearAnimationPose(pose_owner_);
    posed_actor_ = {};
    posed_target_.clear();
}

void SpriteAnimation::Initialize(const SpriteAnimationAsset::Data &data) {
    Initialize(std::make_shared<const SpriteAnimationAsset::Data>(data));
}

void SpriteAnimation::Initialize(std::shared_ptr<const SpriteAnimationAsset::Data> data) {
    if (!HasLiveBinding()) throw std::runtime_error("SpriteAnimation: expired component");
    if (!data) throw std::runtime_error("SpriteAnimation: missing asset");
    SpriteAnimationAsset::ValidateData(*data);
    const auto *initial = clip.empty() ? nullptr : SpriteAnimationAsset::FindClip(*data, clip);
    if (!clip.empty() && !initial) throw std::runtime_error("SpriteAnimation: unknown clip '" + clip + "'");
    const auto initial_index = initial ? static_cast<std::size_t>(initial - data->clips.data()) : 0;
    // Validation precedes all visible mutation, including removal of the old pose.
    ClearOwnedPose();
    data_ = std::move(data);
    selected_ = paused_ = completed_ = false;
    frame_index_ = 0;
    elapsed_ticks_ = 0;
    pending_events_.clear();
    if (initial) BeginPlayback(initial_index);
    else ++playback_id_;
}

void SpriteAnimation::AdoptPreparedState(SpriteAnimation &prepared) {
    if (&prepared == this || !HasLiveBinding())
        throw std::runtime_error("SpriteAnimation: adoption requires a live component");
    if (prepared.actor != nullptr || prepared.actor_handle.generation != 0 || prepared.destroyed_)
        throw std::runtime_error("SpriteAnimation: adoption requires a fresh detached prepared value");
    if (playback_id_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("SpriteAnimation: playback identity exhausted");
    ClearOwnedPose();
    ++playback_id_;
    enabled = prepared.enabled;
    animation_resource = std::move(prepared.animation_resource);
    renderer_key = std::move(prepared.renderer_key);
    clip = std::move(prepared.clip);
    data_ = std::move(prepared.data_);
    clip_index_ = prepared.clip_index_; frame_index_ = prepared.frame_index_;
    elapsed_ticks_ = prepared.elapsed_ticks_;
    selected_ = prepared.selected_; paused_ = prepared.paused_; completed_ = prepared.completed_;
    pending_events_ = std::move(prepared.pending_events_);
    for (auto &event : pending_events_) event.playback_id = playback_id_;
    // actor/key/pose_owner_ stay with this live userdata. The caller applies its
    // new pose only after the complete command has published all peer changes.
    prepared.selected_ = prepared.paused_ = prepared.completed_ = false;
    prepared.frame_index_ = 0; prepared.elapsed_ticks_ = 0;
    prepared.pending_events_.clear(); prepared.destroyed_ = true;
}

void SpriteAnimation::ClearAsset() {
    if (!HasLiveBinding()) throw std::runtime_error("SpriteAnimation: expired component");
    Stop();
    data_.reset();
}

const SpriteAnimationAsset::Clip *SpriteAnimation::CurrentClip() const {
    return data_ && selected_ ? &data_->clips[clip_index_] : nullptr;
}
std::string SpriteAnimation::GetCurrentClip() const {
    const auto *current = CurrentClip();
    return current ? current->name : std::string{};
}
int SpriteAnimation::GetFrame() const { return selected_ ? static_cast<int>(frame_index_) + 1 : 0; }
bool SpriteAnimation::IsPlaying() const {
    return selected_ && enabled && !paused_ && !completed_ && HasLiveBinding();
}
SpriteAnimation::Event SpriteAnimation::FrameEvent() const {
    const auto &current = *CurrentClip();
    return {"frame", current.name, current.frames[frame_index_].event, GetFrame(), playback_id_};
}
void SpriteAnimation::BeginPlayback(std::size_t index) {
    ++playback_id_;
    clip_index_ = index;
    frame_index_ = 0;
    elapsed_ticks_ = 0;
    selected_ = true;
    paused_ = completed_ = false;
    pending_events_.clear();
    pending_events_.push_back(FrameEvent());
    ApplyPose();
}
bool SpriteAnimation::Play(const std::string &name, bool restart) {
    if (!HasLiveBinding()) throw std::runtime_error("SpriteAnimation: expired component");
    if (!data_) throw std::runtime_error("SpriteAnimation: no asset configured");
    const auto *next = SpriteAnimationAsset::FindClip(*data_, name);
    if (!next) throw std::runtime_error("SpriteAnimation: unknown clip '" + name + "'");
    if (!restart && CurrentClip() == next) return false;
    BeginPlayback(static_cast<std::size_t>(next - data_->clips.data()));
    return true;
}
bool SpriteAnimation::Restart(const std::string &name) { return Play(name, true); }
void SpriteAnimation::Pause() { if (HasLiveBinding() && selected_ && !completed_) paused_ = true; }
void SpriteAnimation::Resume() { if (HasLiveBinding() && selected_ && !completed_) paused_ = false; }
void SpriteAnimation::Stop() {
    ++playback_id_;
    ClearOwnedPose();
    selected_ = paused_ = completed_ = false;
    frame_index_ = 0;
    elapsed_ticks_ = 0;
    pending_events_.clear();
}
std::vector<SpriteAnimation::Event> SpriteAnimation::AdvanceTick() {
    if (!HasLiveBinding()) return {};
    return AdvanceBoundTick(ResolveTarget(actor_handle, renderer_key));
}
std::vector<SpriteAnimation::Event> SpriteAnimation::AdvanceBoundTick(SpriteRenderer *target) {
    if (destroyed_ || !selected_ || !enabled || paused_ || completed_) return {};
    auto events = std::move(pending_events_);
    pending_events_.clear();
    const auto &current = *CurrentClip();
    const auto &frame = current.frames[frame_index_];
    ++elapsed_ticks_;
    if (elapsed_ticks_ == frame.ticks) {
        if (frame_index_ + 1 < current.frames.size() || current.loop) {
            frame_index_ = (frame_index_ + 1) % current.frames.size();
            elapsed_ticks_ = 0;
            events.push_back(FrameEvent());
        } else {
            completed_ = true;
            events.push_back({"completed", current.name, "", GetFrame(), playback_id_});
        }
    }
    ApplyBoundPose(target);
    return events;
}
void SpriteAnimation::ApplyPose() {
    if (!HasLiveBinding()) { ClearOwnedPose(); return; }
    ApplyBoundPose(ResolveTarget(actor_handle, renderer_key));
}
void SpriteAnimation::ApplyBoundPose(SpriteRenderer *target) {
    if (destroyed_ || !selected_) { ClearOwnedPose(); return; }
    if (actor_handle.generation == 0) return;
    if (posed_actor_.uid != actor_handle.uid || posed_actor_.generation != actor_handle.generation ||
        posed_target_ != renderer_key) ClearOwnedPose();
    if (!target) return;
    if (pose_image_data_ != data_.get()) {
        pose_image_ = data_->resolved_image_path.empty() ? data_->image : data_->resolved_image_path.string();
        pose_image_data_ = data_.get();
    }
    const auto &frame = CurrentClip()->frames[frame_index_];
    target->SetAnimationPose(pose_owner_, pose_image_, data_->columns, data_->rows, frame.row, frame.column);
    posed_actor_ = actor_handle;
    if (posed_target_ != renderer_key) posed_target_ = renderer_key;
}
void SpriteAnimation::OnStart() { ApplyPose(); }
void SpriteAnimation::OnDestroy() {
    if (destroyed_) return;
    Stop();
    destroyed_ = true;
}
