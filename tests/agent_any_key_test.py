#!/usr/bin/env python3
"""Input.GetAnyKeyDown is true on the frame a key or mouse button goes down."""
import json
from pathlib import Path
import sys
import tempfile
from agent_protocol_test import Agent, check, successful


def presses(snapshot):
    actor = next(a for a in snapshot["actors"] if a["name"] == "probe")
    return next(c["properties"] for c in actor["components"] if c["type"] == "AnyKey")["presses"]


def run(executable):
    with tempfile.TemporaryDirectory(prefix="pocket-anykey-") as directory:
        root = Path(directory)
        for folder in ("scenes", "component_types"):
            (root / folder).mkdir()
        (root / "game.config").write_text('{"initial_scene":"main"}')
        (root / "scenes/main.scene").write_text(json.dumps({"actors": [
            {"name": "probe", "uid": 1, "components": {"k": {"type": "AnyKey"}}}]}))
        (root / "component_types/AnyKey.lua").write_text('''AnyKey = { presses = 0,
          OnUpdate = function(self) if Input.GetAnyKeyDown() then self.presses = self.presses + 1 end end }''')
        agent = Agent(executable, str(root))
        try:
            check(presses(successful(agent, {"cmd": "step", "frames": 2})) == 0, "nothing pressed yet")
            check(presses(successful(agent, {"cmd": "step", "frames": 3, "keys": ["e"]})) == 1,
                  "a key going down counts once while held")
            check(presses(successful(agent, {"cmd": "step", "frames": 2, "keys": []})) == 1, "releasing is not a press")
            check(presses(successful(agent, {"cmd": "step", "frames": 2, "mouse": {"x": 5, "y": 5, "buttons": [1]}})) == 2,
                  "a mouse button counts")
            successful(agent, {"cmd": "quit"})
            check(agent.process.wait(timeout=5) == 0, "clean quit")
        finally:
            agent.close()
    print("agent any key passed")


if __name__ == "__main__":
    run(sys.argv[1])
