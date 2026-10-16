#!/usr/bin/env python3
"""Camera screen/world conversion matches the rendered view (headless: the
screen is the configured resolution)."""
import json
from pathlib import Path
import sys
import tempfile
from agent_protocol_test import Agent, check, successful


def values(snapshot):
    probe = next(a for a in snapshot["actors"] if a["name"] == "probe")
    return next(c["properties"] for c in probe["components"] if c["type"] == "CameraProbe")


def run(executable):
    with tempfile.TemporaryDirectory(prefix="pocket-camera-") as directory:
        root = Path(directory)
        (root / "scenes").mkdir()
        (root / "component_types").mkdir()
        (root / "game.config").write_text('{"initial_scene":"main"}')
        (root / "rendering.config").write_text('{"x_resolution":200,"y_resolution":100,"zoom_factor":0.5}')
        (root / "scenes/main.scene").write_text(json.dumps({"actors": [
            {"name": "probe", "uid": 1, "components": {"probe": {"type": "CameraProbe"}}}]}))
        (root / "component_types/CameraProbe.lua").write_text('''CameraProbe = {
          wx = 0, wy = 0, sx = 0, sy = 0, screen_w = 0, screen_h = 0, view_w = 0, view_h = 0,
          OnUpdate = function(self)
            Camera.SetPosition(3, 4)
            local m = Input.GetMousePosition()
            local w = Camera.ScreenToWorld(m.x, m.y); self.wx = w.x; self.wy = w.y
            local s = Camera.WorldToScreen(w.x, w.y); self.sx = s.x; self.sy = s.y
            local size = Camera.GetScreenSize(); self.screen_w = size.x; self.screen_h = size.y
            local view = Camera.GetViewSize(); self.view_w = view.x; self.view_h = view.y
          end }''')
        agent = Agent(executable, str(root))
        try:
            p = values(successful(agent, {"cmd": "step", "frames": 2, "mouse": {"x": 150, "y": 25}}))
            # 50 px per unit at zoom 0.5: (150-100)/50 = 1 right of x=3; (25-50)/50 = 0.5 above y=4.
            check(abs(p["wx"] - 4.0) < 1e-4 and abs(p["wy"] - 3.5) < 1e-4, "screen to world: %r" % p)
            check(abs(p["sx"] - 150) < 1e-3 and abs(p["sy"] - 25) < 1e-3, "world to screen round trip")
            check((p["screen_w"], p["screen_h"], p["view_w"], p["view_h"]) == (200, 100, 4, 2),
                  "screen size is the configured resolution; view size in world units")
            successful(agent, {"cmd": "quit"})
            check(agent.process.wait(timeout=5) == 0, "clean quit")
        finally:
            agent.close()
    print("agent camera conversion passed")


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()))
