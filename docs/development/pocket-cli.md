# pocket command line

`tools/pocket` is a headless command line for projects. It needs Python 3 and
a built runtime: `--game PATH`, the `POCKET_GAME` environment variable, or
`build/sdl3-release/game` then `build/sdl3-debug/game`.

| Command | Does |
| --- | --- |
| `pocket check PROJECT` | Parses every asset, validates it against `scripting/api/schemas`, loads scenarios, starts the runtime and reports its diagnostics, and runs `lua-language-server` or `emmylua_check` when one is on `PATH` |
| `pocket test PROJECT` | Plays each scenario that has a `script` and compares the outcome with `expect`; verifies every `replays/**/*.jsonl` |
| `pocket run PROJECT [--mcp] [--seed N] [--record FILE]` | Serves the JSONL protocol, or MCP with `--mcp`, on stdio |
| `pocket replay PROJECT FILE` | Verifies one replay |
| `pocket render PROJECT --out PNG [--frames N] [--keys ...] [--width W --height H]` | Renders a frame headlessly |
| `pocket new DIRECTORY` | Creates a minimal project with an action map, a component, a scenario with a script, and `AGENTS.md` |

`--json` (check and test) prints one object per line with `severity`
(`error`, `warning`, `info`), `code`, `file` relative to the project, and
`message`; the exit status is 1 when any error was reported.

A scenario `script` is a fixed input plan for regression tests: entries of
`frames` with `keys` and/or `actions` within the scenario's `controls`. Each
entry runs as one `run_until` on the scenario's success and failure
conditions. `expect` is `success` (default), `failure` or `truncated`.

The Lua checkers receive a temporary configuration whose `workspace.library`
is the generated `types/` directory. Neither checker is installed in the
development environment where this was written, so that path is untested;
`check` reports `lua_check_skipped` when none is found.
