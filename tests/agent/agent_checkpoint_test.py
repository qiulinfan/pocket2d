#!/usr/bin/env python3
"""checkpoint/restore: branch from a named state by deterministic replay."""
from pathlib import Path
import sys
import tempfile
from agent_protocol_test import check
from agent_savegame_test import SaveAgent


def state(response):
    return {key: value for key, value in response.items()
            if key not in ("id", "diagnostics", "diagnostics_dropped", "restored")}


def run(executable, project):
    with tempfile.TemporaryDirectory(prefix="pocket-checkpoint-") as saves:
        agent = SaveAgent(executable, project, saves)
        try:
            def ok(command):
                response = agent.request(command)
                check(response["ok"], "%s failed: %s" % (command["cmd"], response.get("error")))
                return response
            ok({"cmd": "step", "frames": 30, "keys": ["right"]})
            point = ok({"cmd": "checkpoint"})
            check(point["checkpoint"] == "cp-1" and point["frame"] == 30, "checkpoint id and frame")
            at_point = state(ok({"cmd": "observe"}))
            left = state(ok({"cmd": "step", "frames": 60, "keys": ["left", "up"]}))

            restored = ok({"cmd": "restore", "checkpoint": "cp-1"})
            check(restored["restored"] == {"checkpoint": "cp-1", "replayed": 1}, "restore reports the replay")
            check(state(restored) == at_point, "restore reproduces the checkpointed observation")
            check(state(ok({"cmd": "step", "frames": 60, "keys": ["left", "up"]})) == left,
                  "the same branch is reproduced exactly")
            ok({"cmd": "restore", "checkpoint": "cp-1"})
            right = state(ok({"cmd": "step", "frames": 60, "keys": ["right", "down"]}))
            check(right != left and right["frame"] == left["frame"], "a second branch from the same checkpoint differs")

            nested = ok({"cmd": "checkpoint"})
            ok({"cmd": "step", "frames": 10})
            back = ok({"cmd": "restore", "checkpoint": nested["checkpoint"]})
            check(back["frame"] == 90 and back["restored"]["replayed"] == 2, "checkpoints after a restore replay the branch")

            missing = agent.request({"cmd": "restore", "checkpoint": "cp-999"})
            check(not missing["ok"] and missing["code"] == "invalid_request", "unknown checkpoints are rejected")
            ok({"cmd": "reload_scripts", "types": ["Village"]})
            refused = agent.request({"cmd": "checkpoint"})
            check(not refused["ok"] and refused["code"] == "checkpoint_unavailable", "script reloads end replayability")
            ok({"cmd": "reset", "seed": 3})
            check(ok({"cmd": "checkpoint"})["frame"] == 0, "reset makes checkpoints available again")
            ok({"cmd": "quit"})
        finally:
            agent.close()
    print("agent checkpoint/restore regressions passed")


if __name__ == "__main__":
    check(len(sys.argv) == 3, "usage: agent_checkpoint_test.py RUNTIME FIXTURE_PROJECT")
    run(str(Path(sys.argv[1]).resolve()), str(Path(sys.argv[2]).resolve()))
