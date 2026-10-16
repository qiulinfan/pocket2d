"""Several PocketEnv instances stepped in parallel, Gymnasium 1.x style."""
from concurrent.futures import ThreadPoolExecutor

from .env import PocketEnv
from .scenario import Scenario


class VectorPocketEnv:
    """num_envs runtime processes running one scenario.

    reset(seed) -> (observations, infos); step(actions) -> (observations,
    rewards, terminations, truncations, infos), each a list with one entry
    per sub-environment (observations stay protocol dictionaries, so nothing
    is stacked into arrays). Autoreset follows Gymnasium 1.x's default
    next-step mode: after a sub-environment terminates or truncates, its next
    step() call resets it, ignores that action, and returns the reset
    observation with reward 0 and both flags false; infos[i]["autoreset"]
    marks it. Seeds: an int s gives sub-environment i seed s + i.
    """

    def __init__(self, executable, project, scenario, num_envs, save_dir=None, timeout=60.0):
        if num_envs < 1:
            raise ValueError("num_envs must be positive")
        self.scenario = scenario if isinstance(scenario, Scenario) else Scenario.load(scenario)
        self.num_envs = num_envs
        self.envs = []
        try:
            for index in range(num_envs):
                directory = None if save_dir is None else "%s/env%d" % (save_dir, index)
                self.envs.append(PocketEnv(executable, project, self.scenario, save_dir=directory, timeout=timeout))
        except Exception:
            self.close()
            raise
        self._pool = ThreadPoolExecutor(max_workers=num_envs)
        self._needs_reset = [False] * num_envs
        self._seeds = [None] * num_envs

    def _map(self, function, *columns):
        return list(self._pool.map(function, *columns))

    def reset(self, *, seed=None, options=None):
        if seed is None:
            seeds = [None] * self.num_envs
        elif isinstance(seed, int):
            seeds = [seed + index for index in range(self.num_envs)]
        else:
            seeds = list(seed)
            if len(seeds) != self.num_envs:
                raise ValueError("one seed per sub-environment")
        self._seeds = seeds
        results = self._map(lambda env, s: env.reset(seed=s, options=options), self.envs, seeds)
        self._needs_reset = [False] * self.num_envs
        return [r[0] for r in results], [r[1] for r in results]

    def step(self, actions):
        if len(actions) != self.num_envs:
            raise ValueError("one action per sub-environment")

        def advance(index, action):
            env = self.envs[index]
            if self._needs_reset[index]:
                observation, info = env.reset(seed=self._seeds[index])
                info = dict(info, autoreset=True)
                return observation, 0.0, False, False, info
            observation, reward, terminated, truncated, info = env.step(action)
            return observation, reward, terminated, truncated, dict(info, autoreset=False)

        results = self._map(advance, range(self.num_envs), actions)
        self._needs_reset = [r[2] or r[3] for r in results]
        return ([r[0] for r in results], [r[1] for r in results], [r[2] for r in results],
                [r[3] for r in results], [r[4] for r in results])

    def close(self):
        for env in self.envs:
            env.close()
        if hasattr(self, "_pool"):
            self._pool.shutdown(wait=True)
