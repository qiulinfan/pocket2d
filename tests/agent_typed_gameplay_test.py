#!/usr/bin/env python3
"""Replay real engine inputs against handwritten Lua and compiled TypeScript."""
import json
from pathlib import Path
import shutil
import sys
import tempfile

from agent_protocol_test import actor, check, properties, successful
from agent_savegame_test import SaveAgent


def controller(snapshot):
    return properties(snapshot, "player", "TypedController")


def animation(snapshot):
    return properties(snapshot, "player", "SpriteAnimation")


def position(snapshot):
    return actor(snapshot, "player")["position"]


class Comparison:
    def __init__(self, runtime, lua_project, ts_project, root):
        self.projects = [lua_project, ts_project]
        self.roots = [root / "lua-saves", root / "typescript-saves"]
        self.paths = [directory / "typed_gameplay/main.save.json" for directory in self.roots]
        self.runtime = runtime
        self.agents = []
        self.comparisons = 0
        self.start()

    def start(self):
        self.agents = [SaveAgent(self.runtime, project, root)
                       for project, root in zip(self.projects, self.roots)]

    def close(self):
        for agent in self.agents:
            agent.close()
        self.agents = []

    def request(self, command, label):
        responses = [successful(agent, command) for agent in self.agents]
        check(responses[0] == responses[1],
              f"Lua/TypeScript diverged at {label}:\nLua={responses[0]}\nTS={responses[1]}")
        self.comparisons += 1
        return responses[0]

    def observe(self):
        return self.request({"cmd": "observe"}, "observe")

    def step(self, count=1, keys=()):
        # Compare every real tick rather than letting divergent intermediate
        # states accidentally converge at a batched final checkpoint.
        for _ in range(count):
            observed = self.request({"cmd": "step", "frames": 1, "keys": list(keys)},
                                    "step " + str(list(keys)))
        return observed

    def saved_bytes(self):
        check(all(path.is_file() for path in self.paths), "save root/namespace path missing")
        values = [path.read_bytes() for path in self.paths]
        check(values[0] == values[1], "same gameplay state serialized differently")
        return values[0]

    def replace_saves(self, payload):
        for path in self.paths:
            path.write_bytes(payload)


