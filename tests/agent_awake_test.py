#!/usr/bin/env python3
"""OnAwake runs as soon as a component is published: Actor.Instantiate and
AddComponent return awake components, and scene components all wake before
any OnStart."""
import json
from pathlib import Path
import sys
import tempfile
from agent_protocol_test import Agent, check, successful


def properties(snapshot, name, component_type):
    actor = next(a for a in snapshot["actors"] if a["name"] == name)
    return next(c["properties"] for c in actor["components"] if c["type"] == component_type)


def run(executable):
    with tempfile.TemporaryDirectory(prefix="pocket-awake-") as directory:
        root = Path(directory)
        for folder in ("scenes", "component_types", "actor_templates"):
            (root / folder).mkdir()
        (root / "game.config").write_text('{"initial_scene":"main"}')
        (root / "scenes/main.scene").write_text(json.dumps({"actors": [
            {"name": "first", "uid": 1, "components": {"probe": {"type": "Reader"}}},
            {"name": "second", "uid": 2, "components": {"state": {"type": "Source"}}}]}))
        (root / "actor_templates/Child.template").write_text(json.dumps(
            {"name": "child", "components": {"state": {"type": "Source"}}}))
        (root / "component_types/Source.lua").write_text('''Source = {
          ready = 0, awakes = 0, started = false,
          OnAwake = function(self) self.ready = 42; self.awakes = self.awakes + 1 end,
          OnStart = function(self) self.started = true end }''')
        (root / "component_types/Reader.lua").write_text('''Reader = {
          scene_ready = -1, spawned_ready = -1, spawned_started = true, added_ready = -1,
          OnStart = function(self)
            -- uid 2 starts after uid 1, but every scene component woke first.
            self.scene_ready = Actor.Find("second"):GetComponent("Source").ready
          end,
          OnUpdate = function(self)
            if self.spawned_ready ~= -1 then return end
            local child = Actor.Instantiate("Child"):GetComponent("Source")
            self.spawned_ready, self.spawned_started = child.ready, child.started
            self.added_ready = self.actor:AddComponent("Source").ready
          end }''')
        agent = Agent(executable, str(root))
        try:
            snapshot = successful(agent, {"cmd": "step", "frames": 3})
            reader = properties(snapshot, "first", "Reader")
            check(reader["scene_ready"] == 42, "scene components wake before any OnStart: %r" % reader)
            check(reader["spawned_ready"] == 42 and reader["spawned_started"] is False,
                  "Instantiate returns an awake, not yet started component: %r" % reader)
            check(reader["added_ready"] == 42, "AddComponent returns an awake component: %r" % reader)
            child = properties(snapshot, "child", "Source")
            check(child["awakes"] == 1 and child["started"] is True, "OnAwake runs once, OnStart later: %r" % child)
            successful(agent, {"cmd": "quit"})
            check(agent.process.wait(timeout=5) == 0, "clean quit")
        finally:
            agent.close()
    print("agent OnAwake lifecycle passed")


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()))
