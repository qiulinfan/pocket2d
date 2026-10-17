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
`VectorPocketEnv(executable, project, scenario, num_envs)` runs one runtime
process per sub-environment and steps them in parallel threads. Its `reset`
and `step` return per-environment lists (observations are not stacked), seeds
are `seed + i`, and autoreset follows Gymnasium 1.x's next-step mode
(`infos[i]["autoreset"]`). PettingZoo multi-agent channels and LLM-player
evaluation are not implemented yet.

## Scripted players

Game playtests that script a player through a whole game use `Session`
(`clients/python/pocket2d/session.py`) instead of scenario files. It launches
the headless runtime for a project and keeps the protocol plumbing games
otherwise rewrite; game rules (movement, fighting, quest logic) stay in the
game's subclass.

```python
import os, sys
from pathlib import Path
PROJECT = Path(__file__).resolve().parents[1]
# The engine checkout: POCKET2D_ENGINE, else a pocket2d folder beside the games repository.
ENGINE = Path(os.environ.get("POCKET2D_ENGINE", PROJECT.parents[1] / "pocket2d"))
sys.path.insert(0, str(ENGINE / "clients/python"))
from pocket2d import Session

class Bot(Session):
    @property
    def player(self):
        return self.value("player")

with Bot(PROJECT, seed=3, out="build/my-game", select={"names": ["Player"]}) as bot:
    bot.step({"move": [1, 0]}, frames=30)   # actions and keys are held for the step
    bot.key("i")                              # press and release a key
    bot.click_world(4, 2, camera=(bot.player["x"], bot.player["y"]))
    bot.shot("inventory")                     # build/my-game/inventory.png
```

- The runtime binary comes from `executable=`, `POCKET_GAME`, or the client's
  own checkout (`build/sdl3-release/game`, then `sdl3-debug`), so a game
  repository finds the engine once, through `POCKET2D_ENGINE` or the sibling
  folder. `ENGINE_ROOT` in `pocket2d.session` is that checkout.
- Without `save_dir` the game saves into a fresh temporary directory, removed
  on `close()`; `session.save_dir` is the directory in use.
- `step(actions, frames, keys, mouse, image)` sends the complete held keys
  and actions: omitted ones are released. `mouse` is sent only when given, so
  the runtime keeps the pointer's position and buttons until a step changes
  them (a partial dict changes only its fields). `select` (kept as
  `selection`), `game: true` (unless `game=False`) and the `events` types
  given to the constructor ride along on every step.
- After a step, `values` maps each `OnAgentObserve` key to its values in UID
  order and `value(key)` gives the first; `actor(name_or_uid)` searches the
  step's actors, `events` lists its events, `errors` accumulates failing
  observers and `frames` counts stepped frames.
- `key(name, frames)` and `press(action, frames)` hold a key or button action
  and release it for one frame; `click(x, y, button)` presses and releases at
  a screen point. `world_to_screen(x, y, camera, zoom)` and `click_world` map
  world points as `Camera.WorldToScreen` does (100 pixels per unit at zoom 1,
  centred on the screen); the camera position comes from the game, since
  observations do not report it, and `zoom` defaults to `rendering.config`.
- `image=name` (or `shot(name)`) captures the frame at the project's
  resolution to `out/image_prefix + name`, adding `.png` when missing.
  `tiles(actor_uid, component_key, layer, cells)` reads tilemap cells without
  advancing; `request` and `reset` reach the protocol directly.

`tests/agent_session_test.py` drives `tests/fixtures/agent_session` through
each of these. PocketTerraria's `tools/tbot.py` and VillageRim's
`tools/playtest.py` in the games repository build their players on it.
