#!/usr/bin/env python3
"""Actual supervisor/worker persistence across processes, with a temporary root."""
import copy
import json
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading

from agent_protocol_test import Agent, actor, check, properties, successful


class SaveAgent(Agent):
    def __init__(self, executable, project, save_root):
        self.process = subprocess.Popen(
            [str(executable), "--headless", "--project", str(project), "--agent",
             "--save-dir", str(save_root)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1,
        )
        self.lines = queue.Queue()
        self.errors = []
        threading.Thread(target=self._read_stdout, daemon=True).start()
        threading.Thread(target=self._read_stderr, daemon=True).start()


def stats(snapshot):
    return properties(snapshot, "farm", "FarmLogic")


def step(agent, count=1, keys=()):
    return successful(agent, {"cmd": "step", "frames": count, "keys": list(keys)})


def observe(agent):
    return successful(agent, {"cmd": "observe", "tilemaps": [
        {"actor_uid": 10, "component_key": "map", "layer": "ground",
         "cells": [[x, y] for y in range(8, 12) for x in range(8, 12)]},
        {"actor_uid": 10, "component_key": "map", "layer": "solid",
         "cells": [[16, y] for y in range(8, 12)]},
    ]})


def ground(snapshot, index=0):
    return snapshot["tilemaps"][0]["cells"][index]["tile"]


def walls(snapshot):
    return [cell["tile"] for cell in snapshot["tilemaps"][1]["cells"]]


def gameplay(snapshot):
    farm = stats(snapshot)
    return {
        "farm": {key: farm[key] for key in ["tick", "harvested", "supplies", "irrigation_unlocked",
                  "planted_count", "mature_count", "remaining_ticks", "walls_removed"]},
        "player": actor(snapshot, "player")["position"],
        "facing": {key: properties(snapshot, "player", "FarmMovement")[key]
                   for key in ["facing_x", "facing_y"]},
        "ground": [ground(snapshot, i) for i in range(16)], "walls": walls(snapshot),
    }


def save(agent):
    observed = step(agent, keys=["f5"])
    check(stats(observed)["save_status"] == "saved", "Save failed: " + str(stats(observed)))
    return observed


def load(agent):
    observed = step(agent, keys=["f9"])
    check(stats(observed)["load_status"] == "loaded", "Load failed: " + str(stats(observed)))
    return observed


def close(agent):
    try:
        # Preserve the original assertion if cleanup follows a failed request.
        if sys.exc_info()[0] is None:
            successful(agent, {"cmd": "quit"})
    finally:
        agent.close()


def run(runtime, project, save_root):
    path = save_root / "persistent_village/main.save.json"
    agent = SaveAgent(runtime, project, save_root)
    try:
        baseline = observe(agent)
        missing = step(agent, keys=["f9"])
        check(stats(missing)["load_status"] == "not_found", "missing slot did not report not_found")
        check(gameplay(observe(agent)) == gameplay(baseline), "missing save mutated live gameplay")
        planted = step(agent, keys=["e"])
        check(stats(planted)["remaining_ticks"] == 120, "baseline growth must last 120 ticks")
        growing = step(agent, 30)
        check(stats(growing)["remaining_ticks"] == 90, "growth clock mismatch before save")
        before = gameplay(observe(agent))
        save(agent)
        check(gameplay(observe(agent)) == before, "save input tick advanced or mutated gameplay")
        check(path.is_file(), "--save-dir namespace path was not honored")
        first = json.loads(path.read_text())
        check(first["format_version"] == first["schema_version"] == 1, "wrong save envelope")
        check(first["data"]["crops"][0]["stage"] == 3 and
              first["data"]["crops"][0]["remaining_ticks"] == 90, "save lost remaining growth time")
    finally:
        close(agent)

    # This is an entirely fresh supervisor plus worker, not just a new scene.
    agent = SaveAgent(runtime, project, save_root)
    try:
        check(ground(observe(agent)) == 2, "new process auto-loaded without explicit F9")
        restored = load(agent)
        check(stats(restored)["tick"] == 0 and stats(restored)["remaining_ticks"] == 90,
              "fresh process did not reconstruct relative growth deadline")
        check(ground(observe(agent)) == 3, "load did not restore the actual native tile")
        step(agent, 89)
        check(ground(observe(agent)) == 3 and stats(observe(agent))["remaining_ticks"] == 1,
              "restored crop matured too early")
        step(agent)
        check(ground(observe(agent)) == 4, "restored crop did not mature on its exact tick")
        harvested = step(agent, keys=["e"])
        check(stats(harvested)["harvested"] == 1 and stats(harvested)["supplies"] == 1,
              "restored crop was not actually harvestable")
        upgrade = step(agent, keys=["b"])
        check(stats(upgrade)["irrigation_unlocked"] and stats(upgrade)["supplies"] == 0,
              "pump purchase did not consume a real harvest")
        planted = step(agent, keys=["e"])
        check(stats(planted)["remaining_ticks"] == 60, "irrigation did not change real growth duration")
        step(agent, 59)
        check(ground(observe(agent)) == 3, "upgraded crop matured before 60 ticks")
        step(agent)
        check(ground(observe(agent)) == 4, "upgraded crop failed to mature")
        harvested = step(agent, keys=["e"])
        check(stats(harvested)["harvested"] == 2 and stats(harvested)["supplies"] == 1,
              "second real harvest did not update inventory")
        blocked = step(agent, 80, ["right"])
        check(7.5 < actor(blocked, "player")["position"]["x"] <= 7.80,
              "solid wall failed to block Box2D player")
        step(agent, keys=["space"])
        check(walls(observe(agent)) == [0, 5, 5, 5], "tool did not remove exactly one wall")
        passed = step(agent, 20, ["right"])
        check(actor(passed, "player")["position"]["x"] > 8.5,
              "removed wall still blocked the actual body")
        saved_player = actor(passed, "player")["position"].copy()
        save(agent)
        valid_bytes = path.read_bytes()
        check(json.loads(valid_bytes)["data"]["irrigation_unlocked"], "upgrade was not serialized")
        # Reset must keep the explicit save root in the newly spawned worker.
        reset = successful(agent, {"cmd": "reset"})
        check(not stats(reset)["irrigation_unlocked"] and stats(reset)["harvested"] == 0,
              "reset failed to reset runtime gameplay")
        check(path.read_bytes() == valid_bytes, "reset erased or rewrote the persistent slot")
        load(agent)
        recovered = observe(agent)
        check(walls(recovered) == [0, 5, 5, 5] and stats(recovered)["irrigation_unlocked"],
              "reset worker did not inherit save-dir and recover map/progression")
        check(abs(actor(recovered, "player")["position"]["x"] - saved_player["x"]) < 0.0001,
              "player checkpoint position not restored")
    finally:
        close(agent)

    agent = SaveAgent(runtime, project, save_root)
    try:
        load(agent)
        recovered = observe(agent)
        check(stats(recovered)["irrigation_unlocked"] and stats(recovered)["harvested"] == 2
              and stats(recovered)["supplies"] == 1 and walls(recovered) == [0, 5, 5, 5],
              "fresh process lost permanent upgrade, inventory or wall edit")
        x = actor(recovered, "player")["position"]["x"]
        returned = step(agent, round((x - 0.5) / 0.1), ["left"])
        check(abs(actor(returned, "player")["position"]["x"] - 0.5) < 0.08,
              "restored wall passage was not traversable back to the farm")
        planted = step(agent, keys=["e"])
        check(stats(planted)["remaining_ticks"] == 60, "permanent irrigation did not affect new-process planting")
        step(agent, 59)
        check(ground(observe(agent)) == 3, "persisted irrigation matured too early")
        step(agent)
        check(ground(observe(agent)) == 4, "persisted irrigation failed to mature in 60 ticks")
        # Save and reload a mature (not just growing) crop in another process below.
        save(agent)
        mature_bytes = path.read_bytes()
        valid = json.loads(mature_bytes)
        check(valid["data"]["crops"][0]["stage"] == 4, "mature crop not serialized")
        invalid = []
        invalid.append((b'{"format_version":1,', "invalid_data"))
        envelope = copy.deepcopy(valid); envelope["format_version"] = 99
        invalid.append((json.dumps(envelope).encode(), "unsupported_format"))
        envelope = copy.deepcopy(valid); envelope["schema_version"] = 99
        invalid.append((json.dumps(envelope).encode(), "unsupported_game_schema"))
        # Different valid early fields plus a late invalid field would reveal
        # any sequential apply-before-validate bug in the Lua game loader.
        envelope = copy.deepcopy(valid)
        envelope["data"]["harvested"] = 11; envelope["data"]["supplies"] = 10
        envelope["data"]["walls"] = [0, 0, 0, 0]
        envelope["data"]["player"]["facing_y"] = "invalid"
        invalid.append((json.dumps(envelope).encode(), "invalid_game_schema"))
        for mutate in [
            lambda d: d["crops"][15].update(stage=9),
            lambda d: d["crops"][0].update(remaining_ticks=1),
            lambda d: d["walls"].__setitem__(3, 7),
            lambda d: d["player"].update(x=1000),
            lambda d: d.update(supplies=500),
            lambda d: d.update(unknown=True),
            lambda d: d.pop("player"),
        ]:
            envelope = copy.deepcopy(valid); mutate(envelope["data"])
            invalid.append((json.dumps(envelope).encode(), "invalid_game_schema"))
        for payload, code in invalid:
            step(agent)  # Release F9; ordinary gameplay is allowed here.
            before = gameplay(observe(agent))
            path.write_bytes(payload)
            rejected = step(agent, keys=["f9"])
            check(stats(rejected)["load_status"] == code,
                  "wrong rejected-load status: " + str(stats(rejected)))
            check(gameplay(observe(agent)) == before, "invalid load partially mutated live state")
            check(path.read_bytes() == payload, "failed read overwrote the invalid slot")
        path.write_bytes(mature_bytes)
    finally:
        close(agent)

    agent = SaveAgent(runtime, project, save_root)
    try:
        load(agent)
        check(ground(observe(agent)) == 4 and stats(observe(agent))["mature_count"] == 1,
              "fresh process lost mature crop state")
        harvested = step(agent, keys=["e"])
        check(stats(harvested)["harvested"] == 3 and stats(harvested)["supplies"] == 2
              and ground(observe(agent)) == 2, "restored mature crop did not produce a real harvest")
    finally:
        close(agent)


if __name__ == "__main__":
    check(len(sys.argv) == 3, "usage: agent_savegame_test.py RUNTIME FIXTURE_PROJECT")
    runtime, project = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="pocket-savegame-") as temporary:
        run(runtime, project, Path(temporary))
    print("agent_savegame_test: fresh-process growth/harvest, permanent irrigation, wall passage, reset and atomic schema rejection passed")
