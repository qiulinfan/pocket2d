#!/usr/bin/env python3
"""Real JSONL tile queries and fixed-step farm/collision workflows."""
import json
from pathlib import Path
import shutil
import sys
import tempfile

from agent_protocol_test import Agent, check, successful, properties, actor


def query(agent, layer="ground", cells=None):
    return successful(agent, {"cmd": "observe", "tilemaps": [{
        "actor_uid": 10, "component_key": "map", "layer": layer,
        "cells": [[8, 8]] if cells is None else cells,
    }]})


def tile(snapshot, index=0):
    return snapshot["tilemaps"][0]["cells"][index]["tile"]


def step(agent, frames=1, keys=None):
    return successful(agent, {"cmd": "step", "frames": frames,
                               "keys": [] if keys is None else keys})


def farm_state(agent, frame, tile_id, harvested, planted, mature):
    observed = query(agent)
    check(observed["frame"] == frame, "tile observation advanced or lost simulation time")
    check(tile(observed) == tile_id, "wrong committed crop cell")
    stats = properties(observed, "farm", "FarmLogic")
    check((stats["tick"], stats["harvested"], stats["planted_count"], stats["mature_count"])
          == (frame, harvested, planted, mature), "incorrect farm state at frame " + str(frame))
    return observed


def test_farming(runtime, fixture):
    saved_bytes = (fixture / "maps/farm.tilemap.json").read_bytes()
    agent = Agent(str(runtime), str(fixture))
    try:
        initial = successful(agent, {"cmd": "observe"})
        first = query(agent, cells=[[8, 8], [-1, 0], [64, 64], [0, 0]])
        check(first["frame"] == 0 and [tile(first, i) for i in range(4)] == [2, None, None, 1],
              "frame-zero query must distinguish empty/outside coordinates")
        check(first == query(agent, cells=[[8, 8], [-1, 0], [64, 64], [0, 0]]),
              "observation mutated map state")
        for override in [
            {"layer": "missing"}, {"component_key": "missing"},
            {"cells": [[8.2, 8]]}, {"cells": [[8, 8]] * 257},
        ]:
            item = {"actor_uid": 10, "component_key": "map", "layer": "ground", "cells": [[8, 8]]}
            item.update(override)
            invalid = agent.request({"cmd": "observe", "tilemaps": [item]})
            check(invalid["ok"] is False, "invalid cell query accepted")
            check(query(agent)["frame"] == 0 and tile(query(agent)) == 2,
                  "rejected query mutated the runtime")
        item = {"actor_uid": 10, "component_key": "map", "layer": "ground", "cells": [[8, 8]]}
        rejected = agent.request({"cmd": "step", "keys": ["e"], "tilemaps": [dict(item, layer="missing")]})
        check(rejected["ok"] is False and query(agent)["frame"] == 0,
              "a bad tile query on step must reject before any tick")
        stepped = successful(agent, {"cmd": "step", "keys": ["e"], "tilemaps": [item]})
        check(stepped["frame"] == 1 and stepped["tilemaps"][0]["cells"][0]["tile"] == 3,
              "step reports tiles committed by its own ticks")
        farm_state(agent, 1, 3, 0, 1, 0)
        step(agent, 59, ["e"]); farm_state(agent, 60, 3, 0, 1, 0)
        step(agent); step(agent, 1, ["e"]); step(agent); farm_state(agent, 63, 3, 0, 1, 0)
        step(agent, 57); farm_state(agent, 120, 3, 0, 1, 0)
        step(agent); farm_state(agent, 121, 4, 0, 1, 1)
        step(agent, 1, ["e"]); farm_state(agent, 122, 2, 1, 0, 0)
        step(agent, 10, ["e"]); farm_state(agent, 132, 2, 1, 0, 0)
        step(agent); step(agent, 1, ["e"]); farm_state(agent, 134, 3, 1, 1, 0)
        step(agent, 40, ["right"]); step(agent, 1, ["e"])
        final = farm_state(agent, 175, 3, 1, 1, 0)
        check(abs(actor(final, "player")["position"]["x"] - 4.5) < 0.01,
              "real player did not reach the nonfarmable cell")
        check(tile(query(agent, cells=[[12, 8]])) == 1, "grass was incorrectly planted")
        reset = successful(agent, {"cmd": "reset"})
        check(reset == initial, "reset must restore initial actor/property state")
        check(tile(query(agent)) == 2, "reset retained gameplay tile changes")
        check((fixture / "maps/farm.tilemap.json").read_bytes() == saved_bytes,
              "runtime wrote gameplay delta into the authoring asset")
        successful(agent, {"cmd": "quit"})
    finally:
        agent.close()


def test_wall(runtime, fixture, collision_callback):
    with tempfile.TemporaryDirectory(prefix="pocket-agent-tilemap-") as directory:
        project = Path(directory) / "project"
        shutil.copytree(fixture, project)
        path = project / "scenes/farm.scene"
        scene = json.loads(path.read_text())
        player = next(a for a in scene["actors"] if a["name"] == "player")
        for key in ["body", "transform"]:
            player["components"][key].update(x=7.5, y=1.5)
        player["components"]["collision_probe"]["enabled"] = collision_callback
        path.write_text(json.dumps(scene))
        agent = Agent(str(runtime), str(project))
        try:
            if collision_callback:
                requested = None
                for _ in range(60):
                    observed = step(agent, 1, ["right"])
                    probe = properties(observed, "player", "CollisionEditProbe")
                    if probe["requests"]:
                        requested = probe
                        break
                check(requested is not None, "real contact callback never ran")
                check(requested["before"] == 5 and requested["during"] == 5,
                      "callback observed an uncommitted cell edit")
                check(tile(query(agent, "solid", [[16, 9]])) == 5,
                      "collision callback edit committed in the wrong frame")
                step(agent, 1, ["right"])
                check(tile(query(agent, "solid", [[16, 9]])) == 0,
                      "collision edit missing at next boundary")
            else:
                blocked = step(agent, 30, ["right"])
                check(7.5 < actor(blocked, "player")["position"]["x"] <= 7.80,
                      "solid map did not block real player body")
                step(agent, 1, ["space"])
                cells = query(agent, "solid", [[16, 8], [16, 9], [16, 10]])
                check([tile(cells, i) for i in range(3)] == [5, 0, 5],
                      "tool removed wrong wall cells")
                passed = step(agent, 30, ["right"])
                check(actor(passed, "player")["position"]["x"] > 8.5,
                      "removed wall cell still blocks physics")
            successful(agent, {"cmd": "quit"})
        finally:
            agent.close()


if __name__ == "__main__":
    check(len(sys.argv) == 3, "usage: agent_tilemap_test.py RUNTIME FIXTURE_PROJECT")
    executable, project = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    test_farming(executable, project)
    test_wall(executable, project, False)
    test_wall(executable, project, True)
    print("agent_tilemap_test: queries, 175-tick farm, wall, callback boundary and reset passed")