def run(comparison):
    c = comparison
    initial = c.observe()
    check(initial["frame"] == 0 and controller(initial)["health"] == 3, "wrong authored initial state")
    check(c.observe() == initial, "observation changed simulation")
    started = c.step()
    check(controller(started)["ready"] and controller(started)["frame_events"] == 1,
          "native actor/component lookup or initial event failed")
    walking = c.step(6, ["right"])
    check(position(walking)["x"] > 0 and controller(walking)["mode"] == "walk", "actual movement failed")
    diagonal = c.step(3, ["right", "down"])
    check(position(diagonal)["x"] > position(walking)["x"] and position(diagonal)["y"] > 0,
          "diagonal movement failed")
    reversed_pose = c.step(4, ["left"])
    check(controller(reversed_pose)["facing"] == -1 and
          properties(reversed_pose, "player", "SpriteRenderer")["scale_x"] == -5,
          "direction change did not mirror the native sprite")
    attacked = c.step(10, ["space"])
    counts = controller(attacked)
    check((counts["attacks"], counts["hit_events"], counts["completed_events"]) == (1, 1, 1)
          and counts["mode"] == "idle", "held Space restarted attack or lost completion")
    c.step();c.step(keys=["space"])
    twice = c.step(3)
    check((controller(twice)["attacks"], controller(twice)["hit_events"],
           controller(twice)["completed_events"]) == (2, 2, 2), "second input edge did not attack once")
    paused = c.step(keys=["right", "p"])
    pose = position(paused).copy()
    anim = animation(paused)
    frozen = c.step(10, ["right", "space"])
    check(position(frozen) == pose and controller(frozen)["attacks"] == 2 and
          animation(frozen)["frame"] == anim["frame"] and
          animation(frozen)["elapsed_ticks"] == anim["elapsed_ticks"] and
          not animation(frozen)["playing"], "pause failed to freeze body, attack and animation")
    resumed = c.step(keys=["right", "r"])
    check(position(resumed)["x"] > pose["x"] and animation(resumed)["playing"], "resume failed")
    damaged = c.step(8, ["h"])
    check(controller(damaged)["health"] == 2 and controller(damaged)["damage_events"] == 1,
          "held damage key applied multiple hits")
    healed = c.step(8, ["g"])
    check(controller(healed)["health"] == 3 and controller(healed)["heal_events"] == 1,
          "healing exceeded maximum health or repeated while held")
    c.step();full = c.step(keys=["g"])
    check(controller(full)["health"] == 3 and controller(full)["heal_events"] == 1,
          "fresh heal input at full health changed state")
    c.step(keys=["h"]);c.step();weak = c.step(keys=["h"])
    check(controller(weak)["health"] == 1, "damage did not reach one health")
    saved = c.step(keys=["f5"])
    check(controller(saved)["save_status"] == "saved", "validated save failed")
    saved_bytes = c.saved_bytes()
    saved_pose = position(saved).copy()
    c.step();dead = c.step(keys=["h"])
    check(controller(dead)["dead"] and controller(dead)["health"] == 0 and animation(dead)["frame"] == 0,
          "zero health did not stop native playback")
    dead_pose = position(dead).copy()
    blocked = c.step(8, ["left", "space", "g", "h", "r"])
    check(position(blocked) == dead_pose and controller(blocked)["health"] == 0 and
          controller(blocked)["attacks"] == 2 and animation(blocked)["frame"] == 0,
          "dead controller moved, attacked, healed or dropped below zero health")
    restored = c.step(keys=["f9"])
    check(controller(restored)["load_status"] == "loaded" and controller(restored)["health"] == 1
          and not controller(restored)["dead"] and position(restored) == saved_pose,
          "validated load did not restore alive checkpoint")
    check(animation(restored)["current_clip"] == "idle" and animation(restored)["frame"] == 1,
          "load did not reconstruct initial live animation pose")
    valid = json.loads(saved_bytes)
    invalid = []
    bad = json.loads(saved_bytes);bad["schema_version"] = 99
    invalid.append((json.dumps(bad).encode(), "unsupported_game_schema"))
    bad = json.loads(saved_bytes);bad["data"]["health"] = 3;bad["data"]["position"]["x"] = 1001
    invalid.append((json.dumps(bad).encode(), "invalid_game_schema"))
    bad = json.loads(saved_bytes);bad["data"]["position"]["extra"] = True
    invalid.append((json.dumps(bad).encode(), "invalid_game_schema"))
    bad = json.loads(saved_bytes);bad["data"]["health"] = -1
    invalid.append((json.dumps(bad).encode(), "invalid_game_schema"))
    invalid.append((b'{"format_version":1,', "invalid_data"))
    for payload, expected in invalid:
        before = c.step()  # release F9
        before_health, before_pose = controller(before)["health"], position(before).copy()
        c.replace_saves(payload)
        rejected = c.step(keys=["f9"])
        check(controller(rejected)["load_status"] == expected and
              controller(rejected)["health"] == before_health and position(rejected) == before_pose,
              "invalid save partially changed gameplay")
        check(c.saved_bytes() == payload, "failed read overwrote the save")
    c.replace_saves(saved_bytes)
    reset = c.request({"cmd": "reset"}, "reset")
    check(reset == initial, "reset did not reproduce initial gameplay and native pose")
    check(c.saved_bytes() == saved_bytes, "reset erased persistent data")
    recovered = c.step(keys=["f9"])
    check(controller(recovered)["health"] == 1 and position(recovered) == saved_pose, "reset worker lost save root")
    c.request({"cmd": "quit"}, "first process shutdown")
    c.close();c.start()
    check(c.observe() == initial, "fresh process retained VM state")
    fresh = c.step(keys=["f9"])
    check(controller(fresh)["health"] == valid["data"]["health"] and position(fresh) == saved_pose,
          "fresh process did not recover save")
    dead = c.step(keys=["h"])
    check(controller(dead)["dead"], "fresh process could not take real damage")
    dead_save = c.step(keys=["f5"])
    check(controller(dead_save)["save_status"] == "saved", "dead checkpoint save failed")
    c.saved_bytes()
    c.request({"cmd": "reset"}, "dead checkpoint reset")
    loaded_dead = c.step(keys=["f9"])
    check(controller(loaded_dead)["dead"] and controller(loaded_dead)["health"] == 0
          and animation(loaded_dead)["frame"] == 0, "dead checkpoint restarted animation or resurrected actor")
    immobile = c.step(4, ["right", "g", "space"])
    check(position(immobile) == position(loaded_dead) and controller(immobile)["attacks"] == 0,
          "loaded dead actor could act")
    c.request({"cmd": "quit"}, "final shutdown")
    good_bytes = c.saved_bytes()
    c.close()
    # An invalid authored override must not replace an existing valid slot.
    for project in c.projects:
        scene_path = project / "scenes/typed.scene"
        scene = json.loads(scene_path.read_text())
        scene["actors"][0]["components"]["controller"]["health"] = 99
        scene_path.write_text(json.dumps(scene))
    c.start()
    rejected_save = c.step(keys=["f5"])
    check(controller(rejected_save)["save_status"] == "invalid_game_schema" and
          c.saved_bytes() == good_bytes, "invalid live state replaced a valid checkpoint")
    c.request({"cmd": "quit"}, "invalid authoring shutdown")


