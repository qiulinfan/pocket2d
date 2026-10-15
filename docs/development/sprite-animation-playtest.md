# SpriteAnimation playable fixture

```sh
./build/modern-debug/game --project tests/fixtures/sprite_animation
```

Arrow keys move the player and select walk. Releasing movement selects idle.
Space starts a one-shot attack and returns to idle on completion. Leftward
movement mirrors the sprite; direction changes do not restart the selected walk
clip. P pauses, R resumes, D toggles animation enable, S stops, and X explicitly
restarts walk. T requests the same clip to exercise idempotent Play.

The fixture is a geometric diagnostic atlas, not migrated Village_Rim production
art. PNGs and their standard-library generator are included. Integer tick tests
verify exact frame timing, event markers, completion, pause/resume, one-shot return,
stable callback order, reset replay and stale identity rejection. Pixel tests
cover actual atlas cells, tint/alpha, pivot, mirroring and viewport size.
Human review of animation quality, combat feedback and control feel is still
required; these short test clips do not establish playable production polish.

The editor can configure SpriteAnimation fields through the existing component
Inspector. `animation_resource`, `renderer_key` and initial `clip` are authoring
fields, readonly to Lua; gameplay uses Play/Restart/Pause/Resume/Stop. An empty
resource is an inert placeholder. Frozen editor views apply the current pose
without advancing ticks or delivering animation callbacks. A dedicated clip
editor and timeline are outside this slice.
