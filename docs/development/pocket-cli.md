# pocket command line

`tools/pocket` is a headless command line for projects. It needs Python 3 and
a built runtime: `--game PATH`, the `POCKET_GAME` environment variable, or
`build/sdl3-release/game` then `build/sdl3-debug/game`.

| Command | Does |
| --- | --- |
| `pocket check PROJECT` | Parses every asset, validates it against `api/schemas`, loads scenarios, starts the runtime and reports its diagnostics, and runs `lua-language-server` or `emmylua_check` when one is on `PATH` |
| `pocket test PROJECT` | Plays each scenario that has a `script` and compares the outcome with `expect`; verifies every `replays/**/*.jsonl` |
| `pocket run PROJECT [--mcp] [--seed N] [--record FILE]` | Serves the JSONL protocol, or MCP with `--mcp`, on stdio |
| `pocket replay PROJECT FILE` | Verifies one replay |
| `pocket render PROJECT --out PNG [--frames N] [--keys ...] [--width W --height H]` | Renders a frame headlessly |
| `pocket new DIRECTORY` | Creates a minimal project with an action map, a component, a scenario with a script, and `AGENTS.md` |
| `pocket import aseprite SHEET.json --project P --name N` | Converts an Aseprite sprite-sheet export (array or hash JSON) into `animations/N.animation.json` and copies the sheet to `images/` |
| `pocket import ldtk WORLD.ldtk --project P --name N --ground LAYER [--solid LAYER] [--level ID]` | Converts one LDtk level into `maps/N.tilemap.json` and copies its tileset to `images/` |
| `pocket import unity SCENE.unity --assets ASSETS --project P --name N [--skip-texture-folder F ...] [--point-prefix P ...]` | Converts a Unity 2D scene's tilemaps, sprites, colliders and entities into `scenes/N.scene`, `maps/`, `images/vr_tiles/` and `images/vr_props/` (needs Pillow) |

`--json` (check and test) prints one object per line with `severity`
(`error`, `warning`, `info`), `code`, `file` relative to the project, and
`message`; the exit status is 1 when any error was reported.

A scenario `script` is a fixed input plan for regression tests: entries of
`frames` with `keys` and/or `actions` within the scenario's `controls`. Each
entry runs as one `run_until` on the scenario's success and failure
conditions. `expect` is `success` (default), `failure` or `truncated`.

The Lua checkers receive a temporary configuration whose `workspace.library`
is the generated `api/types/` directory. Neither checker is installed in the
development environment where this was written, so that path is untested;
`check` reports `lua_check_skipped` when none is found.

## Importers

Imports are exact or refused; the written asset is validated against its
schema.

- Aseprite: each frame tag becomes a clip; `forward`, `reverse`, `pingpong`
  and `pingpong_reverse` directions are expanded into frame lists, and a tag
  with a finite `repeat` plays once (`loop: false`). Durations convert from
  milliseconds to 60 Hz ticks (rounded, at least one). Without tags all frames
  form one looping clip named `default`. Frames must form a uniform grid with
  no borders, padding, trimming or rotation.
- LDtk: the project must embed its levels. `--ground` and `--solid` name
  Tiles, AutoLayer or auto-tiled IntGrid layers sharing one tileset without
  spacing or padding; solid cells are drawn and collide. LDtk tile id `t`
  becomes Pocket2D id `t + 1` (row-major, 1-based); stacked tiles keep the
  topmost; flipped tiles are refused. Entities and IntGrid values without
  tiles are not imported.
- Unity: reads the scene and its prefab instances (overrides and stripped
  objects included) from text YAML, resolving GUIDs through `--assets`. Each
  Tilemap becomes one map actor (tiles whose Tile asset has a collider are
  solid, as full cells; flips/rotations are baked into the atlas); oversized
  tiles and SpriteRenderers become sprite actors (16 PPU art keeps its scale,
  flips become negative scales); sorting layers become `layer index * 100000 +
  order` with y-sorting; solid Box/Circle/Capsule colliders become static
  bodies; GameObjects with a non-static Rigidbody2D become `SpawnMarker`
  actors (the project defines that component); inactive tilemaps and entities
  are imported disabled (`enabled: false`) rather than dropped; `--point-prefix` also imports
  GameObjects named with that prefix (spawn points) as `SpawnMarker` actors of
  kind `point`; a point that has a sprite gets the marker on its sprite actor.
  Scaled or rotated tilemaps,
  sliced sprites and triggers are reported in `import_report.json`, not
  converted. Scene actors with uid below 1000 survive re-imports. Verified on
  Village_Rim's Gameplay scene (49 tilemaps, 189 sprites, 111 colliders).
