#!/usr/bin/env python3
"""Mouse actions run through native input and actual Lua callbacks."""
import json
from pathlib import Path
import sys
import tempfile
from agent_protocol_test import Agent, check, successful


def values(snapshot):
    probe = next(a for a in snapshot["actors"] if a["name"] == "probe")
    return next(c["properties"] for c in probe["components"] if c["type"] == "MouseProbe")


def run(executable):
    with tempfile.TemporaryDirectory(prefix="pocket-mouse-") as directory:
        root = Path(directory)
        (root / "scenes").mkdir()
        (root / "component_types").mkdir()
        (root / "game.config").write_text('{"initial_scene":"main"}')
        (root / "scenes/main.scene").write_text(json.dumps({"actors": [
            {"name": "probe", "uid": 1, "components": {"probe": {"type": "MouseProbe"}}}]}))
        (root / "component_types/MouseProbe.lua").write_text('''MouseProbe = {
          x = 0, y = 0, down = 0, held = 0, up = 0, scroll = 0, scroll_frames = 0,
          OnUpdate = function(self)
            local p = Input.GetMousePosition(); self.x = p.x; self.y = p.y
            if Input.GetMouseButtonDown(1) then self.down = self.down + 1 end
            if Input.GetMouseButton(1) then self.held = self.held + 1 end
            if Input.GetMouseButtonUp(1) then self.up = self.up + 1 end
            local d = Input.GetMouseScrollDelta(); self.scroll = self.scroll + d
            if d ~= 0 then self.scroll_frames = self.scroll_frames + 1 end
          end }''')
        agent = Agent(executable, str(root))
        try:
            for mouse in [[], {"x": "bad"}, {"buttons": [0]}, {"buttons": [1.5]}, {"wheel": 1e100}]:
                check(not agent.request({"cmd": "step", "keys": ["right"], "mouse": mouse})["ok"],
                      "invalid mouse request rejected")
                check(successful(agent, {"cmd": "observe"})["frame"] == 0, "invalid action is atomic")
            p = values(successful(agent, {"cmd": "step", "frames": 3,
                      "mouse": {"x": 10.5, "y": -3.25, "buttons": [1], "wheel": -2.5}}))
            check((p["x"], p["y"], p["down"], p["held"], p["up"], p["scroll"], p["scroll_frames"])
                  == (10.5, -3.25, 1, 3, 0, -2.5, 1), "mouse edges, coordinates, held and wheel")
            p = values(successful(agent, {"cmd": "step", "frames": 2}))
            check(p["held"] == 5 and p["down"] == 1 and p["scroll_frames"] == 1, "omitted mouse retains held only")
            p = values(successful(agent, {"cmd": "step", "mouse": {"buttons": []}}))
            check(p["up"] == 1 and p["held"] == 5, "release delivered once")
            successful(agent, {"cmd": "reset"})
            p = values(successful(agent, {"cmd": "step"}))
            check(p["held"] == 0 and p["down"] == 0 and p["up"] == 0 and p["scroll"] == 0, "reset clears mouse")
            successful(agent, {"cmd": "quit"})
            check(agent.process.wait(timeout=5) == 0, "clean quit")
        finally:
            agent.close()
    print("agent mouse action regressions passed")


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()))
