"""pocket: a headless command line for Pocket2D projects.

  pocket check PROJECT             validate assets, start the runtime, type-check Lua
  pocket test PROJECT              run scenario scripts and replay files
  pocket run PROJECT [--mcp]       serve the agent protocol (JSONL or MCP) on stdio
  pocket replay PROJECT REPLAY     verify a recorded replay
  pocket render PROJECT --out PNG  render a frame headlessly
  pocket new DIRECTORY             create a minimal project

Diagnostics are text by default; --json prints one JSON object per line with
severity, code, file and message. The runtime binary comes from --game, the
POCKET_GAME environment variable, or build/sdl3-{release,debug}/game.
"""
import argparse
import base64
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from .runtime import ProtocolError, Runtime
from .scenario import Scenario, ScenarioError

REPOSITORY = Path(__file__).resolve().parents[3]
ASSET_PATTERNS = [
    ("game.config", "game-config"), ("rendering.config", "rendering-config"), ("actions.json", "actions"),
    ("scenes/**/*.scene", "scene"), ("actor_templates/**/*.template", "actor-template"),
    ("maps/**/*.tilemap.json", "tilemap"), ("animations/**/*.animation.json", "animation"),
    ("items/**/*.items.json", "items"), ("scenarios/**/*.scenario.json", "scenario"),
]


class Report:
    def __init__(self, as_json):
        self.as_json = as_json
        self.errors = 0

    def emit(self, severity, code, message, file=None):
        if severity == "error":
            self.errors += 1
        if self.as_json:
            print(json.dumps({"severity": severity, "code": code, "file": str(file) if file else None,
                              "message": message}))
        else:
            print("%s: %s%s [%s]" % (severity, (str(file) + ": ") if file else "", message, code))


def find_game(explicit):
    for candidate in [explicit, os.environ.get("POCKET_GAME"),
                      REPOSITORY / "build/sdl3-release/game", REPOSITORY / "build/sdl3-debug/game"]:
        if candidate and Path(candidate).is_file():
            return str(Path(candidate).resolve())
    raise SystemExit("pocket: no runtime binary; build the project or pass --game PATH")


def project_path(text):
    path = Path(text).resolve()
    if not (path / "game.config").is_file():
        raise SystemExit("pocket: %s is not a project (missing game.config)" % path)
    return path


