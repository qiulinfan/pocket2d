#!/usr/bin/env python3
"""Scenario files drive the Gymnasium-style client against two_world_village.

Each scenario is solved by a scripted policy that only chooses held keys, the
same interface an RL agent or LLM player gets. The scenarios are data under
tests/fixtures/two_world_village/scenarios.
"""
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "clients/python"))
from pocket2d import PocketEnv, Scenario, ScenarioError, find_actor, read_property  # noqa: E402


def check(condition, message):
    if not condition:
        raise AssertionError(message)


class Episode:
    """Runs a policy and accumulates the Gymnasium step results."""

    def __init__(self, env):
        self.env = env
        self.observation, self.info = env.reset()
        self.total_reward = 0.0
        self.done = False

    def act(self, keys=()):
        check(not self.done, "episode already finished")
        self.observation, reward, terminated, truncated, self.info = self.env.step(list(keys))
        self.total_reward += reward
        self.done = terminated or truncated
        if truncated:
            raise AssertionError("episode truncated at frame %d" % self.info["frame"])
        return self.observation

    def state(self, prop):
        return read_property(self.observation, "village", prop, "state")

    def position(self, name="player"):
        actor = find_actor(self.observation, name)
        return actor["position"]["x"], actor["position"]["y"]

    def press(self, key):
        self.act()
        return self.act([key])

    def move_to(self, x, y, attack=False, limit=2400):
        for _ in range(limit):
            if self.done:
                return
            px, py = self.position()
            dx, dy = x - px, y - py
            keys = ["space"] if attack else []
            if abs(dx) <= 0.075 and abs(dy) <= 0.075:
                self.act(keys)
                return
            if abs(dx) > 0.075:
                keys.append("right" if dx > 0 else "left")
            if abs(dy) > 0.075:
                keys.append("down" if dy > 0 else "up")
            self.act(keys)
        raise AssertionError("could not reach %r" % ((x, y),))

    def wait_until(self, predicate, limit, keys=()):
        for _ in range(limit):
            if self.done or predicate():
                return
            self.act(keys)
        raise AssertionError("condition not reached")

    def wait(self, steps):
        for _ in range(steps):
            if self.done:
                return
            self.act()

    def fight(self, name, limit=480):
        for _ in range(limit):
            if self.done:
                return
            health = read_property(self.observation, name, "health", "combat")
            if health is not None and health <= 0:
                self.act()
                return
            px, py = self.position()
            qx, qy = self.position(name)
            keys = ["space"]
            if abs(px - qx) > 0.8:
                keys.append("right" if px < qx else "left")
            if abs(py - qy) > 0.8:
                keys.append("down" if py < qy else "up")
            self.act(keys)
        raise AssertionError("could not defeat " + name)

    def plant_and_harvest(self):
        self.act()
        self.move_to(-4.5, -2.5)
        self.press("e")
        self.wait(121)
        if not self.done:
            self.press("e")
        self.wait(2)

    def portal_to_underworld(self):
        self.move_to(4.75, 2.5)
        self.wait_until(lambda: self.state("transition_remaining") > 0, 30, ["right"])
        self.wait_until(lambda: self.state("world_id") == "underworld" and self.state("transition_remaining") == 0, 60)


def wider(scenario, names):
    """The same scenario observing extra actors the scripted policy targets."""
    data = {"version": 1, "name": scenario.name, "seed": scenario.seed, "max_frames": scenario.max_frames,
            "frames_per_step": scenario.frames_per_step, "controls": scenario.controls,
            "success": scenario.success, "failure": scenario.failure, "reward": scenario.reward,
            "select": {"names": sorted(set(scenario.select["names"]) | set(names))}}
    return Scenario.from_dict(data)


def run(executable, project):
    scenarios = {path.name.split(".")[0]: Scenario.load(path)
                 for path in sorted((project / "scenarios").glob("*.scenario.json"))}
    check(sorted(scenarios) == ["close_route", "defend_wave", "plant_and_harvest"], "three village scenarios")
    for bad in [{}, {"version": 1, "name": "x", "max_frames": 1, "controls": {}, "success": [{}]},
                {"version": 1, "name": "x", "max_frames": 1, "controls": {"keys": ["a"]},
                 "success": [{"actor": "a", "property": "x", "op": "~", "value": 1}]}]:
        try:
            Scenario.from_dict(bad)
        except ScenarioError:
            continue
        raise AssertionError("invalid scenario accepted: %r" % bad)

    with tempfile.TemporaryDirectory(prefix="pocket-scenarios-") as saves:
        env = PocketEnv(executable, project, scenarios["plant_and_harvest"], save_dir=saves)
        try:
            episode = Episode(env)
            check(episode.info == {"frame": 0, "outcome": None} and
                  [a["name"] for a in episode.observation["actors"]] == ["village", "player"],
                  "reset returns the selected observation")
            try:
                env.step(["f5"])
                raise AssertionError("keys outside the controls must be rejected")
            except ValueError:
                pass
            episode.plant_and_harvest()
            check(episode.done and episode.info["outcome"] == "success", "harvest succeeds: %r" % episode.info)
            expected = 0.25 + 1.0 - 0.0005 * episode.info["frame"]
            check(abs(episode.total_reward - expected) < 1e-9,
                  "reward sums planting, harvest and time: %f != %f" % (episode.total_reward, expected))
            first = episode.info["frame"]
            # The same seed and the same policy reproduce the episode.
            again = Episode(env)
            again.plant_and_harvest()
            check(again.info["frame"] == first and abs(again.total_reward - episode.total_reward) < 1e-9,
                  "episodes are reproducible")
        finally:
            env.close()

        invaders = ["invader_1", "invader_2", "invader_3"]
        env = PocketEnv(executable, project, wider(scenarios["defend_wave"], invaders), save_dir=saves)
        try:
            episode = Episode(env)
            episode.act()
            episode.press("1")
            episode.move_to(4.0, -1.5, attack=True)
            episode.wait_until(lambda: episode.state("route_phase") == "active", 1000)
            for name in invaders:
                episode.fight(name)
            episode.wait(4)
            check(episode.done and episode.info["outcome"] == "success", "wave defended: %r" % episode.info)
            check(episode.total_reward > 1.5, "defeats and the cleared wave are rewarded")
        finally:
            env.close()

        env = PocketEnv(executable, project, wider(scenarios["close_route"], invaders + ["guardian", "source"]),
                        save_dir=saves)
        try:
            episode = Episode(env)
            episode.plant_and_harvest()
            episode.press("1")
            episode.move_to(4.0, -1.5, attack=True)
            episode.wait_until(lambda: episode.state("route_phase") == "active", 1000)
            for name in invaders:
                episode.fight(name)
            episode.portal_to_underworld()
            episode.move_to(22.0, 1.5)
            episode.wait_until(lambda: episode.state("player_health") < 6, 180)
            episode.fight("guardian")
            episode.press("q")
            episode.fight("source")
            episode.wait(4)
            check(episode.done and episode.info["outcome"] == "success", "route closed: %r" % episode.info)
        finally:
            env.close()
    print("scenario Gym client regressions passed")


if __name__ == "__main__":
    check(len(sys.argv) == 3, "usage: scenario_env_test.py RUNTIME TWO_WORLD_VILLAGE")
    run(str(Path(sys.argv[1]).resolve()), Path(sys.argv[2]).resolve())
