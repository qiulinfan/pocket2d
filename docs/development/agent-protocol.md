# Local agent gameplay protocol, version 2

Launch the real runtime with an explicit project. No window or audio device is
created; the Lua lifecycle, physics and scene systems are the normal engine.

```sh
./build/sdl3-debug/game --headless --agent --project tests/fixtures/village_defense --seed 42
```

Send one JSON object per line on stdin. Each request gets one JSON response on
stdout. Diagnostics, Lua `print`, and normal Lua `io` output go to stderr. This
is trusted local project execution, not a sandbox for untrusted Lua.

The public process supervises a separate runtime worker. Defaults are 50 million
Lua instructions per request, 64 MiB of live Lua allocator bytes, and a 5-second
request deadline. `describe.execution_limits` reports the configured values.
See [script budgets](script-budgets.md) for CLI overrides, exact accounting,
input/response limits, and failure/recovery semantics. These limits do not cap
native allocations or restrict filesystem/network access.

Every command, its access level (`read`, `simulate`, `modify`, `session`) and a
self-contained JSON Schema 2020-12 for its parameters are defined once in
`scripting/api/agent-protocol.json`. `tools/generate_agent_protocol.py` embeds
that schema in the runtime (`describe.command_schema`, MCP tool definitions) and
writes the [command reference](agent-commands.md); CTest checks both are current.

Version 2 keeps every version 1 command and its semantics. It adds `run_until`,
`describe.command_schema`, and request correlation: a request may carry an `id`
(string or integer) that its response, including errors, echoes. Responses report
`protocol: 2`.

```json
{"cmd":"describe"}
{"cmd":"observe"}
{"cmd":"step","frames":40,"keys":["right"]}
{"cmd":"step","frames":1,"keys":[]}
{"cmd":"step","mouse":{"x":100.5,"y":80,"buttons":[1],"wheel":-1}}
{"cmd":"run_until","max_frames":600,"keys":["right"],"until":[{"event":"crop_harvested"},{"actor":"player","property":"x","op":">=","value":12}],"id":"r1"}
{"cmd":"reload_scripts","types":["Village"]}
{"cmd":"reset","seed":42}
{"cmd":"quit"}
```

- `describe` reports the command surface without requiring a healthy scene. With
  a loaded project it also reports component names and the scalar/array property
  types used by the actual editor/runtime metadata, for agent discovery. It is
  explicitly a supported-serialization view, not a complete Lua type checker.
  `script_api` separately embeds the versioned curated host contract used to
  generate TypeScript/LuaLS declarations. It records self/static conventions,
  nullable values and access modes; it is not a new RPC command surface or a
  claim that every engine API is described. See `script-api-contract.md`.
- `observe` returns frame, fixed delta, seed, scene, running state, stable actor
  UIDs, world positions and supported component properties. Actors and fields
  are sorted for repeatable observations. It does not simulate a tick.
  `uid` and `parent_uid` are opaque decimal strings, preserving all uint64 bits
  for JavaScript/TypeScript clients; parent `"0"` means no parent.
  Optional `tilemaps` requests inspect bounded cells without advancing or
  committing pending edits; see the example below.
- Observations (and `step` results) include `diagnostics`: engine log records
  written since the previous observation, oldest first, each with `level`,
  `category`, `frame` and `message` (at most 32 records, messages truncated to
  512 bytes), and `diagnostics_dropped`, the number not shown. Categories
  include `lua` (runtime errors), `script` (`Debug.Log`), `component`, `scene`,
  `config`, `audio`, `render` and `input`.
- `step` advances 1–3,600 fixed 1/60 s ticks. `keys` is the complete held key
  set; omitting it retains previous held state, while `[]` releases it. Key
  edges occur once at the start of the request. Invalid arguments are rejected
  before mutation. If gameplay quits or raises an error, the actual completed
  frame is reported; the request is not falsely reported as a full advance.
- `step.mouse` optionally supplies finite screen-space `x`/`y`, a complete held
  `buttons` array (`1` left, `2` middle, `3` right), and signed `wheel`. Omitted
  coordinates/buttons retain their state; `[]` releases all buttons. Button
  transitions and wheel input occur on the first tick only. Full reset clears
  held keys, mouse buttons, coordinates and pending wheel input.
- `run_until` applies `keys`/`mouse` like `step`, then advances up to
  `max_frames` (1–36,000) ticks, checking its 1–16 `until` conditions after each
  tick and stopping at the first tick where any holds. Conditions are an
  `event` type published through `Event.Publish` during that tick; an actor
  property comparison (`actor` as a decimal UID string or a name, optional
  `component` key, `property`, `op` one of `== != < <= > >=`, and a number,
  string or boolean `value`; without `component`, `property` is the world
  `x`, `y` or `rotation`); or `actor_exists` with `value` true/false. Missing
  actors, components or properties and mismatched types compare false. The
  response is an observation plus `run_until`: `reason` (`condition`,
  `max_frames` or `stopped` when gameplay quit), `frames` advanced, the
  matched `condition` index or `null`, and for event matches the `event` type
  and frame. Invalid conditions return `invalid_argument` before any input or
  tick is applied. Errors during the run behave as in `step`.
