#!/usr/bin/env python3
"""call: game-declared agent commands run at idle, ordered with steps and replayed by checkpoints."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "clients/python"))
from pocket2d import ProtocolError, Runtime  # noqa: E402


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def write_project(root):
    (root / "scenes").mkdir()
    (root / "component_types").mkdir()
    (root / "game.config").write_text('{"initial_scene":"main"}')
    (root / "scenes/main.scene").write_text(json.dumps({"actors": [
        {"name": "bank", "uid": 1, "components": {
            "transform": {"type": "Transform"}, "logic": {"type": "Bank"}}},
        {"name": "echo", "uid": 2, "components": {"b": {"type": "Echo"}, "a": {"type": "Echo"}}},
        {"name": "plain", "uid": 3, "components": {"transform": {"type": "Transform"}}}]}))
    # OnUpdate adds the rate each tick, so a call's effect is visible from the
    # next tick on; commands change the rate, the balance or raise.
    (root / "component_types/Bank.lua").write_text('''Bank = {
      balance = 0, rate = 0, ticks = 0,
      OnUpdate = function(self)
        self.ticks = self.ticks + 1
        self.balance = self.balance + self.rate
      end,
      OnAgentCommand = function(self, name, args)
        if name == "deposit" then
          self.balance = self.balance + args.amount
          Event.Publish("deposited", {amount = args.amount})
          return {balance = self.balance, integer = math.type(args.amount) == "integer"}
        elseif name == "rate" then
          self.rate = args.rate
          return self.rate
        elseif name == "ping" then
          return "bank"
        elseif name == "fail" then
          self.balance = self.balance + 1000
          error("refused")
        end
        error("unknown command " .. name)
      end }''')
    # Echo shares broadcast names with Bank, so it returns nil for the others.
    (root / "component_types/Echo.lua").write_text('''Echo = {
      OnAgentCommand = function(self, name, args)
        if name == "ping" then return self.key end
        if name ~= "echo" then return nil end
        return {key = self.key, list = args.list, nested = args.nested, gone = args.gone == nil, n = #args.list}
      end }''')


def property_of(response, name, key, prop):
    actor = next(a for a in response["actors"] if a["name"] == name)
    component = next(c for c in actor["components"] if c["key"] == key)
    return component["properties"][prop]


def state(response):
    return {key: value for key, value in response.items()
            if key not in ("id", "diagnostics", "diagnostics_dropped", "restored", "call")}


def run(executable):
    with tempfile.TemporaryDirectory(prefix="pocket-call-") as directory:
        root = Path(directory)
        write_project(root)
        with Runtime(executable, root, seed=7) as runtime:
            described = runtime.request({"cmd": "describe"})
            schema = next(c for c in described["command_schema"]["commands"] if c["name"] == "call")
            check(schema["access"] == "simulate" and schema["params"]["required"] == ["name"],
                  "call is advertised as a simulate command")

            for bad, code in [({}, "invalid_argument"), ({"name": ""}, "invalid_argument"),
                              ({"name": "deposit", "args": [1]}, "invalid_argument"),
                              ({"name": "deposit", "actor": "nobody"}, "invalid_argument"),
                              ({"name": "deposit", "actor": "plain"}, "no_command_handler"),
                              ({"name": "deposit", "component": "missing"}, "no_command_handler")]:
                response = runtime.request({"cmd": "call", **bad}, check=False)
                check(not response["ok"] and response["code"] == code, "rejects %s: %s" % (bad, response))
            check(property_of(runtime.request({"cmd": "observe"}), "bank", "logic", "balance") == 0,
                  "rejected calls run no handler")

            deposited = runtime.call("deposit", {"amount": 5}, actor="bank", events=True)
            check(deposited["frame"] == 0, "call does not advance time")
            check(deposited["call"] == {"name": "deposit", "results": [
                {"actor_uid": "1", "key": "logic", "value": {"balance": 5, "integer": True}}]},
                  "the handler's return value is reported: " + json.dumps(deposited["call"]))
            check(property_of(deposited, "bank", "logic", "balance") == 5, "the observation shows the effect")
            check([e["type"] for e in deposited["events"]] == ["deposited"] and
                  deposited["events"][0]["payload"] == {"amount": 5}, "events published by the handler are reported")

            pinged = runtime.call("ping")
            check([(r["actor_uid"], r["key"], r["value"]) for r in pinged["call"]["results"]] ==
                  [("1", "logic", "bank"), ("2", "a", "a"), ("2", "b", "b")],
                  "without a target every handler runs in UID then key order")
            echoed = runtime.call("echo", {"list": [1, 2.5, "three", True], "nested": {"deep": [{"x": 1}]},
                                           "gone": None}, actor="echo")
            check(echoed["call"]["results"][0]["value"] == {
                "key": "a", "list": [1, 2.5, "three", True], "nested": {"deep": [{"x": 1}]}, "gone": True, "n": 4},
                  "JSON args become Lua tables, null members are absent: " + json.dumps(echoed["call"]))
            only = runtime.call("echo", {"list": []}, actor="2", component="b")
            check([r["key"] for r in only["call"]["results"]] == ["b"], "actor and component narrow the targets")

            try:
                runtime.call("withdraw", actor="bank")
                check(False, "an unknown command raises")
            except ProtocolError as error:
                failed = error.response
            check(failed["code"] == "agent_command_failed" and "unknown command withdraw" in failed["error"] and
                  failed["call"]["results"][0]["value"] is None and "error" in failed["call"]["results"][0],
                  "a handler error is reported with its results: " + json.dumps(failed))

            # Ordering: the rate set between steps applies from the next tick.
            runtime.request({"cmd": "step", "frames": 3})
            runtime.call("rate", {"rate": 2}, actor="bank")
            stepped = runtime.request({"cmd": "step", "frames": 4})
            check(property_of(stepped, "bank", "logic", "balance") == 13, "the call is ordered between ticks")

            # A failing call keeps the effects it made before raising, and is
            # still replayed by checkpoints.
            runtime.call("fail", actor="bank", check=False)
            point = runtime.request({"cmd": "checkpoint"})
            at_point = state(runtime.request({"cmd": "observe"}))
            check(property_of(at_point, "bank", "logic", "balance") == 1013, "effects before an error are kept")
            runtime.call("deposit", {"amount": 100}, actor="bank")
            later = state(runtime.request({"cmd": "step", "frames": 10}))
            restored = runtime.request({"cmd": "restore", "checkpoint": point["checkpoint"]})
            check(restored["restored"]["replayed"] == 9 and state(restored) == at_point,
                  "restore replays calls with steps: " + json.dumps(restored["restored"]))
            runtime.call("deposit", {"amount": 100}, actor="bank")
            check(state(runtime.request({"cmd": "step", "frames": 10})) == later, "the branch is reproduced exactly")

        replay = root / "session.jsonl"
        requests = [{"cmd": "step", "frames": 2}, {"cmd": "call", "name": "rate", "args": {"rate": 3}, "actor": "bank"},
                    {"cmd": "call", "name": "fail", "actor": "bank"}, {"cmd": "step", "frames": 5},
                    {"cmd": "observe"}, {"cmd": "quit"}]
        recorded = subprocess.run([executable, "--headless", "--agent", "--project", str(root), "--seed", "7",
                                   "--record", str(replay)], input="".join(json.dumps(r) + "\n" for r in requests),
                                  capture_output=True, text=True, timeout=60)
        check(recorded.returncode == 0, "recording failed: " + recorded.stderr[-2000:])
        lines = [json.loads(line) for line in replay.read_text().splitlines()]
        check(sum(1 for line in lines if line.get("request", {}).get("cmd") == "call") == 2,
              "calls are recorded in replays")
        result = subprocess.run([executable, "--headless", "--replay", str(replay), "--project", str(root)],
                                capture_output=True, text=True, timeout=60)
        summary = json.loads(result.stdout.strip().splitlines()[-1])
        check(result.returncode == 0 and summary["ok"] and summary["frame"] == 7,
              "the replay with calls matches: " + result.stdout + result.stderr[-2000:])
    print("agent call regressions passed")


if __name__ == "__main__":
    check(len(sys.argv) == 2, "usage: agent_call_test.py RUNTIME")
    run(str(Path(sys.argv[1]).resolve()))
