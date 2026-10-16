# Scenarios and the Python client

A scenario is a task stored as data, used unchanged by reinforcement-learning
environments, LLM players and regression tests. Scenario files live with the
project, for example
`tests/fixtures/two_world_village/scenarios/*.scenario.json`.

```json
{
  "version": 1,
  "name": "plant_and_harvest",
  "description": "Plant a seed and harvest the grown crop into the bag.",
  "seed": 42,
  "max_frames": 1200,
  "frames_per_step": 1,
  "controls": {"keys": ["up", "down", "left", "right", "e", "space", "1", "q"]},
  "select": {"names": ["village", "player"]},
  "success": [{"actor": "village", "component": "state", "property": "harvested", "op": ">=", "value": 1}],
  "failure": [{"actor": "village", "component": "state", "property": "phase", "op": "==", "value": "failed"}],
  "reward": [
    {"actor": "village", "component": "state", "property": "harvested", "weight": 1.0},
    {"event": "crop_harvested", "weight": 0.5},
    {"per_frame": -0.0005}
  ]
}
```

- The episode starts from the project's initial scene after `reset` with
  `seed` (overridable per reset).
- `controls` lists either the held `keys` or the semantic `actions` (declared
  in `actions.json`) an agent may use. The other inputs stay released.
- `success` and `failure` are `run_until` conditions (at most 16 together).
  Each step stops early at the first tick where one holds; a stopped game is a
  failure. `max_frames` truncates the episode.
- `reward` terms add each step: `weight` × the change of a numeric actor
  property (`component` key, or world `x`/`y`/`rotation` without one),
  `weight` × the number of published events of a type, and `per_frame` × the
  ticks advanced.
- `game: true` adds game-declared observations (`OnAgentObserve`) to every
  observation.
- `select` narrows observations as in the agent protocol; actors referenced by
  rewards are always kept when `names` is given.

## Python client

`clients/python/pocket2d` needs only the standard library. When `gymnasium` is
installed, `PocketEnv` subclasses `gymnasium.Env` and sets `action_space`
(`MultiBinary` for keys; a `Dict` of `Discrete(2)`/`Box` for actions).
Observations are the protocol's dictionaries, so `observation_space` is `None`.

```python
import sys; sys.path.insert(0, "clients/python")
from pocket2d import PocketEnv

env = PocketEnv("build/sdl3-release/game", "tests/fixtures/two_world_village",
                "tests/fixtures/two_world_village/scenarios/plant_and_harvest.scenario.json",
                save_dir="/tmp/pocket-saves")
observation, info = env.reset(seed=42)
observation, reward, terminated, truncated, info = env.step(["right"])
env.close()
```

`step` takes held key names (or a binary vector in `controls.keys` order) or a
dict of action values, and returns Gymnasium 1.x results. `info` carries
`frame`, `outcome` (`"success"`, `"failure"` or `None`), the matched
`condition` index and the reported `events`. `Runtime` is the underlying JSONL
client for direct protocol use.

`tests/scenario_env_test.py` solves all three village scenarios with scripted
key policies and checks reward accounting and episode reproducibility.
Vectorized environments, PettingZoo multi-agent channels and LLM-player
evaluation are not implemented yet.
