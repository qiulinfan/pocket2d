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
        Event.Publish("tick", self.ticks)
        if self.ticks == 10 then
          Event.Publish("tick_10", {count = self.ticks, at = Vector2(1.5, 2), who = self.actor,
            fn = function() end, list = {1, 2, 3}, deep = {a = {b = {c = {d = 1}}}}})
        end
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
            check("events" not in result, "run_until reports events only on request")
            successful(agent, {"cmd": "reset"})
            def selected(select, command="observe"):
                response = successful(agent, {"cmd": command, "select": select})
                return [a["name"] for a in response["actors"]], response["actors_matched"], response
            check(selected({"names": ["ticker"]})[:2] == (["ticker"], 1), "select by name")
            check(selected({"types": ["Ticker"]})[:2] == (["ticker"], 1), "select by component type")
            check(selected({"uids": ["2"]})[:2] == (["doomed"], 1), "select by uid")
            check(selected({"limit": 1})[:2] == (["ticker"], 2), "limit keeps the lowest uids")
            names, matched, response = selected({"region": {"min_x": 0.5, "min_y": -1, "max_x": 2, "max_y": 1},
                                                 "components": False}, "step")
            check(names == ["ticker"] and matched == 1 and "components" not in response["actors"][0] and
                  "position" in response["actors"][0], "region on a step response, components omitted")
            check("actors_matched" not in successful(agent, {"cmd": "observe"}), "unselected observations are unchanged")
            response = agent.request({"cmd": "observe", "select": {"uids": ["x"]}})
            check(not response["ok"] and response["code"] == "invalid_argument", "invalid selection")
            successful(agent, {"cmd": "reset"})
            stepped = successful(agent, {"cmd": "step", "frames": 12, "events": ["tick_10"]})
            check(stepped["events_dropped"] == 0 and len(stepped["events"]) == 1, "filtered event stream")
            event = stepped["events"][0]
            check(event["frame"] == 9 and event["type"] == "tick_10", "event frame is the frame being simulated")
            check(event["payload"] == {"count": 10, "at": {"x": 1.5, "y": 2.0}, "who": {"actor_uid": "1"},
                                       "fn": "<function>", "list": [1, 2, 3],
                                       "deep": {"a": {"b": {"c": "<table>"}}}},
                  "bounded raw payload: " + json.dumps(event["payload"]))
            stepped = successful(agent, {"cmd": "step", "frames": 3, "events": True})
            check([(e["type"], e["payload"]) for e in stepped["events"]] == [("tick", 13), ("tick", 14), ("tick", 15)],
                  "every event type in order")
            check("events" not in successful(agent, {"cmd": "step"}), "events are opt-in")
            result = successful(agent, {"cmd": "run_until", "max_frames": 5, "events": ["tick"],
                                        "until": [{"event": "never"}]})
            check(len(result["events"]) == 5, "run_until reports events too")
            response = agent.request({"cmd": "step", "events": [""]})
            check(not response["ok"] and response["code"] == "invalid_argument", "invalid event filters")
            successful(agent, {"cmd": "quit"})
            check(agent.process.wait(timeout=5) == 0, "clean quit")
        finally:
            agent.close()
    print("agent protocol v2 run_until regressions passed")


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()))
