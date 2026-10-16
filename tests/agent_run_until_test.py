#!/usr/bin/env python3
"""Protocol v2: command schema discovery, request ids and run_until stop conditions."""
import json
from pathlib import Path
import sys
import tempfile
from agent_protocol_test import Agent, check, successful


def write_project(root):
    (root / "scenes").mkdir()
    (root / "component_types").mkdir()
    (root / "game.config").write_text('{"initial_scene":"main"}')
    (root / "scenes/main.scene").write_text(json.dumps({"actors": [
        {"name": "ticker", "uid": 1, "components": {
            "transform": {"type": "Transform"}, "logic": {"type": "Ticker"}}},
        {"name": "doomed", "uid": 2, "components": {"transform": {"type": "Transform"}}}]}))
    # Ticks count OnUpdate calls: tick n runs on the n-th simulated frame.
    (root / "component_types/Ticker.lua").write_text('''Ticker = {
      ticks = 0, phase = "start",
      OnUpdate = function(self)
        self.ticks = self.ticks + 1
        self.actor:GetComponent("Transform").x = self.ticks
        if self.ticks == 10 then Event.Publish("tick_10", {}) end
        if self.ticks == 15 then self.phase = "middle" end
        if self.ticks == 20 then Actor.Destroy(Actor.Find("doomed")) end
        if self.ticks == 90 then Application.Quit() end
      end }''')


def frame(response):
    return response["frame"]


def run(executable):
    with tempfile.TemporaryDirectory(prefix="pocket-run-until-") as directory:
        root = Path(directory)
        write_project(root)
        agent = Agent(executable, str(root))
        try:
            described = successful(agent, {"cmd": "describe", "id": "discover"})
            check(described["protocol"] == 2 and described.get("id") == "discover", "v2 describe echoes the id")
            schema = described["command_schema"]
            check([c["name"] for c in schema["commands"]] == described["commands"], "schema lists every command")
            check("run_until" in described["commands"], "run_until is advertised")
            run_schema = next(c for c in schema["commands"] if c["name"] == "run_until")
            check(run_schema["params"]["required"] == ["max_frames", "until"] and
                  "$ref" not in json.dumps(run_schema), "command schemas are self-contained")
            check(successful(agent, {"cmd": "observe", "id": 7})["id"] == 7, "integer ids are echoed")
            failed = agent.request({"cmd": "nonsense", "id": "x"})
            check(not failed["ok"] and failed["id"] == "x", "errors echo the id")

            for bad in [{}, {"max_frames": 0, "until": [{"event": "e"}]},
                        {"max_frames": 10, "until": []},
                        {"max_frames": 10, "until": [{"actor": "ticker", "property": "x", "op": "~", "value": 1}]},
                        {"max_frames": 10, "until": [{"actor": "ticker", "property": "x", "op": "<"}]},
                        {"max_frames": 10, "until": [{"nothing": 1}]}]:
                response = agent.request({"cmd": "run_until", **bad})
                check(not response["ok"] and response["code"] == "invalid_argument", "rejects " + json.dumps(bad))
                check(frame(successful(agent, {"cmd": "observe"})) == 0, "invalid run_until does not advance")

            result = successful(agent, {"cmd": "run_until", "max_frames": 100, "until": [{"event": "tick_10"}]})
            check(result["run_until"]["reason"] == "condition" and result["run_until"]["condition"] == 0,
                  "event condition matched")
            check(result["run_until"]["frames"] == 10 and result["run_until"]["event"]["type"] == "tick_10",
                  "stops on the tick that published the event: " + json.dumps(result["run_until"]))
            start = frame(result)

            result = successful(agent, {"cmd": "run_until", "max_frames": 100, "until": [
                {"actor": "ticker", "component": "logic", "property": "phase", "op": "==", "value": "middle"},
                {"actor": "1", "property": "x", "op": ">=", "value": 12}]})
            check(result["run_until"]["condition"] == 1 and result["run_until"]["frames"] == 2,
                  "position condition by UID wins first: " + json.dumps(result["run_until"]))
            result = successful(agent, {"cmd": "run_until", "max_frames": 100, "until": [
                {"actor": "ticker", "component": "logic", "property": "phase", "op": "==", "value": "middle"}]})
            check(frame(result) == start + 5, "string property condition")
            result = successful(agent, {"cmd": "run_until", "max_frames": 100,
                                        "until": [{"actor_exists": "doomed", "value": False}]})
            check(result["run_until"]["reason"] == "condition" and
                  not any(a["name"] == "doomed" for a in result["actors"]), "actor removal condition")
            result = successful(agent, {"cmd": "run_until", "max_frames": 5,
                                        "until": [{"actor": "missing", "property": "x", "op": ">", "value": 0}]})
            check(result["run_until"] == {"reason": "max_frames", "frames": 5, "condition": None},
                  "missing actors compare false until the frame limit")
            result = successful(agent, {"cmd": "run_until", "max_frames": 1000, "until": [{"event": "never"}]})
            check(result["run_until"]["reason"] == "stopped" and not result["running"],
                  "a quitting game stops the run: " + json.dumps(result["run_until"]))
            successful(agent, {"cmd": "quit"})
            check(agent.process.wait(timeout=5) == 0, "clean quit")
        finally:
            agent.close()
    print("agent protocol v2 run_until regressions passed")


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()))
