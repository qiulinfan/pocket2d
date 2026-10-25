#!/usr/bin/env python3
"""Animation behavior through the real JSONL runtime; no direct Lua execution."""
import json
from pathlib import Path
import shutil
import sys
import tempfile
from agent_protocol_test import Agent, check, successful, properties, actor


def step(agent, count=1, keys=()):
    return successful(agent, {"cmd": "step", "frames": count, "keys": list(keys)})


def state(snapshot, clock, clip, frame, elapsed, playing=True):
    animation = properties(snapshot, "player", "SpriteAnimation")
    actual = (snapshot["frame"], animation["current_clip"], animation["frame"],
              animation["elapsed_ticks"], animation["playing"])
    check(actual == (clock, clip, frame, elapsed, playing),
          f"animation state mismatch: {actual} at expected clock {clock}")
    return animation


def event(kind, clip, frame, marker):
    return "".join(f"{label}:{kind}:{clip}:{frame}:{marker};" for label in "ACZ")


def timeline(runtime, project):
    saved = (project / "animations/hero.animation.json").read_bytes()
    agent = Agent(str(runtime), str(project))
    try:
        initial = successful(agent, {"cmd": "observe"})
        state(initial, 0, "idle", 1, 0)
        check(initial == successful(agent, {"cmd": "observe"}), "observe advanced animation")
        state(step(agent), 1, "idle", 1, 1)
        state(step(agent, 2), 3, "idle", 1, 3)
        state(step(agent), 4, "idle", 1, 0)
        state(step(agent, keys=["right"]), 5, "walk", 1, 0)
        state(step(agent, keys=["right"]), 6, "walk", 1, 1)
        state(step(agent, keys=["right"]), 7, "walk", 2, 0)
        observed = step(agent, 2, ["right"])
        state(observed, 9, "walk", 1, 0)
        stats = properties(observed, "player", "AnimationController")
        check(stats["frame_events"] == 5 and stats["play_calls"] == 9,
              "per-tick held-input Play restarted the animation")
        check(actor(observed, "player")["position"]["x"] > 0, "walk was not actual body movement")
        state(step(agent, keys=["space"]), 10, "attack", 1, 0)
        observed = step(agent)
        state(observed, 11, "attack", 2, 0)
        check(properties(observed, "player", "AnimationController")["hit_events"] == 1,
              "duration-one attack lost its hit marker")
        state(step(agent), 12, "attack", 2, 1)
        state(step(agent), 13, "idle", 1, 0)
        observed = step(agent)
        state(observed, 14, "idle", 1, 1)
        expected = (event("frame", "idle", 1, "rest") * 2 + event("frame", "walk", 1, "left")
                    + event("frame", "walk", 2, "right") + event("frame", "walk", 1, "left")
                    + event("frame", "attack", 1, "swing") + event("frame", "attack", 2, "hit")
                    + event("completed", "attack", 2, "") + event("frame", "idle", 1, "rest"))
        check(properties(observed, "view", "AnimationView")["trace"] == expected,
              "wrong event payload, order or multiplicity")
        check(properties(observed, "player", "AnimationController")["completed_events"] == 1,
              "one-shot completion duplicated")
        reset = successful(agent, {"cmd": "reset"})
        check(reset == initial, "reset did not restore the exact frame-zero snapshot")
        check((project / "animations/hero.animation.json").read_bytes() == saved,
              "runtime changed the author animation asset")
        successful(agent, {"cmd": "quit"})
    finally:
        agent.close()


def controls(runtime, project):
    agent = Agent(str(runtime), str(project))
    try:
        step(agent, 2, ["right"])
        state(step(agent, keys=["right", "p"]), 3, "walk", 2, 0, False)
        paused = step(agent, 5, ["right", "t"])
        check(state(paused, 8, "walk", 2, 0, False)["paused"], "Play resumed paused clip")
        state(step(agent, keys=["right", "r"]), 9, "walk", 2, 0)
        state(step(agent, 2, ["right"]), 11, "walk", 1, 0)
        state(step(agent, keys=["right", "d"]), 12, "walk", 1, 1, False)
        state(step(agent, 5, ["right", "t"]), 17, "walk", 1, 1, False)
        state(step(agent, keys=["right", "d"]), 18, "walk", 1, 1)
        state(step(agent, keys=["right"]), 19, "walk", 2, 0)
        state(step(agent, keys=["right", "s"]), 20, "", 0, 0, False)
        state(step(agent, 4, ["right", "t"]), 24, "", 0, 0, False)
        state(step(agent, keys=["right", "r"]), 25, "", 0, 0, False)
        state(step(agent, keys=["right", "x"]), 26, "walk", 1, 0)
        step(agent, keys=["right"])
        state(step(agent, keys=["right", "x"]), 28, "walk", 1, 0)
        successful(agent, {"cmd": "quit"})
    finally:
        agent.close()


def reentry(runtime, fixture):
    for mode in ["stop", "restart", "remove", "destroy"]:
        with tempfile.TemporaryDirectory(prefix="pocket-animation-agent-") as directory:
            project = Path(directory) / "project"
            shutil.copytree(fixture, project)
            scene_path = project / "scenes/animation.scene"
            scene = json.loads(scene_path.read_text())
            components = scene["actors"][0]["components"]
            components["a_observer"]["reentry"] = mode
            components["anim"]["clip"] = "attack"
            components["controller"]["auto_select"] = False
            scene_path.write_text(json.dumps(scene))
            agent = Agent(str(runtime), str(project))
            try:
                observed = step(agent)
                check(properties(observed, "view", "AnimationView")["trace"] == "A:frame:attack:1:swing;",
                      "stale callbacks survived reentry " + mode)
                if mode == "stop":
                    state(observed, 1, "", 0, 0, False)
                elif mode == "restart":
                    state(observed, 1, "walk", 1, 0)
                    observed = step(agent)
                    state(observed, 2, "walk", 1, 1)
                    check(properties(observed, "view", "AnimationView")["trace"] ==
                          "A:frame:attack:1:swing;" + event("frame", "walk", 1, "left"),
                          "restart queued event recursively dispatched or lost")
                elif mode == "remove":
                    check(not any(c["type"] == "SpriteAnimation" for c in actor(observed, "player")["components"]),
                          "removed animation remains observable")
                else:
                    check(not any(a["name"] == "player" for a in observed["actors"]),
                          "destroyed actor remains observable")
                successful(agent, {"cmd": "quit"})
            finally:
                agent.close()


if __name__ == "__main__":
    check(len(sys.argv) == 3, "usage: agent_animation_test.py RUNTIME FIXTURE_PROJECT")
    runtime, fixture = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    timeline(runtime, fixture)
    controls(runtime, fixture)
    reentry(runtime, fixture)
    print("agent_animation_test: exact timeline, held input, controls, callbacks and reset passed")
