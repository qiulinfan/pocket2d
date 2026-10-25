#!/usr/bin/env python3
"""Exercise the real runtime's JSONL agent protocol with no third-party modules."""
import json
from pathlib import Path
import os
import queue
import subprocess
import sys
import threading


def check(condition, message):
    if not condition:
        raise AssertionError(message)


class Agent:
    def __init__(self, executable, project, extra_args=()):
        self.process = subprocess.Popen(
            [executable, "--headless", "--project", project, "--agent", *extra_args],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", bufsize=1,
        )
        self.lines = queue.Queue()
        self.errors = []
        threading.Thread(target=self._read_stdout, daemon=True).start()
        threading.Thread(target=self._read_stderr, daemon=True).start()

    def _read_stdout(self):
        for line in self.process.stdout:
            self.lines.put(line)
        self.lines.put(None)

    def _read_stderr(self):
        for line in self.process.stderr:
            self.errors.append(line)

    def request(self, request, raw=False):
        self.process.stdin.write((request if raw else json.dumps(request)) + "\n")
        self.process.stdin.flush()
        try:
            line = self.lines.get(timeout=15)
        except queue.Empty:
            raise AssertionError("agent response timed out; stderr: " + "".join(self.errors[-20:]))
        check(line is not None, "agent exited before responding: " + "".join(self.errors[-20:]))
        try:
            response = json.loads(line)
        except json.JSONDecodeError as error:
            raise AssertionError("stdout must contain one JSON object per request: " + line) from error
        check(isinstance(response, dict) and isinstance(response.get("ok"), bool),
              "response must contain boolean ok")
        return response

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)


def actor(snapshot, name):
    matches = [item for item in snapshot["actors"] if item["name"] == name]
    check(len(matches) == 1, "expected one actor named " + name)
    return matches[0]


def properties(snapshot, name, kind):
    matches = [item for item in actor(snapshot, name)["components"] if item["type"] == kind]
    check(len(matches) == 1, "expected component " + kind)
    return matches[0]["properties"]


def successful(agent, command):
    response = agent.request(command)
    check(response["ok"], str(response))
    return response


def run(executable, project):
    agent = Agent(executable, project)
    try:
        initial = successful(agent, {"cmd": "observe"})
        check(initial["frame"] == 0, "observe must not step")
        description = successful(agent, {"cmd": "describe"})
        expected_api = json.loads((Path(__file__).resolve().parents[2] / "api/host-api.json").read_text())
        check(description.get("script_api") == expected_api, "agent discovery must expose the generated curated script contract")
        types = {entry["type"]: entry for entry in description["component_types"]}
        check("Rigidbody" in types and "Village" in types,
              "agent discovery must include native and loaded project components")
        check(any(p["name"] == "health" and p["value_type"] in ("integer", "number")
                  for p in types["Village"]["properties"]),
              "project property metadata must be discoverable without source scraping")
        uid = actor(initial, "player")["uid"]
        check(isinstance(uid, str) and uid.isdecimal(),
              "actor UID must be lossless for JavaScript/TypeScript clients")
        observed_again = successful(agent, {"cmd": "observe"})
        for observation in (initial, observed_again):
            check(isinstance(observation.get("diagnostics"), list) and
                  isinstance(observation.get("diagnostics_dropped"), int),
                  "observations must report new engine diagnostics")
        check(observed_again["diagnostics"] == [] and observed_again["diagnostics_dropped"] == 0,
              "diagnostics are reported once")
        strip = lambda o: {k: v for k, v in o.items() if k not in ("diagnostics", "diagnostics_dropped")}
        check(strip(initial) == strip(observed_again), "observation must not mutate simulation")

        # Failures must be one response each and leave the process usable.
        for bad, raw in [("{bad json", True), ({"cmd": "unknown"}, False),
                         ({"cmd": "step", "frames": -1}, False)]:
            response = agent.request(bad, raw=raw)
            check(not response["ok"] and isinstance(response.get("error"), str),
                  "invalid command should return a structured error")
            check(successful(agent, {"cmd": "observe"})["frame"] == 0,
                  "rejected request must not advance time")

        moved = successful(agent, {"cmd": "step", "frames": 40, "keys": ["right"]})
        check(moved["frame"] == 40, "step must advance requested frames")
        check(actor(moved, "player")["uid"] == uid, "actor identity must be stable")
        control = properties(moved, "player", "Defender")
        check(control["presses"] == 1 and control["held_ticks"] == 40,
              "batched step must preserve held input and one down edge")
        check(properties(moved, "village", "Village")["gathered"] == 1,
              "real trigger pickup must be observable")

        held = successful(agent, {"cmd": "step", "frames": 1})
        check(properties(held, "player", "Defender")["held_ticks"] == 41,
              "omitted keys must preserve held set")
        stopped = successful(agent, {"cmd": "step", "frames": 1, "keys": []})
        check(properties(stopped, "player", "Defender")["releases"] == 1,
              "empty keys must release input")
        won = successful(agent, {"cmd": "step", "frames": 36, "keys": ["space"]})
        check(properties(won, "village", "Village")["state"] == "won",
              "agent actions must complete devil-tree victory")

        reset = successful(agent, {"cmd": "reset"})
        check(reset == initial, "reset must restore the complete initial observation")
        replay = successful(agent, {"cmd": "step", "frames": 40, "keys": ["right"]})
        check(replay == moved, "same commands after reset must reproduce observable state")
        successful(agent, {"cmd": "reset"})
        failed = successful(agent, {"cmd": "step", "frames": 1000, "keys": []})
        check(properties(failed, "village", "Village")["state"] == "lost",
              "unattended defense must fail through physics collision")
        successful(agent, {"cmd": "quit"})
        check(agent.process.wait(timeout=5) == 0, "quit must exit successfully")
        check(agent.lines.get(timeout=5) is None, "agent emitted unsolicited stdout")
    finally:
        agent.close()


if __name__ == "__main__":
    check(len(sys.argv) == 3, "usage: agent_protocol_test.py RUNTIME FIXTURE_PROJECT")
    run(os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2]))
    print("agent_protocol_test: JSONL, errors, input, physics, victory, reset and replay passed")