def load_validator():
    path = REPOSITORY / "tools/validate_schemas.py"
    if not path.is_file():
        return None
    spec = importlib.util.spec_from_file_location("pocket_validate_schemas", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check_assets(project, report):
    validator = load_validator()
    schemas = REPOSITORY / "scripting/api/schemas"
    for pattern, name in ASSET_PATTERNS:
        for path in sorted(project.glob(pattern)):
            try:
                data = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
                report.emit("error", "invalid_json", str(error), path.relative_to(project))
                continue
            schema_path = schemas / (name + ".schema.json")
            if validator is not None and schema_path.is_file():
                schema = json.loads(schema_path.read_text(encoding="utf-8"))
                for problem in validator.validate(data, schema):
                    report.emit("error", "schema", problem, path.relative_to(project))
            if name == "scenario":
                try:
                    Scenario.from_dict(data, path)
                except ScenarioError as error:
                    report.emit("error", "scenario", str(error), path.relative_to(project))


def check_runtime(game, project, report):
    with tempfile.TemporaryDirectory(prefix="pocket-check-") as saves:
        runtime = Runtime(game, project, save_dir=saves, timeout=30)
        try:
            described = runtime.request({"cmd": "describe"})
            observed = runtime.request({"cmd": "observe"})
            for record in observed.get("diagnostics", []):
                severity = "error" if record["level"] == "error" else "warning"
                report.emit(severity, "runtime_" + record["category"], record["message"])
            report.emit("info", "runtime", "scene '%s' loaded with %d actors and %d component types" %
                        (observed["scene"], len(observed["actors"]), len(described["component_types"])))
        except ProtocolError as error:
            report.emit("error", error.response.get("code", "runtime"), str(error))
        except (RuntimeError, TimeoutError) as error:
            report.emit("error", "runtime", str(error).strip())
        finally:
            runtime.close()


def check_lua(project, report):
    """Runs LuaLS or EmmyLua when installed, with the engine's generated host
    API declarations as a library. Neither checker is required."""
    if not any(project.glob("component_types/**/*.lua")):
        return
    lua_ls, emmylua = shutil.which("lua-language-server"), shutil.which("emmylua_check")
    if not lua_ls and not emmylua:
        report.emit("info", "lua_check_skipped",
                    "no lua-language-server or emmylua_check on PATH; Lua type checking skipped")
        return
    with tempfile.TemporaryDirectory(prefix="pocket-luacheck-") as directory:
        config = Path(directory) / "luarc.json"
        config.write_text(json.dumps({"runtime.version": "Lua 5.4",
                                      "workspace.library": [str(REPOSITORY / "types")]}), encoding="utf-8")
        if lua_ls:
            command = [lua_ls, "--check=" + str(project), "--checklevel=Warning", "--configpath=" + str(config)]
        else:
            command = [emmylua, str(project), "-c", str(config)]
        result = subprocess.run(command, capture_output=True, text=True)
    output = (result.stdout + result.stderr).strip()
    if result.returncode != 0:
        report.emit("error", "lua_check", output[-4000:] or "Lua checker failed")
    elif output:
        report.emit("info", "lua_check", output[-2000:])


def command_check(options):
    project = project_path(options.project)
    report = Report(options.json)
    check_assets(project, report)
    check_runtime(find_game(options.game), project, report)
    check_lua(project, report)
    if not options.json:
        print("%s: %d error(s)" % (project.name, report.errors))
    return 1 if report.errors else 0


def run_script(runtime, scenario):
    """Plays a scenario's fixed script; returns (outcome, frame)."""
    runtime.request({"cmd": "reset", "seed": scenario.seed})
    conditions = scenario.success + scenario.failure
    start = runtime.request({"cmd": "observe", "select": {"limit": 0}})["frame"]
    allowed_keys = set(scenario.controls.get("keys", []))
    allowed_actions = set(scenario.controls.get("actions", []))
    frame = start
    for entry in scenario.script:
        keys, actions = entry.get("keys", []), entry.get("actions", {})
        if set(keys) - allowed_keys or set(actions) - allowed_actions:
            raise ScenarioError("script uses inputs outside the scenario controls")
        remaining = scenario.max_frames - (frame - start)
        if remaining <= 0:
            return "truncated", frame
        command = {"cmd": "run_until", "max_frames": min(entry["frames"], remaining), "until": conditions,
                   "keys": keys, "select": {"limit": 0}}
        if allowed_actions:
            command["actions"] = actions
        result = runtime.request(command)
        frame = result["frame"]
        reason = result["run_until"]
        if reason["reason"] == "condition":
            return ("success" if reason["condition"] < len(scenario.success) else "failure"), frame
        if reason["reason"] == "stopped":
            return "failure", frame
    return "truncated", frame


def command_test(options):
    project = project_path(options.project)
    game = find_game(options.game)
    report = Report(options.json)
    ran = 0
    with tempfile.TemporaryDirectory(prefix="pocket-test-") as saves:
        for path in sorted(project.glob("scenarios/**/*.scenario.json")):
            try:
                scenario = Scenario.load(path)
            except (ScenarioError, json.JSONDecodeError) as error:
                report.emit("error", "scenario", str(error), path.relative_to(project))
                continue
            if scenario.script is None:
                continue
            runtime = Runtime(game, project, seed=scenario.seed, save_dir=saves, timeout=120)
            try:
                outcome, frame = run_script(runtime, scenario)
            except (ProtocolError, ScenarioError, RuntimeError, TimeoutError) as error:
                outcome, frame = "error: " + str(error), None
            finally:
                runtime.close()
            ran += 1
            if outcome == scenario.expect:
                report.emit("info", "scenario_passed", "%s: %s at frame %d" % (scenario.name, outcome, frame),
                            path.relative_to(project))
            else:
                report.emit("error", "scenario_failed", "%s: expected %s, got %s" % (scenario.name, scenario.expect, outcome),
                            path.relative_to(project))
        for path in sorted(project.glob("replays/**/*.jsonl")):
            result = subprocess.run([game, "--headless", "--replay", str(path), "--project", str(project),
                                     "--save-dir", saves], capture_output=True, text=True)
            ran += 1
            summary = result.stdout.strip().splitlines()[-1] if result.stdout.strip() else result.stderr.strip()
            if result.returncode == 0:
                report.emit("info", "replay_passed", summary, path.relative_to(project))
            else:
                report.emit("error", "replay_failed", summary, path.relative_to(project))
    if not options.json:
        print("%s: %d test(s), %d failure(s)" % (project.name, ran, report.errors))
    return 1 if report.errors else 0


def command_run(options):
    project = project_path(options.project)
    command = [find_game(options.game), "--headless", "--mcp" if options.mcp else "--agent",
               "--project", str(project), "--seed", str(options.seed)]
    if options.save_dir:
        command += ["--save-dir", options.save_dir]
    if options.record and not options.mcp:
        command += ["--record", options.record]
    os.execv(command[0], command)


def command_replay(options):
    project = project_path(options.project)
    command = [find_game(options.game), "--headless", "--replay", options.replay, "--project", str(project)]
    if options.save_dir:
        command += ["--save-dir", options.save_dir]
    return subprocess.run(command).returncode


def command_render(options):
    project = project_path(options.project)
    with tempfile.TemporaryDirectory(prefix="pocket-render-") as saves:
        with Runtime(find_game(options.game), project, seed=options.seed, save_dir=saves) as runtime:
            command = {"cmd": "observe", "image": {"width": options.width, "height": options.height}}
            if options.frames:
                command = {"cmd": "step", "frames": options.frames, "keys": options.keys,
                           "image": command["image"]}
            response = runtime.request(command)
    Path(options.out).write_bytes(base64.b64decode(response["image"]["data"]))
    print("%s: frame %d, %dx%d" % (options.out, response["frame"], options.width, options.height))
    return 0


TEMPLATE = {
    "game.config": {"game_title": "New Pocket2D Game", "initial_scene": "main"},
    "rendering.config": {"x_resolution": 640, "y_resolution": 360},
    "actions.json": {"version": 1, "actions": {
        "move": {"type": "axis2d", "left": ["a", "left"], "right": ["d", "right"], "up": ["w", "up"], "down": ["s", "down"]}}},
    "scenes/main.scene": {"actors": [{"uid": 1, "name": "player", "components": {
        "transform": {"type": "Transform"}, "mover": {"type": "Mover"}}}]},
    "scenarios/move_right.scenario.json": {
        "version": 1, "name": "move_right", "description": "The player reaches x >= 2 by moving right.",
        "seed": 1, "max_frames": 600, "frames_per_step": 4, "controls": {"actions": ["move"]},
        "success": [{"actor": "player", "property": "x", "op": ">=", "value": 2}],
        "reward": [{"actor": "player", "property": "x", "weight": 1.0}],
        "script": [{"frames": 120, "actions": {"move": [1, 0]}}]},
}
MOVER = """-- A minimal component: moves its actor by the "move" action.
Mover = {
    speed = 2.0,
    OnUpdate = function(self)
        local move = Input.GetActionVector("move")
        local transform = self.actor:GetComponent("Transform")
        transform.x = transform.x + move.x * self.speed / 60
        transform.y = transform.y + move.y * self.speed / 60
    end
}
"""
AGENTS = """# Agent guide for this Pocket2D project

- Gameplay is Lua: component types live in `component_types/` (one global
  table per file, named like the file). Host API declarations for LuaLS are in
  the engine's `types/pocket2d.lua`.
- Scenes are JSON under `scenes/`; inputs are semantic actions in
  `actions.json`; scenarios under `scenarios/` define tasks and regression
  scripts.
- Validate with `pocket check .`, run scenario scripts and replays with
  `pocket test .`, and drive the game with `pocket run .` (JSONL) or
  `pocket run . --mcp`.
"""


def command_new(options):
    root = Path(options.directory)
    if root.exists() and any(root.iterdir()):
        raise SystemExit("pocket: %s exists and is not empty" % root)
    for relative, content in TEMPLATE.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(content, indent=2) + "\n", encoding="utf-8")
    (root / "component_types").mkdir(exist_ok=True)
    (root / "component_types/Mover.lua").write_text(MOVER, encoding="utf-8")
    (root / "AGENTS.md").write_text(AGENTS, encoding="utf-8")
    print("created %s" % root)
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(prog="pocket", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", help="runtime binary (default: POCKET_GAME or build/sdl3-*/game)")
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser("check", help="validate a project")
    check.add_argument("project")
    check.add_argument("--json", action="store_true")
    check.set_defaults(handler=command_check)
    test = commands.add_parser("test", help="run scenario scripts and replays")
    test.add_argument("project")
    test.add_argument("--json", action="store_true")
    test.set_defaults(handler=command_test)
    run = commands.add_parser("run", help="serve the agent protocol on stdio")
    run.add_argument("project")
    run.add_argument("--mcp", action="store_true")
    run.add_argument("--seed", type=int, default=0)
    run.add_argument("--save-dir")
    run.add_argument("--record")
    run.set_defaults(handler=command_run)
    replay = commands.add_parser("replay", help="verify a replay file")
    replay.add_argument("project")
    replay.add_argument("replay")
    replay.add_argument("--save-dir")
    replay.set_defaults(handler=command_replay)
    render = commands.add_parser("render", help="render a frame to PNG headlessly")
    render.add_argument("project")
    render.add_argument("--out", required=True)
    render.add_argument("--frames", type=int, default=0)
    render.add_argument("--keys", nargs="*", default=[])
    render.add_argument("--width", type=int, default=640)
    render.add_argument("--height", type=int, default=360)
    render.add_argument("--seed", type=int, default=0)
    render.set_defaults(handler=command_render)
    new = commands.add_parser("new", help="create a minimal project")
    new.add_argument("directory")
    new.set_defaults(handler=command_new)
    options = parser.parse_args(argv)
    return options.handler(options)
