#!/usr/bin/env python3
"""--scene NAME starts a project in another scene than game.config's initial_scene,
also after a reset (a new worker), and a scene that does not exist is refused."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from agent_protocol_test import Agent, check, successful


def run(executable):
    with tempfile.TemporaryDirectory(prefix="pocket-start-scene-") as directory:
        root = Path(directory)
        (root / "scenes").mkdir()
        (root / "game.config").write_text('{"initial_scene":"main"}')
        for name in ("main", "lab"):
            (root / "scenes" / (name + ".scene")).write_text(json.dumps({"actors": [{"name": name, "uid": 1}]}))
        agent = Agent(executable, str(root), ["--scene", "lab"])
        try:
            observed = successful(agent, {"cmd": "observe"})
            check(observed["scene"] == "lab", "the run starts in the named scene: %s" % observed["scene"])
            check([a["name"] for a in observed["actors"]] == ["lab"], "with that scene's actors")
            successful(agent, {"cmd": "reset"})
            check(successful(agent, {"cmd": "observe"})["scene"] == "lab", "a reset starts there again")
            successful(agent, {"cmd": "quit"})
            check(agent.process.wait(timeout=5) == 0, "clean quit")
        finally:
            agent.close()
        default = Agent(executable, str(root))
        try:
            check(successful(default, {"cmd": "observe"})["scene"] == "main", "without --scene, initial_scene")
            successful(default, {"cmd": "quit"})
        finally:
            default.close()
        missing = subprocess.run([executable, "--project", str(root), "--scene", "nowhere", "--frames", "1"],
                                 capture_output=True, text=True, timeout=30)
        check(missing.returncode != 0 and "nowhere" in missing.stderr, "a missing scene is an error: " + missing.stderr)
    print("agent start scene passed")


if __name__ == "__main__":
    run(sys.argv[1])
