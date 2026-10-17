# Rendering, Audio, and Input

## `Image`

### UI-space drawing

| Function | Description |
| --- | --- |
| `Image.DrawUI(image_name, x, y)` | Draws a UI image with default tint |
| `Image.DrawUIEx(image_name, x, y, r, g, b, a, sorting_order)` | Draws a UI image with tint and sorting order |
| `Image.DrawUIScaled(image_name, x, y, scale_x, scale_y, r, g, b, a, sorting_order)` | Draws a UI image scaled from its top-left corner; a negative scale mirrors it in place |
| `Image.GetSize(image_name)` | The image's pixel size as a `Vector2` (works headless; raises if the image cannot load) |

Images named `dynamic:...` are generated at runtime by native code (for example
`Tilemap:ComputeLight` light maps) and drawn by name like image files. A light map
multiplies what is drawn under it and is sampled smoothly; other images blend normally
with pixel-art sampling.

### World-space drawing

| Function | Description |
| --- | --- |
| `Image.Draw(image_name, x, y)` | Draws a world image with default pivot/tint |
| `Image.DrawEx(image_name, x, y, rotation_degrees, scale_x, scale_y, pivot_x, pivot_y, r, g, b, a, sorting_order)` | Full world-space image draw |
| `Image.DrawPixel(x, y, r, g, b, a)` | Queues a pixel draw |

## `Text`

### `Text.Draw(content, x, y, font_name, font_size, r, g, b, a)`

Draws screen-space text.

## `Audio`

| Function | Description |
| --- | --- |
| `Audio.Play(channel, clip_name, does_loop)` | Plays a chunk-style clip on one mixer channel |
| `Audio.HasClip(clip_name)` | Returns whether a clip can be resolved from the current resource set |
| `Audio.Preload(clip_name, as_music)` | Warms a clip or music track into the audio cache |
| `Audio.Halt(channel)` | Stops playback on a channel |
| `Audio.SetVolume(channel, volume)` | Sets one channel volume, clamped to `0..128` |
| `Audio.IsPlaying(channel)` | Returns whether a mixer channel is currently active |
| `Audio.IsPlaybackEnabled()` | Returns whether runtime audio routing is currently enabled |

Use `Audio.*` for SFX, layered ambience, and other channel-based sounds.
Agents see every `Audio.Play`, `Music.Play` and `Music.Halt` as `audio.play`,
`music.play` and `music.halt` events, with or without an audio device.

## `Music`

| Function | Description |
| --- | --- |
| `Music.Play(clip_name, does_loop)` | Plays the dedicated BGM/music stream |
| `Music.Halt()` | Stops the current music stream |
| `Music.SetVolume(volume)` | Sets music-stream volume, clamped to `0..128` |
| `Music.IsPlaying()` | Returns whether the music stream is currently active |

Use `Music.*` for BGM. It is separate from `Audio` channels, so BGM does not
compete with normal SFX channels.

## `Input`

### Keyboard

| Function | Description |
| --- | --- |
| `Input.GetKey(keycode)` | Key held |
| `Input.GetKeyDown(keycode)` | Key pressed this frame |
| `Input.GetKeyUp(keycode)` | Key released this frame |
| `Input.GetAnyKeyDown()` | Any key or mouse button pressed this frame |

### Actions

Actions are declared in an optional `actions.json` at the project root and read
by name, so gameplay does not depend on particular keys and agents can set
actions directly (the agent protocol's `actions` parameter):

```json
{"version": 1, "actions": {
  "attack": {"type": "button", "keys": ["space"], "mouse": [1]},
  "zoom": {"type": "axis", "negative": ["q"], "positive": ["e"]},
  "move": {"type": "axis2d", "left": ["a"], "right": ["d"], "up": ["w"], "down": ["s"]}}}
```

Bound keys and agent values combine: a button is held when either holds, an
axis sums and clamps to [-1, 1], a 2D axis sums and is scaled to at most unit
length. Screen y grows downward, so `up` is y = -1. An invalid file fails
project startup. Unknown action names raise a Lua error.

| Function | Description |
| --- | --- |
| `Input.GetAction(name)` | Action held (nonzero) |
| `Input.GetActionDown(name)` | Held now but not at the previous frame end |
| `Input.GetActionUp(name)` | Held at the previous frame end but not now |
| `Input.GetActionValue(name)` | Number: 0/1 for buttons, [-1, 1] for axes, x for 2D axes |
| `Input.GetActionVector(name)` | `Vector2` (y is 0 except for 2D axes) |

### Mouse

| Function | Description |
| --- | --- |
| `Input.GetMousePosition()` | Returns a `vec2` with `.x` and `.y` |
| `Input.GetMouseButton(button_num)` | Mouse button held |
| `Input.GetMouseButtonDown(button_num)` | Mouse button pressed this frame |
| `Input.GetMouseButtonUp(button_num)` | Mouse button released this frame |
| `Input.GetMouseScrollDelta()` | Scroll wheel delta for this frame |
| `Input.HideCursor()` | Hides the cursor |
| `Input.ShowCursor()` | Shows the cursor |

## `vec2`

`Input.GetMousePosition()` returns `vec2`, not `Vector2`.

It exposes:

- `.x`
- `.y`

Use `Vector2` for physics math and raycasts.
