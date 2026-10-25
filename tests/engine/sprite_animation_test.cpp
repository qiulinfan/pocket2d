#include "rendering/SpriteAnimation.h"
#include "rendering/SpriteRenderer.h"
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void Check(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
void Reject(const std::function<void()> &operation, const char *message) {
    bool rejected = false;
    try { operation(); } catch (const std::runtime_error &) { rejected = true; }
    Check(rejected, message);
}
SpriteAnimationAsset::Data Asset() {
    SpriteAnimationAsset::Data data;
    data.image = "atlas.png"; data.columns = 3; data.rows = 2;
    data.tile_width_px = data.tile_height_px = 16;
    data.clips = {{"walk", true, {{1,1,2,"first"}, {2,3,3,"step"}}},
                  {"attack", false, {{1,2,1,"windup"}, {2,2,2,"hit"}}},
                  {"single", false, {{2,1,1,"only"}}}};
    return data;
}
void CheckEvent(const SpriteAnimation::Event &event, const char *type, const char *clip,
                int frame, const char *marker, std::uint64_t playback) {
    Check(event.type == type && event.clip == clip && event.frame == frame &&
          event.marker == marker && event.playback_id == playback, "event identity/order mismatch");
}
void Timeline() {
    const auto asset = std::make_shared<const SpriteAnimationAsset::Data>(Asset());
    SpriteAnimation animation;
    animation.Initialize(asset);
    Check(animation.HasAsset() && animation.GetFrame() == 0 && !animation.IsPlaying(), "empty clip must be stopped");
    Check(animation.AdvanceTick().empty(), "stopped asset cannot generate events");
    Check(animation.Play("walk"), "first play starts");
    const auto id = animation.GetPlaybackId();
    Check(animation.GetFrame() == 1 && animation.GetElapsedTicks() == 0, "Play selects frame immediately");
    for (int i = 0; i < 10; ++i) { animation.ApplyPose(); animation.OnStart(); }
    Check(animation.GetFrame() == 1 && animation.GetElapsedTicks() == 0 && animation.GetPlaybackId() == id,
          "frozen and OnStart must neither restart nor advance");
    auto events = animation.AdvanceTick();
    Check(events.size() == 1, "initial frame event delivered exactly once");
    CheckEvent(events[0], "frame", "walk", 1, "first", id);
    Check(animation.GetFrame() == 1 && animation.GetElapsedTicks() == 1, "duration2 first tick");
    Check(!animation.Play("walk") && animation.GetPlaybackId() == id, "same clip play is idempotent");
    events = animation.AdvanceTick();
    Check(events.size() == 1 && animation.GetFrame() == 2 && animation.GetElapsedTicks() == 0, "duration2 boundary");
    CheckEvent(events[0], "frame", "walk", 2, "step", id);
    Check(animation.AdvanceTick().empty() && animation.AdvanceTick().empty(), "duration3 holds two ticks");
    events = animation.AdvanceTick();
    Check(events.size() == 1 && animation.GetFrame() == 1, "loop boundary");
    CheckEvent(events[0], "frame", "walk", 1, "first", id);

    animation.Pause();
    Check(animation.IsPaused() && !animation.IsPlaying(), "paused state");
    for (int i = 0; i < 5; ++i) Check(animation.AdvanceTick().empty(), "paused event freeze");
    Check(animation.GetElapsedTicks() == 0 && !animation.Play("walk") && animation.IsPaused(), "same paused clip stays paused");
    animation.enabled = false;
    animation.Resume();
    Check(!animation.IsPaused() && !animation.IsPlaying(), "disabled and paused flags independent");
    animation.Restart("attack");
    const auto attack_id = animation.GetPlaybackId();
    Check(animation.GetFrame() == 1 && !animation.IsPlaying(), "disabled restart chooses first pose");
    Check(animation.AdvanceTick().empty() && animation.GetElapsedTicks() == 0, "disabled queues entry without delivery");
    animation.enabled = true;
    events = animation.AdvanceTick();
    Check(events.size() == 2, "duration1 emits initial then next frame");
    CheckEvent(events[0], "frame", "attack", 1, "windup", attack_id);
    CheckEvent(events[1], "frame", "attack", 2, "hit", attack_id);
    Check(animation.AdvanceTick().empty(), "last frame holds");
    events = animation.AdvanceTick();
    Check(events.size() == 1, "one completion");
    CheckEvent(events[0], "completed", "attack", 2, "", attack_id);
    Check(animation.IsCompleted() && !animation.IsPlaying() && animation.GetFrame() == 2 && animation.GetElapsedTicks() == 2,
          "complete retains final pose and consumed duration");
    Check(!animation.Play("attack") && animation.IsCompleted(), "Play completed clip does not restart");
    for (int i = 0; i < 10; ++i) Check(animation.AdvanceTick().empty(), "completion emitted only once");
    animation.Restart("single");
    events = animation.AdvanceTick();
    Check(events.size() == 2 && events[0].type == "frame" && events[1].type == "completed", "single frame completion event order");
    animation.Restart("walk");
    animation.Stop();
    Check(animation.GetFrame() == 0 && animation.GetCurrentClip().empty() && animation.GetElapsedTicks() == 0 &&
          !animation.IsPaused() && !animation.IsCompleted() && animation.AdvanceTick().empty(), "Stop clears playback and pending events");
    animation.Play("walk");
    animation.AdvanceTick();
    const auto before_id = animation.GetPlaybackId();
    Reject([&] { animation.Play("missing"); }, "unknown clip rejects");
    animation.clip = "missing";
    Reject([&] { animation.Initialize(asset); }, "invalid initial clip rejects transactionally");
    Check(animation.GetCurrentClip() == "walk" && animation.GetElapsedTicks() == 1 && animation.GetPlaybackId() == before_id,
          "invalid configuration preserves current playback");
    animation.clip = "walk";
    auto invalid = Asset(); invalid.clips[0].frames[0].ticks = 0;
    Reject([&] { animation.Initialize(invalid); }, "invalid data rejects transactionally");
    Check(animation.GetElapsedTicks() == 1 && animation.GetPlaybackId() == before_id, "invalid data preserves timeline");
    animation.ClearAsset();
    Check(!animation.HasAsset() && animation.GetFrame() == 0 && animation.GetCurrentClip().empty() &&
          animation.GetElapsedTicks() == 0 && !animation.IsPlaying() && !animation.IsPaused() &&
          !animation.IsCompleted() && animation.AdvanceTick().empty(), "ClearAsset restores inert component");
    Reject([&] { animation.Play("walk"); }, "cleared asset cannot play without reconfiguration");
    animation.Initialize(asset);
    Check(animation.HasAsset() && animation.GetFrame() == 1 && animation.GetElapsedTicks() == 0 && animation.IsPlaying(),
          "cleared component accepts a new asset");
    Check(animation.AdvanceTick().size() == 1, "clear discards old events and reinitialize queues one entry");
    animation.OnDestroy();
    Reject([&] { animation.ClearAsset(); }, "ClearAsset cannot revive destroyed component");
    Check(animation.GetFrame() == 0 && animation.AdvanceTick().empty(), "destroy clears playback");
    Reject([&] { animation.Play("walk"); }, "destroyed native cannot revive");
    Reject([&] { animation.Initialize(asset); }, "destroyed native cannot reinitialize");
    animation.OnStart(); animation.ApplyPose();
    Check(!animation.IsPlaying(), "OnStart cannot revive destroyed animation");
}
void InstanceIsolation() {
    auto asset = Asset();
    SpriteAnimation first; first.clip = "walk"; first.Initialize(asset);
    SpriteAnimation second(first);
    first.AdvanceTick(); first.AdvanceTick();
    Check(first.GetFrame() == 2 && second.GetFrame() == 1 && second.GetElapsedTicks() == 0,
          "shared immutable data has independent playback");
    asset.clips[0].frames[0].ticks = 99;
    second.AdvanceTick(); second.AdvanceTick();
    Check(second.GetFrame() == 2, "const data overload owns a copy");
    SpriteRenderer renderer;
    renderer.sprite = "author.png"; renderer.sprite_row = 2; renderer.sprite_column = 3;
    renderer.SetAnimationPose(41, {"atlas.png", 3, 2, 1, 2});
    renderer.ClearAnimationPose(40);
    Check(renderer.HasAnimationOverride(), "non-owner cannot clear override");
    SpriteRenderer copy(renderer);
    Check(!copy.HasAnimationOverride() && copy.sprite == "author.png", "renderer copy excludes transient pose");
    renderer.SetAnimationPose(42, {"atlas.png", 3, 2, 2, 1});
    renderer.ClearAnimationPose(41);
    Check(renderer.HasAnimationOverride() && renderer.GetAnimationOverrideOwner() == 42, "old owner cannot clear replacement");
    renderer.ClearAnimationPose(42);
    Check(!renderer.HasAnimationOverride() && renderer.sprite == "author.png" && renderer.sprite_row == 2 && renderer.sprite_column == 3,
          "clear restores authored state without writes");
}
}
int main() {
    try { Timeline(); InstanceIsolation(); std::cout << "SpriteAnimation timeline/isolation: PASS\n"; return 0; }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
