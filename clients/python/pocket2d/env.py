"""A Gymnasium-style environment over the real Pocket2D runtime."""
from .runtime import Runtime
from .scenario import Scenario

try:  # Optional: subclass gymnasium.Env and describe the action space when available.
    import gymnasium
    from gymnasium import spaces
    _Base = gymnasium.Env
except ImportError:  # pragma: no cover - exercised when gymnasium is absent
    gymnasium = None
    spaces = None
    _Base = object


def find_actor(observation, reference):
    """An actor by decimal UID string or by name (lowest UID), or None."""
    actors = observation.get("actors", [])
    for actor in actors:
        if actor["uid"] == reference:
            return actor
    named = [actor for actor in actors if actor["name"] == reference]
    return min(named, key=lambda actor: int(actor["uid"])) if named else None


def read_property(observation, reference, prop, component=None):
    """An actor's component property, or its world x/y/rotation without component."""
    actor = find_actor(observation, reference)
    if actor is None:
        return None
    if component is None:
        return actor.get("position", {}).get(prop)
    for entry in actor.get("components", []):
        if entry["key"] == component:
            return entry["properties"].get(prop)
    return None


class PocketEnv(_Base):
    """reset(seed, options) -> (observation, info); step(action) -> (observation,
    reward, terminated, truncated, info), following Gymnasium 1.x.

    An action is a list of held key names (scenario controls.keys) or a dict of
    semantic action values (controls.actions). Each step advances
    frames_per_step ticks with run_until, stopping early when a success or
    failure condition holds. Observations are the protocol's observation
    dictionaries narrowed by the scenario's select. info carries frame,
    outcome ("success", "failure" or None), the matched condition and events.
    """

    metadata = {"render_modes": []}

    def __init__(self, executable, project, scenario, save_dir=None, timeout=60.0):
        self.scenario = scenario if isinstance(scenario, Scenario) else Scenario.load(scenario)
        self.runtime = Runtime(executable, project, seed=self.scenario.seed, save_dir=save_dir, timeout=timeout)
        self._events = sorted({term["event"] for term in self.scenario.reward if "event" in term})
        self._select = self._selection()
        self._start_frame = 0
        self._last = None
        self._action_types = {}
        if "actions" in self.scenario.controls:
            declared = {entry["name"]: entry["type"]
                        for entry in self.runtime.request({"cmd": "describe"})["actions"]}
            for name in self.scenario.controls["actions"]:
                if name not in declared:
                    raise ValueError("scenario control is not a declared action: " + name)
                self._action_types[name] = declared[name]
        if spaces is not None:
            self.action_space = self._action_space()
            self.observation_space = None  # observations are structured dictionaries

    def _selection(self):
        select = dict(self.scenario.select or {})
        if "names" in select:  # keep every actor the reward refers to
            names = set(select["names"])
            names.update(term["actor"] for term in self.scenario.reward if "actor" in term)
            select["names"] = sorted(names)
        return select or None

    def _action_space(self):
        controls = self.scenario.controls
        if "keys" in controls:
            return spaces.MultiBinary(len(controls["keys"]))
        space = {}
        for name, kind in self._action_types.items():
            if kind == "button":
                space[name] = spaces.Discrete(2)
            elif kind == "axis":
                space[name] = spaces.Box(-1.0, 1.0, shape=(1,))
            else:
                space[name] = spaces.Box(-1.0, 1.0, shape=(2,))
        return spaces.Dict(space)

    def _input(self, action):
        controls = self.scenario.controls
        if "keys" in controls:
            if action is None:
                return {"keys": []}
            if not isinstance(action, dict) and len(action) == len(controls["keys"]) and \
                    all(not isinstance(value, str) for value in action):
                action = [key for key, held in zip(controls["keys"], action) if held]
            keys = list(action)
            unknown = [key for key in keys if key not in controls["keys"]]
            if unknown:
                raise ValueError("keys outside the scenario controls: " + ", ".join(unknown))
            return {"keys": keys}
        action = dict(action or {})
        unknown = [name for name in action if name not in controls["actions"]]
        if unknown:
            raise ValueError("actions outside the scenario controls: " + ", ".join(unknown))
        values = {}
        for name, value in action.items():
            if hasattr(value, "tolist"):  # numpy values from gymnasium spaces
                value = value.tolist()
            kind = self._action_types[name]
            if kind == "button":
                values[name] = bool(value)
            elif kind == "axis":
                values[name] = float(value[0] if isinstance(value, list) else value)
            else:
                values[name] = [float(value[0]), float(value[1])]
        return {"actions": values}

    def _reward(self, previous, current, frames, events):
        total = 0.0
        for term in self.scenario.reward:
            if "per_frame" in term:
                total += term["per_frame"] * frames
            elif "event" in term:
                total += term.get("weight", 1.0) * sum(1 for event in events if event["type"] == term["event"])
            else:
                before = read_property(previous, term["actor"], term["property"], term.get("component"))
                after = read_property(current, term["actor"], term["property"], term.get("component"))
                if isinstance(before, (int, float)) and isinstance(after, (int, float)) and \
                        not isinstance(before, bool) and not isinstance(after, bool):
                    total += term.get("weight", 1.0) * (after - before)
        return total

    def reset(self, *, seed=None, options=None):
        if _Base is not object:
            super().reset(seed=seed)
        command = {"cmd": "reset", "seed": self.scenario.seed if seed is None else seed}
        self.runtime.request(command)
        observation = self.runtime.request({"cmd": "observe", **({"select": self._select} if self._select else {})})
        self._start_frame = observation["frame"]
        self._last = observation
        return observation, {"frame": observation["frame"], "outcome": None}

    def step(self, action):
        if self._last is None:
            raise RuntimeError("call reset() before step()")
        remaining = self.scenario.max_frames - (self._last["frame"] - self._start_frame)
        if remaining <= 0:
            raise RuntimeError("episode is truncated; call reset()")
        conditions = self.scenario.success + self.scenario.failure
        command = {"cmd": "run_until", "max_frames": min(self.scenario.frames_per_step, remaining),
                   "until": conditions, **self._input(action)}
        if self._select:
            command["select"] = self._select
        if self._events:
            command["events"] = self._events
        observation = self.runtime.request(command)
        result = observation["run_until"]
        events = observation.get("events", [])
        reward = self._reward(self._last, observation, result["frames"], events)
        outcome = None
        if result["reason"] == "condition":
            outcome = "success" if result["condition"] < len(self.scenario.success) else "failure"
        elif result["reason"] == "stopped":
            outcome = "failure"
        terminated = outcome is not None
        truncated = not terminated and observation["frame"] - self._start_frame >= self.scenario.max_frames
        self._last = observation
        info = {"frame": observation["frame"], "outcome": outcome, "condition": result["condition"],
                "events": events}
        return observation, reward, terminated, truncated, info

    def close(self):
        self.runtime.close()
