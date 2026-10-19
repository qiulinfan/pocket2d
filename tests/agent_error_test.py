#!/usr/bin/env python3
"""Failure/reset contracts against the executable, including actual Lua errors."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from agent_protocol_test import Agent, check, successful


def startup_failure(executable, source, mutator):
    with tempfile.TemporaryDirectory(prefix="pocket-agent-invalid-") as directory:
        project = Path(directory) / "project"
        shutil.copytree(source, project)
        mutator(project)
        result = subprocess.run([executable, "--project", str(project), "--headless", "--agent"],
                                input='{"cmd":"observe"}\n', capture_output=True, text=True, encoding="utf-8", timeout=15)
        check(result.returncode != 0, "invalid startup must fail nonzero")
        lines = result.stdout.splitlines()
        check(len(lines) == 1 and json.loads(lines[0])["ok"] is False,
              "startup error must be a single JSON response: " + result.stdout)


def add_probe(project, source):
    path = project / "component_types" / "Probe.lua"
    path.write_text(source)
    scene = project / "scenes" / "defense.scene"
    data = json.loads(scene.read_text())
    data["actors"] = [actor for actor in data["actors"] if actor.get("uid") != 901]
    data["actors"].append({"name": "probe", "uid": 901,
                           "components": {"probe": {"type": "Probe"}}})
    scene.write_text(json.dumps(data))


def quit_cleanly(agent):
    successful(agent, {"cmd": "quit"})
    check(agent.process.wait(timeout=5) == 0, "quit must succeed even after a runtime error")
    check(agent.lines.get(timeout=5) is None, "agent emitted unsolicited stdout")


def particle_observation(executable, source):
    with tempfile.TemporaryDirectory(prefix="pocket-agent-particle-") as directory:
        project = Path(directory) / "project"
        shutil.copytree(source, project)
        scene = project / "scenes" / "defense.scene"
        data = json.loads(scene.read_text())
        data["actors"].append({"name": "emitter", "uid": 902,
                               "components": {"particle": {"type": "ParticleSystem"}}})
        scene.write_text(json.dumps(data))
        agent = Agent(executable, str(project))
        try:
            initial = successful(agent, {"cmd": "observe"})
            emitter = next(actor for actor in initial["actors"] if actor["uid"] == "902")
            settings = emitter["components"][0]["properties"]
            check("end_scale" in settings and settings["end_scale"] is None,
                  "unset particle end_scale must be JSON null, not an observation failure")
            successful(agent, {"cmd": "step"})
            check(successful(agent, {"cmd": "reset"}) == initial,
                  "default particle state must survive reset and observation")
            quit_cleanly(agent)
        finally:
            agent.close()


def invalid_number_quit(executable, source):
    with tempfile.TemporaryDirectory(prefix="pocket-agent-nonfinite-") as directory:
        project = Path(directory) / "project"
        shutil.copytree(source, project)
        add_probe(project, '''Probe = { OnUpdate = function(self)
            self.invalid_number = math.huge
        end }''')
        agent = Agent(executable, str(project))
        try:
            successful(agent, {"cmd": "observe"})
            failed = agent.request({"cmd": "step"})
            check(not failed["ok"] and "non-finite" in failed["error"],
                  "invalid gameplay numbers must not be silently serialized as null")
            check(not agent.request({"cmd": "observe"})["ok"],
                  "invalid number must still fail a later observation")
            quit_cleanly(agent)
        finally:
            agent.close()


def missing_scene_reset(executable, source):
    with tempfile.TemporaryDirectory(prefix="pocket-agent-reset-scene-") as directory:
        project = Path(directory) / "project"
        shutil.copytree(source, project)
        agent = Agent(executable, str(project))
        try:
            successful(agent, {"cmd": "observe"})
            (project / "scenes" / "defense.scene").unlink()
            failed = agent.request({"cmd": "reset"})
            check(not failed["ok"] and "missing" in failed["error"],
                  "missing scene reset must return the actual load error")
            for command in [{"cmd": "observe"}, {"cmd": "step", "frames": 2}]:
                rejected = agent.request(command)
                check(not rejected["ok"] and rejected["frame"] == failed["frame"],
                      "failed reset must reject access without advancing a partial world")
            check(successful(agent, {"cmd": "describe"})["protocol"] == 2,
                  "protocol discovery remains available after failed reset")
            quit_cleanly(agent)
        finally:
            agent.close()


def lua_io_diagnostics(executable, source):
    with tempfile.TemporaryDirectory(prefix="pocket-agent-io-") as directory:
        project = Path(directory) / "project"
        shutil.copytree(source, project)
        add_probe(project, '''
            io.write("top-level io.write diagnostic\\n")
            io.stdout:write("top-level io.stdout diagnostic\\n")
            io.stdout:flush()
            Probe = { OnUpdate = function(self)
                print("print diagnostic")
                io.write("update io.write diagnostic\\n")
                io.output():write("default output diagnostic\\n")
                io.stdout:write("update io.stdout diagnostic\\n")
                io.stdout:flush()
            end }
        ''')
        agent = Agent(executable, str(project))
        try:
            successful(agent, {"cmd": "observe"})
            successful(agent, {"cmd": "step", "frames": 2})
            quit_cleanly(agent)
        finally:
            agent.close()


def run(executable, source):
    startup_failure(executable, source, lambda p: (p / "game.config").write_text("{"))
    startup_failure(executable, source, lambda p: (p / "scenes" / "defense.scene").write_text("[]"))
    with tempfile.TemporaryDirectory(prefix="pocket-agent-errors-") as directory:
        project = Path(directory) / "project"
        shutil.copytree(source, project)
        add_probe(project, '''Probe = { n = 0, OnUpdate = function(self)
            print("lua diagnostic stays off stdout")
            error("intentional-agent-test-error")
        end }''')
        agent = Agent(executable, str(project))
        try:
            check(successful(agent, {"cmd": "describe"})["protocol"] == 2, "protocol discovery")
            for request in [{"cmd": "step", "frames": 3601}, {"cmd": "step", "frames": 1.5},
                            {"cmd": "step", "keys": ["not-a-key"]},
                            {"cmd": "step", "keys": "right"}, {"cmd": "reset", "seed": -1}]:
                check(agent.request(request)["ok"] is False, "invalid request must fail")
                check(successful(agent, {"cmd": "observe"})["frame"] == 0, "rejection is atomic")
            failed = agent.request({"cmd": "step"})
            check(failed["ok"] is False and "intentional-agent-test-error" in failed["error"],
                  "actual callback failure must reach protocol: " + str(failed))
            check(successful(agent, {"cmd": "observe"})["frame"] == 1,
                  "failed callback reports the completed tick honestly")
            # A failing reset must not leave a null engine that crashes observe.
            config = project / "game.config"
            original = config.read_text()
            config.write_text("{")
            check(agent.request({"cmd": "reset"})["ok"] is False, "reset failure is structured")
            check(agent.request({"cmd": "observe"})["ok"] is False, "failed reset leaves recoverable state")
            config.write_text(original)
            add_probe(project, '''Probe = { first = math.random(), n = 0,
                OnUpdate = function(self) self.n = math.random() end }''')
            first = successful(agent, {"cmd": "reset", "seed": 123})
            played = successful(agent, {"cmd": "step", "frames": 4})
            check(successful(agent, {"cmd": "reset", "seed": 123}) == first,
                  "seed must apply before loading top-level scripts")
            check(successful(agent, {"cmd": "step", "frames": 4}) == played, "seeded replay")
            different = successful(agent, {"cmd": "reset", "seed": 456})
            def rng_value(snapshot):
                return next(a for a in snapshot["actors"] if a["name"] == "probe")["components"]
            check(rng_value(first) != rng_value(different), "different seeds change real Lua draws")
            successful(agent, {"cmd": "quit"})
            check(agent.process.wait(timeout=5) == 0, "quit returns success")
        finally:
            agent.close()
    particle_observation(executable, source)
    invalid_number_quit(executable, source)
    missing_scene_reset(executable, source)
    lua_io_diagnostics(executable, source)
    print("agent errors, seeded reset, particle sentinel and Lua I/O regressions passed")


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()), str(Path(sys.argv[2]).resolve()))
