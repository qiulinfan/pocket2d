"""A scripted player's session over the real runtime: the protocol plumbing
that game playtests share, with the game's rules left to a subclass."""
import base64
import json
import shutil
import tempfile
from pathlib import Path

from .cli import REPOSITORY, find_game
from .env import find_actor
from .runtime import Runtime

ENGINE_ROOT = REPOSITORY


def rendering(project):
    """(width, height, zoom_factor) from a project's rendering.config, with the engine defaults."""
    path = Path(project) / "rendering.config"
    data = json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {}
    return int(data.get("x_resolution", 640)), int(data.get("y_resolution", 360)), float(data.get("zoom_factor", 1.0))


class Session:
    """One headless runtime for `project` and the input a scripted player holds.

    The runtime binary comes from `executable`, POCKET_GAME, or this engine
    checkout's build (see cli.find_game). Without `save_dir` the game saves
    into a fresh temporary directory, removed on close.

    step() sends the complete held keys and actions (omitted ones are
    released), while the mouse is sent only when given: the runtime keeps its
    position and buttons until a step passes new ones. Every step carries `select` (kept as `selection`),
    `game: true` (unless game=False) and the `events` types given here. After it, `response` is
    the reply, `values` maps each game-declared observation key
    (OnAgentObserve) to its values in UID order, `events` lists the step's
    events, `errors` collects failing observers and `frames` counts the frames
    stepped since launch. Images are saved as PNG under `out`, named
    image_prefix + name.
    """

    image_prefix = ""

    def __init__(self, project, seed=0, out=None, select=None, game=True, events=(), save_dir=None,
                 executable=None, timeout=60.0, extra_args=()):
        self.project = Path(project).resolve()
        self.screen_width, self.screen_height, self.zoom = rendering(self.project)
        self.out = Path(out) if out is not None else None
        self.selection, self.observe_game, self.event_types = select, game, list(events)
        self._temporary = None if save_dir is not None else tempfile.mkdtemp(prefix="pocket2d-saves-")
        self.save_dir = Path(save_dir if save_dir is not None else self._temporary)
        try:
            self.runtime = Runtime(executable or find_game(None), self.project, seed=seed, save_dir=self.save_dir,
                                   timeout=timeout, extra_args=extra_args)
        except BaseException:
            self._remove_temporary()
            raise
        self.frames = 0
        self.response, self.values, self.events, self.errors = {}, {}, [], set()

    # Protocol ------------------------------------------------------------------
    def request(self, command, check=True):
        return self.runtime.request(command, check=check)

    def step(self, actions=None, frames=1, keys=None, mouse=None, image=None):
        command = {"cmd": "step", "frames": frames, "actions": actions or {}, "keys": keys or []}
        if mouse is not None:
            command["mouse"] = mouse
        if self.selection is not None:
            command["select"] = self.selection
        if self.observe_game:
            command["game"] = True
        if self.event_types:
            command["events"] = self.event_types
        if image:
            command["image"] = {"width": self.screen_width, "height": self.screen_height}
        response = self.request(command)
        self.frames += frames
        self.response, self.values = response, {}
        for entry in response.get("game", []):
            if entry.get("error"):
                self.errors.add("%s: %s" % (entry["key"], entry["error"][:200]))
                continue
            self.values.setdefault(entry["key"], []).append(entry.get("value"))
        self.events = response.get("events", [])
        if image:
            self.save_image(response, image)
        return response

    def reset(self, seed=None):
        """A fresh runtime state (same seed unless given); held input and the mouse are released."""
        return self.request({"cmd": "reset"} if seed is None else {"cmd": "reset", "seed": seed})

    # Observations --------------------------------------------------------------
    def value(self, key, default=None):
        """The first value a game observer published under `key` in the last step."""
        found = self.values.get(key)
        return found[0] if found else default

    def actor(self, reference):
        """An actor of the last step's observation by UID string or name, or None."""
        return find_actor(self.response, reference)

    def tiles(self, actor_uid, component_key, layer, cells):
        """Tile ids at [x, y] cells of a tilemap layer, without advancing time."""
        response = self.request({"cmd": "observe", "select": {"limit": 0}, "tilemaps": [
            {"actor_uid": str(actor_uid), "component_key": component_key, "layer": layer, "cells": cells}]})
        return [cell["tile"] for cell in response["tilemaps"][0]["cells"]]

    def image_path(self, name):
        """out/image_prefix+name, with .png when name has no suffix."""
        if self.out is None:
            raise ValueError("Session needs out= to save images")
        path = self.out / (self.image_prefix + name)
        return path if path.suffix else path.with_suffix(".png")

    def save_image(self, response, name):
        """Writes a response's captured frame to image_path(name) and returns that path."""
        path = self.image_path(name)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(base64.b64decode(response["image"]["data"]))
        return path

    def shot(self, name):
        """Steps one frame, saves its image and returns the path."""
        self.step(image=name)
        return self.image_path(name)

    # Input ---------------------------------------------------------------------
    def key(self, name, frames=1):
        """Holds a key for `frames`, then releases it for one frame."""
        self.step(keys=[name], frames=frames)
        self.step()

    def press(self, action, frames=1):
        """Holds a button action for `frames`, then releases it for one frame."""
        self.step({action: True}, frames=frames)
        self.step()

    def click(self, x, y, button=1):
        """Presses and releases a mouse button at a screen point, one frame each."""
        self.step(mouse={"x": x, "y": y, "buttons": [button]})
        self.step(mouse={"x": x, "y": y, "buttons": []})

    def world_to_screen(self, x, y, camera, zoom=None):
        """The screen point of world point (x, y) under a camera centred at `camera`
        (x, y) with `zoom` (default: rendering.config zoom_factor), as the engine's
        Camera.WorldToScreen computes it: 100 pixels per unit at zoom 1."""
        scale = 100.0 * (self.zoom if zoom is None else zoom)
        return self.screen_width / 2 + (x - camera[0]) * scale, self.screen_height / 2 + (y - camera[1]) * scale

    def click_world(self, x, y, camera, zoom=None, button=1):
        self.click(*self.world_to_screen(x, y, camera, zoom), button=button)

    # Lifetime ------------------------------------------------------------------
    def _remove_temporary(self):
        if self._temporary is not None:
            shutil.rmtree(self._temporary, ignore_errors=True)
            self._temporary = None

    def close(self):
        try:
            self.runtime.close()
        finally:
            self._remove_temporary()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
