# Native SpriteAnimation implementation contract

2026-10-15, phase 3. This is the active implementation contract, not a completion
claim. Build one real animation playback slice before save/load or another VM.

## Asset

`animations/*.animation.json`, exact relative resource path; image is exact
relative to `images/`. No basename search or editor grid dependency. Version 1:

```json
{"version":1,"image":"hero.png","columns":3,"rows":1,"tile_width_px":16,"tile_height_px":16,"clips":[{"name":"walk","loop":true,"frames":[{"row":1,"column":1,"ticks":2},{"row":1,"column":2,"ticks":2,"event":"footstep"}]}]}
```

Strict required fields, optional frame event only; reject unknown/duplicate
fields, duplicate clip names, bad integer/range/UTF-8, path escape and image/grid
mismatch. Max file 4 MiB, clips 256, total frames 65,536, per-frame ticks 1..36,000,
name/event lengths 1..128 bytes (empty event only when absent). Positive grid and
pixel sizes with checked products. No new external dependency.

C++ namespace `SpriteAnimationAsset`: Frame {int row,column,ticks; string event},
Clip {string name; bool loop; vector<Frame> frames}, Data {int version=1;
string image; int columns,rows,tile_width_px,tile_height_px; vector<Clip> clips;
filesystem::path resolved_image_path}. APIs ValidateData(Data),
ValidateAtlasImage(Data&, images_root), LoadFile(path,images_root),
LoadResource(relative), FindClip(const Data&,name)->const Clip*.

## Component

Native SpriteAnimation shares immutable clip data and owns playback plus generation-safe actor identity.
Manager preflight loads each referenced resource once per scene batch and gives
players shared_ptr<const Data>; the standalone Initialize(const Data&) overload
owns a validated copy. This has no mutable global resource manager.
Author fields: enabled=true, animation_resource="", renderer_key="sprite",
clip="". Exactly one animator may target a given SpriteRenderer key on its actor.
Configured assets must resolve target and initial clip in preflight before the
current scene is cleared. Empty resource is an inert authoring placeholder.
Renderer and animation component key order must not change initialization.

Lua methods: Play(name) is idempotent when already selected (even paused or
completed); Restart(name) explicitly resets; Pause(), Resume(), Stop(). Bad clip
names reject before changing valid playback. No rate/time controls this slice.
Readonly state: current_clip, frame (1-based; 0 when stopped), elapsed_ticks
(current-frame consumed ticks, reset on frame transition),
playing (live/enabled/selected and not paused/completed), paused, completed.
Empty initial clip means stopped, not an implicit first clip.
Native Initialize(data), AdvanceTick()->vector<Event>,
ApplyPose(), OnStart(), OnDestroy(); C++ Play(name,bool restart=false) is okay.
Event {string type (frame/completed), clip, marker; int frame; uint64 playback_id}.
Expose GetPlaybackId for coordinator validation; increment on Play transition,
Restart, Stop and destruction so queued old-playback events can be rejected.

Play selects its first frame immediately and queues a frame-entry event. Engine
advances existing animations once per tick after pending OnStart and before Lua
OnUpdate. Thus Play in OnUpdate at tick T displays frame 1 in that snapshot and
first consumes time at T+1. Autoplay exists at frame 0; duration d changes at
boundary d. Each frame remains for its positive integer tick duration. No float
accumulation. Loop enters frame 1 again; non-loop completes once after the last
frame duration and holds its final pose. Pause/disabled freezes time and queued
events, retaining the pose; Stop clears events/time and restores base sprite.
Frozen editor renders apply pose without advancing or delivering gameplay events.

Native pose is a transient SpriteRenderer override, separate from authored sprite,
row/column/grid state. It preserves tint, pivot, scale/mirroring, sort and transform.
SpriteRenderer gets explicit animation atlas grid and exact resolved image, no
writes to .pocketengine. Override is cleared by its owning animation only on stop,
remove, actor destroy and reset. Replacing/removing target or animation cannot
leave a stale pointer or let an old component control a new actor generation.

## Events and integration

Coordinator dispatches `OnAnimationEvent(self,event)` on enabled Lua components
of the owning actor, in stable component key order, after native advance and
before ordinary OnUpdate. Event fields: type, clip, frame, marker, component_key.
Native class has no Lua callbacks. Snapshot recipients and validate actor,
component liveness and playback_id before each dispatch; Stop/Restart/removal
inside a callback invalidates remaining old events. Exceptions use normal runtime
error reporting. Do not recurse native AdvanceTick from callbacks.

Manager prepares all assets/ownership before scene clear, initializes after all
components bind, updates before OnUpdate, applies poses in frozen preview, and
clears preflight caches on failures. Author-only fields are readonly in Lua;
editor property mutation validates first and preserves live state on failure.
Agent observe uses existing scalar component metadata; no custom protocol needed.

## Ownership and gates

Native worker: SpriteAnimation.h/.cpp, SpriteRenderer.h/.cpp, native unit/render
tests. Resource worker: SpriteAnimationAsset.h/.cpp and asset tests. QA worker:
animation fixture (self-authored PNG geometry), runtime/protocol tests. Root:
all CMake, manager/Engine/Lua binding/metadata, docs and final integration.
All agents share this checkout, never revert unrelated changes.

Required evidence: loop/one-shot timeline; idempotent Play; explicit restart;
pause/resume/stop; disabled/frozen no advance; markers/completion count/order;
reentrant callbacks; stale actor/target/animation handles; invalid scene/resource
preserves old world; real sprite pixel readback with mirror/tint and atlas indices;
input-driven walk/attack/return-to-idle in headless and graphic fixture; reset
replay. Run meaningful Debug, Release, ASan/UBSan checks. Do not claim complete
Unity animation parity, human feel, production asset migration or SOTA.