def reload_compiled_pair(runtime, fixture, generated, root):
    projects = [root / "reload-lua", root / "reload-typescript"]
    for project in projects:
        shutil.copytree(fixture, project, ignore=shutil.ignore_patterns(".pocketengine", "__pycache__"))
    shutil.copyfile(generated / "TypedController.lua", projects[1] / "component_types/TypedController.lua")
    shutil.copyfile(generated / "TSTL-LICENSE.txt", projects[1] / "component_types/TSTL-LICENSE.txt")
    c = Comparison(runtime, *projects, root / "reload-saves")
    try:
        c.step(8, ["right"])
        c.step(keys=["h"])
        before = c.step()
        reloaded = c.request({"cmd": "reload_scripts", "types": ["TypedController"]}, "compiled bundle hot reload")
        check(reloaded["frame"] == before["frame"] and reloaded["actors"] == before["actors"],
              "compiled reload changed live state or advanced simulation")
        check(reloaded["reload"]["instances_updated"] == 1, "compiled controller was not updated")
        c.step(3, ["right"])
        c.step(keys=["space"])
        c.step(12)
        c.step(keys=["f5"])
        c.step(keys=["h"])
        restored = c.step(keys=["f9"])
        check(controller(restored)["load_status"] == "loaded",
              "transferred compiled module helper no longer validates/restores saves")
        c.request({"cmd": "quit"}, "compiled reload shutdown")
        return c.comparisons
    finally:
        c.close()


if __name__ == "__main__":
    check(len(sys.argv) == 4, "usage: agent_typed_gameplay_test.py RUNTIME FIXTURE GENERATED_LUA_DIR")
    runtime, fixture, generated = [Path(value).resolve() for value in sys.argv[1:]]
    controller_path = generated / "TypedController.lua"
    check(controller_path.is_file(), "compiler did not emit TypedController.lua into generated output directory")
    with tempfile.TemporaryDirectory(prefix="pocket-typed-gameplay-") as directory:
        root = Path(directory)
        projects = [root / "lua", root / "typescript"]
        for project in projects:
            shutil.copytree(fixture, project, ignore=shutil.ignore_patterns(".pocketengine", "__pycache__"))
        # Exactly one compiled file replaces its reference; scene, assets, view,
        # engine, inputs and save namespaces are identical.
        shutil.copyfile(controller_path, projects[1] / "component_types/TypedController.lua")
        shutil.copyfile(generated / "TSTL-LICENSE.txt", projects[1] / "component_types/TSTL-LICENSE.txt")
        comparison = Comparison(runtime, *projects, root)
        try:
            run(comparison)
            print(f"agent_typed_gameplay_test: {comparison.comparisons} exact real-runtime comparisons passed; "
                  "movement, animation, health/death, pause, schema validation, reset and fresh-process saves")
        finally:
            comparison.close()
        reload_comparisons = reload_compiled_pair(runtime, fixture, generated, root)
        print(f"agent_typed_gameplay_test: {reload_comparisons} additional exact comparisons passed after compiled-bundle hot reload")