- `step` and `run_until` accept `events`: `true` for every type or an array of
  1..64 type names. The response then carries `events`, oldest first and at
  most 256, each with `frame` (the frame being simulated when it was
  published, as in diagnostics; the observation after that tick reports one
  more), `type` and `payload`, plus `events_dropped`. Payloads are the event
  object read without metamethods: at most 4 levels, 32 entries per table and
  256 bytes per string; functions and unknown userdata become `"<function>"`
  or `"<userdata>"`, deeper tables `"<table>"`, `Vector2` `{x,y}` and actors
  `{actor_uid}`. Events are not reported unless requested.
- `step` and `run_until` accept `actions`, the complete set of agent-held
  semantic actions declared in the project's `actions.json`: `true`/`false`
  for buttons, a number for axes, `[x, y]` for 2D axes (components clamped to
  [-1, 1]). Omitting it keeps the previous values and `{}` releases them; reset
  releases them too. Unknown actions or wrong value types return
  `invalid_argument` before any input is applied. `describe.actions` lists the
  declared actions and types. See the Lua [Input actions](../lua-api/rendering-audio-input.md#actions).
- `observe`, `step` and `run_until` accept `select` to narrow `actors`:
  `names`, `uids` (decimal strings) and component `types` each match any listed
  value, `region` keeps actors whose world position lies in the inclusive
  `min_x/min_y/max_x/max_y` box, and the filters combine. `limit` keeps the
  lowest UIDs and `components:false` omits component properties. Selected
  responses add `actors_matched`, the count before `limit`.
- `observe`, `step` and `run_until` accept `image: {width, height}` (at most
  1920×1080). The worker renders the current frame with SDL's software renderer
  into an offscreen surface, from the game camera with the configured window
  size scaled to the requested size, and adds `image` with `format: "png"`,
  `encoding: "base64"` and `data`. Lua draw calls from the last tick are
  included. Rendering never changes the simulation, and at the window size
  the frame matches graphical mode with the software renderer pixel for pixel
  (`agent_image_test`).
- `reload_assets` reloads image files and the spritesheet metadata that changed
  on disk since they were loaded (missing images are retried); the next draw
  uses them. It does not advance time and returns an observation plus
  `reload_assets: {images, spritesheet_metadata}`. It is not recorded in
  replays because it depends on files outside the request. The editor checks
  for changed assets every 30 rendered frames, and the graphical runtime does
  so when `POCKET_HOT_RELOAD=1`.
- `reload_scripts` takes 1..64 distinct registered Lua component `types` and
  stages their known source files as one batch. Success returns a same-frame
  observation plus `reload.types`, `instances_updated` and empty diagnostics.
  Ordinary syntax/evaluation/validation rejection returns `script_reload_failed`,
  `frame`, `requires_reset:false`, and diagnostics with type/resource/stage/message/
  line (0 if unavailable). The old running state remains usable. It does not
  simulate, reset, repeat OnStart or clear input. Unsupported live closure state
  and anonymous event subscriptions reject explicitly. See [script-reload.md](script-reload.md)
  for isolated top-level capabilities, preserved-state rules and allocation limits.
- `reset` destroys and recreates runtime state, then reloads the project. The
  uint32 seed is applied before component scripts execute, including top-level
  random draws. Omitting it retains the current seed. Failed resets leave the
  host usable for retry/describe/quit, with observe/step rejected until recovery.
  Each reset starts a new worker process, including after a script budget or
  deadline failure. It does not reuse a damaged Lua VM.
- `quit` responds with `ok`, `running:false` and frame, then closes cleanly even
  if scene observation is invalid.

Success responses carry `ok:true`. Errors carry `ok:false`, `error`, and the
current frame. A callback error may occur after gameplay mutation; there is no
transactional rollback claim. A default ParticleSystem's unset `end_scale` is
JSON `null`; other non-finite numeric state is rejected rather than producing
invalid JSON. This observation format is a supported-property view, not a full
Lua heap/savegame snapshot.

Worker failure, instruction/heap limits, or deadline errors include
`requires_reset:true`. When no trustworthy current worker snapshot is available,
`frame` is the last observed frame and `frame_is_last_observed:true` makes that
distinction explicit. `describe` remains available from its last validated cache;
`step`/`observe` require successful reset. The internal worker command and fields
`_bootstrap`, `_pocket_request_id`, and `_initial_observation` are reserved.

## MCP server

`--mcp` serves the same commands as [Model Context Protocol](https://modelcontextprotocol.io)
tools over stdio (newline-delimited JSON-RPC 2.0), backed by the same supervised
worker, limits and recovery rules as JSONL:

```sh
./build/sdl3-debug/game --headless --mcp --project tests/fixtures/village_defense --seed 42
```

A client configuration such as `.mcp.json` names that command and its arguments.

- Every command except `quit` is a tool (`describe`, `observe`, `step`,
  `run_until`, `reload_scripts`, `reset`). Tool input schemas are the command
  parameter schemas; annotations mark `read` tools read-only. A call returns the
  protocol response both as `structuredContent` and as JSON text, with
  `isError` set when the response has `ok:false`. Unknown or disabled tools,
  non-object arguments and the reserved argument names `cmd`, `id` and `_*` are
  JSON-RPC `-32602` errors.
- `--mcp-access` limits the offered tools to a comma list of `read`,
  `simulate` and `modify` (default all three). Disabled tools are neither
  listed nor callable.
- The server supports both protocol eras. Requests carrying
  `_meta["io.modelcontextprotocol/protocolVersion"] = "2026-07-28"` are served
  statelessly with `resultType: "complete"`, and `server/discover` reports that
  version; any other per-request version gets `-32022` with the supported list.
  Legacy clients may send `initialize` (versions 2024-11-05 through
  2025-11-25 are echoed; others receive 2025-11-25). Notifications are ignored;
  batches are rejected.
- The runtime is one World per server process. If the project fails to start,
  tool calls report `worker_unavailable` until `reset` succeeds.

## Replay files

`--record FILE` (with `--agent`) writes a replay as JSON lines: a header with
`pocket_replay: 1`, the protocol version, engine version, a project content
digest (FNV-1a 64 over non-hidden files; change detection, not security) and
the starting seed; the initial observation; then every `observe`, `step`,
`run_until`, `reset` and `reload_scripts` request with its response. Client
`id`s and `diagnostics` are omitted because they describe the session rather
than gameplay. `describe` and `quit` are not recorded.

```sh
./build/sdl3-debug/game --headless --agent --project P --seed 42 --record session.jsonl
./build/sdl3-release/game --headless --replay session.jsonl --project P
```

`--replay FILE` starts a fresh host with the recorded seed, re-sends each
request and compares each response field by field. It prints one JSON line and
exits 0 when everything matches (`records`, final `frame`, `project_changed`),
or exits 1 with `code: replay_diverged`, the record index and command, the
recorded `frame`, the first differing JSON pointer `path`, and the `expected` and
`actual` values there. A changed project digest is reported as
`project_changed: true` but does not by itself fail a replay. Responses are
compared exactly; a replay recorded by a Debug build matched Release over 1,020
frames of `village_defense` and `two_world_village` on macOS arm64. Other
platforms are unverified.

## Bounded Tilemap observations

```json
{"cmd":"observe","tilemaps":[{"actor_uid":"10","component_key":"map","layer":"ground","cells":[[8,8],[9,8]]}]}
```

At most 16 queries and 256 total cells are accepted. Coordinates must be signed
32-bit integers. A query identifies an existing Tilemap component and either
`ground` or `solid`; invalid queries fail before observation. Out-of-bounds cells
return JSON `null`. Responses report the map revision, dimensions and cell size,
plus requested cell values. UIDs return as decimal strings even when a request
uses a lossless unsigned JSON integer. Pending `SetCell` writes remain invisible
until the next pre-physics commit. The real farming fixture and Python protocol
test are `tests/fixtures/tilemap_farm` and `tests/agent_tilemap_test.py`.

Seeded replay is tested on the same platform/build. Cross-platform bit-exact
physics, arbitrary Lua IO determinism, controller action schemas,
checkpoint restoration and concurrent environments require further work.

## Scene transition failures

A failed queued `Scene.Load` preflight returns `scene_load_failed` with the actual
completed frame and `requires_reset:false`. The preserved world still executes
that ordinary gameplay tick. The diagnostic is reported for that attempt only;
a following valid `step` can advance its full requested count. Unexpected errors
after commit starts return `requires_reset:true` and end the worker. `reset`
creates a fresh runtime. Neither case promises rollback of arbitrary callback IO.
See [scene transitions](scene-transitions.md) for preparation and persistence rules.

## Runtime actor publication

A normal Actor.Instantiate preparation error is catchable by Lua and leaves no
published candidate or pending teardown. If uncaught, it uses the ordinary script
error response; later requests remain usable. An unexpected host failure after
publication starts instead stops the runtime and returns `actor_publish_failed`
with `requires_reset:true`, then ends the worker. This fatal classification is
not a guarantee of rollback after allocator failure. See
[runtime actor preparation](runtime-actor-preparation.md).

## Scene edit commit failures

An ordinary scene-edit preparation rejection preserves the running world. If an
unexpected exception happens after a prepared edit starts publication, the runtime
stops and the worker returns `scene_edit_failed` with `requires_reset:true`, then
exits. Reset creates a fresh runtime; external callback effects are not rolled
back. This category also covers a fatal hierarchy command invoked by gameplay.
There is no new agent wire command for editor mutations. Direct manager component
key publication uses the existing `actor_publish_failed` fatal boundary. See
[editor command transactions](editor-command-transactions.md).
