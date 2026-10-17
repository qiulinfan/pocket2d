# Application, Scene, and Camera

## `Debug`

### `Debug.Log(message)`

Logs a string to the runtime console. In agent mode diagnostics go to stderr.

## `Application`

### `Application.Quit()`

Requests the runtime to stop after the current simulation frame. The host can
finish its response and release resources; it does not terminate the process
from inside the Lua callback.

### `Application.Sleep(milliseconds)`

Sleeps the current thread. Negative values are clamped to `0`.

### `Application.GetFrame() -> int`

Returns the simulation frame number. Each completed simulation tick advances
it once; frozen previews and additional presentations do not advance it.

### `Application.GetDeltaTime() -> float`

Returns the fixed simulation step, currently `1/60` second. Headless stepping
uses the same lifecycle and physics step as interactive gameplay.

### `Application.GetProjectDirectory() -> string`

Returns the absolute path of the loaded project's resource root (the folder
holding `game.config`). Trusted gameplay scripts can combine it with `io` and
`load` to read data files on demand instead of loading them all at startup.

### `Application.OpenURL(url)`

Asks the host OS to open a URL with the default browser/app.

## `Scene`

### `Scene.Load(scene_name)`

Queues loading a scene by name at the next frame boundary. Invalid scene
preparation preserves the old world, which can still run its ordinary tick.
Known component source paths are reused; editing a file requires explicit script
reload or a fresh-VM reset. New source variants must satisfy the isolated
declaration contract. See [scene transitions](../development/scene-transitions.md).

### `Scene.GetCurrent() -> string`

Returns the current scene name.

### `Scene.DontDestroy(actor)`

Marks this actor so it survives queued scene transitions. It does not implicitly
mark descendants. A transition rejects incoming UID collisions, loss of a retained
actor's parent, or changing the source variant of a retained component type.
Incoming authored parent links must stay within the incoming scene. Full editor
scene replacement/reset does not preserve these actors.

## `Camera`

### `Camera.SetPosition(x, y)`

Sets the runtime camera world position.

### `Camera.GetPositionX() -> float`

Returns the runtime camera X position.

### `Camera.GetPositionY() -> float`

Returns the runtime camera Y position.

### `Camera.SetZoom(zoom_factor)`

Sets the runtime camera zoom.

### `Camera.GetZoom() -> float`

Returns the runtime camera zoom.

### `Camera.ScreenToWorld(x, y) -> Vector2` / `Camera.WorldToScreen(x, y) -> Vector2`

Convert between screen coordinates (window coordinates, as
`Input.GetMousePosition` reports them) and world coordinates through the
current camera position and zoom. The camera view is the configured
resolution stretched to the window, so a resized window still maps the mouse
to the right world point. Headless runs use the configured resolution as the
screen.

### `Camera.GetScreenSize() -> Vector2`

The configured resolution: the UI drawing space of `Image.DrawUI*` and `Text.Draw`.

### `Camera.GetViewSize() -> Vector2`

The world-unit size of the camera's view at the current zoom
(`screen size / (100 * zoom)`).
