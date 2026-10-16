"""Scenario files: a task as data for RL environments, LLM players and regression tests."""
import json
from dataclasses import dataclass, field
from pathlib import Path

CONDITION_OPS = {"==", "!=", "<", "<=", ">", ">="}


class ScenarioError(ValueError):
    pass


def _require(condition, message):
    if not condition:
        raise ScenarioError(message)


def _check_condition(condition, where):
    _require(isinstance(condition, dict), where + " must be an object")
    if "event" in condition:
        _require(isinstance(condition["event"], str) and condition["event"], where + ".event must be a string")
    elif "actor_exists" in condition:
        _require(isinstance(condition["actor_exists"], str), where + ".actor_exists must be a string")
    else:
        for key in ("actor", "property", "op"):
            _require(isinstance(condition.get(key), str), where + "." + key + " must be a string")
        _require(condition["op"] in CONDITION_OPS, where + ".op is not a comparison")
        _require(isinstance(condition.get("value"), (int, float, str, bool)), where + ".value is required")


@dataclass
class Scenario:
    """A validated scenario. See docs/development/scenarios.md for the format."""
    name: str
    seed: int
    max_frames: int
    frames_per_step: int
    controls: dict
    success: list
    failure: list = field(default_factory=list)
    reward: list = field(default_factory=list)
    select: dict = None
    description: str = ""
    script: list = None
    expect: str = "success"
    path: Path = None

    @classmethod
    def load(cls, path):
        path = Path(path)
        data = json.loads(path.read_text(encoding="utf-8"))
        return cls.from_dict(data, path)

    @classmethod
    def from_dict(cls, data, path=None):
        _require(isinstance(data, dict) and data.get("version") == 1, "scenario must have version 1")
        name = data.get("name")
        _require(isinstance(name, str) and name, "scenario needs a name")
        seed = data.get("seed", 0)
        _require(isinstance(seed, int) and 0 <= seed <= 0xFFFFFFFF, "seed must be a uint32")
        max_frames = data.get("max_frames")
        _require(isinstance(max_frames, int) and max_frames >= 1, "max_frames must be a positive integer")
        frames_per_step = data.get("frames_per_step", 1)
        _require(isinstance(frames_per_step, int) and 1 <= frames_per_step <= 3600, "frames_per_step must be in 1..3600")
        controls = data.get("controls")
        _require(isinstance(controls, dict) and ("keys" in controls) != ("actions" in controls),
                 "controls must declare either keys or actions")
        for kind in ("keys", "actions"):
            if kind in controls:
                _require(isinstance(controls[kind], list) and controls[kind] and
                         all(isinstance(entry, str) for entry in controls[kind]),
                         "controls." + kind + " must list names")
        success = data.get("success")
        _require(isinstance(success, list) and success, "success needs at least one condition")
        failure = data.get("failure", [])
        _require(isinstance(failure, list), "failure must be a list")
        _require(len(success) + len(failure) <= 16, "at most 16 success and failure conditions")
        for index, condition in enumerate(success):
            _check_condition(condition, "success[%d]" % index)
        for index, condition in enumerate(failure):
            _check_condition(condition, "failure[%d]" % index)
        reward = data.get("reward", [])
        _require(isinstance(reward, list), "reward must be a list")
        for index, term in enumerate(reward):
            where = "reward[%d]" % index
            _require(isinstance(term, dict), where + " must be an object")
            if "per_frame" in term:
                _require(isinstance(term["per_frame"], (int, float)), where + ".per_frame must be a number")
                continue
            _require(isinstance(term.get("weight", 1.0), (int, float)), where + ".weight must be a number")
            if "event" in term:
                _require(isinstance(term["event"], str) and term["event"], where + ".event must be a string")
            else:
                _require(isinstance(term.get("actor"), str) and isinstance(term.get("property"), str),
                         where + " needs per_frame, event, or actor and property")
        select = data.get("select")
        _require(select is None or isinstance(select, dict), "select must be an object")
        script = data.get("script")
        if script is not None:
            _require(isinstance(script, list) and 1 <= len(script) <= 10000, "script must list 1..10000 inputs")
            for index, entry in enumerate(script):
                where = "script[%d]" % index
                _require(isinstance(entry, dict), where + " must be an object")
                _require(isinstance(entry.get("frames"), int) and 1 <= entry["frames"] <= 36000,
                         where + ".frames must be in 1..36000")
                _require(isinstance(entry.get("keys", []), list) and isinstance(entry.get("actions", {}), dict),
                         where + " keys must be a list and actions an object")
        expect = data.get("expect", "success")
        _require(expect in ("success", "failure", "truncated"), "expect must be success, failure or truncated")
        return cls(name=name, seed=seed, max_frames=max_frames, frames_per_step=frames_per_step,
                   controls=controls, success=success, failure=failure, reward=reward, select=select,
                   description=data.get("description", ""), script=script, expect=expect, path=path)
