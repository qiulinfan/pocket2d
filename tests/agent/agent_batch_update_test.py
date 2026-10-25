#!/usr/bin/env python3
"""OnUpdateBatch: one call per type per frame, at the first instance's place."""
import json
from pathlib import Path
import sys
import tempfile
from agent_protocol_test import Agent, check, successful


def props(snapshot, name):
    actor = next(a for a in snapshot["actors"] if a["name"] == name)
    return actor["components"][0]["properties"]


def run(executable):
    with tempfile.TemporaryDirectory(prefix="pocket-batch-") as directory:
        root = Path(directory)
        (root / "scenes").mkdir()
        (root / "component_types").mkdir()
        (root / "game.config").write_text('{"initial_scene":"main"}')
        (root / "scenes/main.scene").write_text(json.dumps({"actors": [
            {"uid": 1, "name": "a1", "components": {"m": {"type": "Batched"}}},
            {"uid": 2, "name": "plain", "components": {"m": {"type": "Plain"}}},
            {"uid": 3, "name": "a2", "components": {"m": {"type": "Batched"}}},
            {"uid": 4, "name": "off", "components": {"m": {"type": "Batched", "enabled": False}}}]}))
        (root / "component_types/Batched.lua").write_text('''Batched = {
          ticks = 0, batches = 0,
          OnUpdateBatch = function(list)
            ORDER = (ORDER or "") .. "B" .. #list
            for _, c in ipairs(list) do c.ticks = c.ticks + 1; c.batches = c.batches + 1 end
          end }''')
        (root / "component_types/Plain.lua").write_text('''Plain = {
          ticks = 0, seen = "",
          OnUpdate = function(self) self.ticks = self.ticks + 1; self.seen = ORDER or "" end }''')
        agent = Agent(executable, str(root))
        try:
            snapshot = successful(agent, {"cmd": "step", "frames": 3})
            check(props(snapshot, "a1")["ticks"] == 3 and props(snapshot, "a2")["ticks"] == 3,
                  "every enabled instance updates once per frame")
            check(props(snapshot, "off")["ticks"] == 0, "disabled instances are not passed")
            check(props(snapshot, "plain")["seen"] == "B2B2B2",
                  "the batch runs once per frame before the later plain component: " + props(snapshot, "plain")["seen"])
            successful(agent, {"cmd": "quit"})
        finally:
            agent.close()
    print("OnUpdateBatch regressions passed")


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()))
