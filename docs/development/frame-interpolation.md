# Frame interpolation

The standalone runtime (`Engine::GameLoop`) ticks gameplay at a fixed 60 Hz and
draws frames as often as it can between ticks (`max_fps`/`vsync` limit it).
Without interpolation a 120 Hz display showed each tick twice. With
`game.config`'s `frame_interpolation` (default `true`) a frame drawn between
two ticks shows the world part of the way from the previous tick to the latest,
so every frame moves. It is presentation only: gameplay, replays, agent
stepping (`StepSimulation`), the editor's play mode and `--frames` runs never
call the interpolating path and stay bit-for-bit as before.

## How it works

- **Fraction.** After a frame's ticks, `alpha = lag / tick`, the share of the
  next tick that real time has already covered (0 just after a tick).
- **Camera.** GameLoop remembers the camera position and zoom before each tick
  and renders at `previous + (latest - previous) * alpha`. Gameplay still sees
  only the tick's camera (`Camera.GetPosition`, `ScreenToWorld`).
- **Draws.** `ImageDrawRequest::interpolation_key` names what a draw shows.
  `Renderer::HoldFrameRequests(true)` keeps the previous tick's scene requests
  and pairs each keyed request with the previous tick's request of the same key
  (several draws sharing a key pair in submission order, so an entity's layers
  can share one key). `RequeueHeldFrameRequests(alpha, snap)` moves each pair's
  position, rotation (the short way round) and scale by alpha. Unkeyed and
  unpaired draws stay where the latest tick put them, which is right for
  everything fixed to the world (tiles, tilemaps, decorations).
- **Keys.** Built-in `SpriteRenderer`s key by actor UID, so they are queued with
  each tick instead of in `render()`. Lua passes an optional last argument to
  `Image.Draw`/`Image.DrawEx`: a number or a string, in separate key spaces
  from each other and from actors.
- **Cuts.** Moving more than a screen (the larger window side at the current
  zoom) in one tick is a teleport and snaps, for the camera and for draws. A
  flip (scale sign change) or a turn of more than 90 degrees in a tick keeps the
  latest angle and scale. A scene load forgets the previous tick.

Camera-relative draws (a sky or parallax layer positioned from the camera) need
keys too, or they would jitter against the interpolated camera; with a key they
move exactly with it, since interpolation is linear.

## Cost and limits

- The picture runs one tick (16.7 ms) behind the simulation, the usual price of
  interpolating between known states. `frame_interpolation: false` shows the
  latest tick instead (each tick repeated until the next).
- Pairing sorts the keyed requests once per tick; each frame adds one pass over
  the held requests. Unkeyed games pay nothing beyond the copy they already had.
- Not interpolated: particles, screen-space UI and text (damage numbers placed
  with a world-to-screen conversion drift up to a tick against the world), and
  draws whose count per key changes between ticks may pair the wrong layers.

## Verification

`tests/frame_interpolation_test.cpp` covers pairing, the fraction, cuts and
the Lua key argument. On 2026-10-17 PocketTerraria (a scratch copy with
`max_fps: 120`, a fresh scratch save, `RENDERLOGGER=1` and an input file
walking right for 360 ticks) drew about 1.9 frames per tick. Interpolated, the
scroll changed in all 452 consecutive frames (steps alternating 0.083/0.104
tiles); with `frame_interpolation: false` 217 of 457 frames repeated the
previous one (steps alternating 0/0.187 tiles). Frames where the player held
still matched ticks where it was blocked climbing terrain.
