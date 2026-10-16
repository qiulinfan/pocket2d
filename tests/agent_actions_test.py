#!/usr/bin/env python3
"""Semantic actions: project bindings, Lua reads and direct agent actions."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from agent_protocol_test import Agent, check, successful

ACTIONS = {"version": 1, "actions": {
    "attack": {"type": "button", "keys": ["space"], "mouse": [1]},
    "zoom": {"type": "axis", "negative": ["q"], "positive": ["e"]},
    "move": {"type": "axis2d", "left": ["a", "left"], "right": ["d"], "up": ["w"], "down": ["s"]}}}


def write_project(root, actions):
    (root / "scenes").mkdir()
    (root / "component_types").mkdir()
    (root / "game.config").write_text('{"initial_scene":"main"}')
    (root / "actions.json").write_text(json.dumps(actions))
    (root / "scenes/main.scene").write_text(json.dumps({"actors": [
        {"name": "probe", "uid": 1, "components": {"probe": {"type": "ActionProbe"}}}]}))
    (root / "component_types/ActionProbe.lua").write_text('''ActionProbe = {
      held = 0, pressed = 0, released = 0, zoom = 0, x = 0, y = 0, unknown_rejected = false,
      OnUpdate = function(self)
        if Input.GetAction("attack") then self.held = self.held + 1 end
        if Input.GetActionDown("attack") then self.pressed = self.pressed + 1 end
        if Input.GetActionUp("attack") then self.released = self.released + 1 end
        self.zoom = Input.GetActionValue("zoom")
        local move = Input.GetActionVector("move"); self.x = move.x; self.y = move.y
        self.unknown_rejected = not pcall(Input.GetAction, "nope")
      end }''')


def probe(response):
    actor = next(a for a in response["actors"] if a["name"] == "probe")
    return actor["components"][0]["properties"]


def run(executable):
    with tempfile.TemporaryDirectory(prefix="pocket-actions-") as directory:
        root = Path(directory)
        write_project(root, ACTIONS)
        agent = Agent(executable, str(root))
        try:
            described = successful(agent, {"cmd": "describe"})
            check(described["actions"] == [{"name": "attack", "type": "button"}, {"name": "move", "type": "axis2d"},
                                           {"name": "zoom", "type": "axis"}], "describe lists actions")
            p = probe(successful(agent, {"cmd": "step", "frames": 2, "keys": ["space", "e"]}))
            check((p["held"], p["pressed"], p["zoom"]) == (2, 1, 1) and p["unknown_rejected"],
                  "key bindings drive actions: " + json.dumps(p))
            p = probe(successful(agent, {"cmd": "step", "keys": [], "actions": {"attack": True, "zoom": -0.5}}))
            check((p["held"], p["pressed"], p["released"], p["zoom"]) == (3, 1, 0, -0.5),
                  "agent action continues a held button without a new edge: " + json.dumps(p))
            p = probe(successful(agent, {"cmd": "step", "frames": 2, "actions": {}}))
            check((p["held"], p["released"], p["zoom"]) == (3, 1, 0), "{} releases agent actions")
            p = probe(successful(agent, {"cmd": "step", "actions": {"attack": True, "move": [1, 0]}, "keys": ["s"]}))
            check(p["pressed"] == 2 and abs(p["x"] - 0.7071) < 1e-3 and abs(p["y"] - 0.7071) < 1e-3,
                  "agent and bound 2D input combine and clamp to unit length: " + json.dumps(p))
            p = probe(successful(agent, {"cmd": "step", "keys": []}))
            check(p["held"] == 5 and p["x"] == 1 and p["y"] == 0, "omitted actions are retained")
            frame = successful(agent, {"cmd": "observe"})["frame"]
            for bad in [{"nope": True}, {"attack": 1}, {"zoom": "x"}, {"move": [1]}, []]:
                response = agent.request({"cmd": "step", "actions": bad})
                check(not response["ok"] and response["code"] == "invalid_argument", "rejects " + json.dumps(bad))
            check(successful(agent, {"cmd": "observe"})["frame"] == frame, "invalid actions do not advance")
            p = probe(successful(agent, {"cmd": "reset"}))
            p = probe(successful(agent, {"cmd": "step"}))
            check(p["held"] == 0 and p["x"] == 0, "reset releases agent actions")
            successful(agent, {"cmd": "quit"})
        finally:
            agent.close()

    with tempfile.TemporaryDirectory(prefix="pocket-actions-bad-") as directory:
        root = Path(directory)
        write_project(root, {"version": 1, "actions": {"jump": {"type": "button", "keys": ["not-a-key"]}}})
        result = subprocess.run([executable, "--headless", "--agent", "--project", str(root)],
                                input='{"cmd":"quit"}\n', capture_output=True, text=True, timeout=30)
        response = json.loads(result.stdout.splitlines()[0])
        check(result.returncode == 1 and not response["ok"] and "unknown key name" in response["error"],
              "invalid actions.json fails startup: " + result.stdout)
    print("agent semantic action regressions passed")


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()))
