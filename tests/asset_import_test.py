#!/usr/bin/env python3
"""pocket import: Aseprite sheets and LDtk levels become assets the runtime loads."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

REPOSITORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPOSITORY / "clients/python"))
from pocket2d import Runtime  # noqa: E402


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def write_png(path, width, height):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    rows = b"".join(b"\x00" + b"".join(bytes((x * 13 % 256, y * 29 % 256, 128, 255)) for x in range(width))
                    for y in range(height))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def pocket(game, *arguments, expect=0):
    result = subprocess.run([sys.executable, str(REPOSITORY / "tools/pocket"), "--game", game, *arguments],
                            capture_output=True, text=True, timeout=120)
    check(result.returncode == expect, "pocket %s exited %d:\n%s%s" % (" ".join(arguments), result.returncode,
                                                                       result.stdout, result.stderr))
    return result.stdout + result.stderr


def aseprite_json(image, frames, tags):
    return {"frames": frames, "meta": {"image": image, "size": {"w": 64, "h": 32}, "frameTags": tags}}


def grid_frames(count, duration):
    return [{"frame": {"x": (i % 4) * 16, "y": (i // 4) * 16, "w": 16, "h": 16}, "rotated": False,
             "trimmed": False, "duration": duration} for i in range(count)]


def ldtk_project(tiles, walls):
    return {"defs": {"tilesets": [{"uid": 7, "identifier": "Terrain", "relPath": "terrain.png",
                                   "tileGridSize": 8, "spacing": 0, "padding": 0}]},
            "levels": [{"identifier": "Level_0", "layerInstances": [
                {"__identifier": "Walls", "__type": "AutoLayer", "__cWid": 10, "__cHei": 6, "__gridSize": 8,
                 "__tilesetDefUid": 7, "autoLayerTiles": walls, "gridTiles": []},
                {"__identifier": "Ground", "__type": "Tiles", "__cWid": 10, "__cHei": 6, "__gridSize": 8,
                 "__tilesetDefUid": 7, "gridTiles": tiles, "autoLayerTiles": []}]}]}


def run(game):
    with tempfile.TemporaryDirectory(prefix="pocket-import-") as directory:
        root = Path(directory)
        project = root / "game"
        pocket(game, "new", str(project))
        sources = root / "sources"
        sources.mkdir()

        # Aseprite: array and hash exports, tags with directions and repeat.
        write_png(sources / "hero.png", 64, 32)
        frames = grid_frames(8, 100)
        frames[5]["duration"] = 50
        tags = [{"name": "walk", "from": 0, "to": 3, "direction": "forward"},
                {"name": "swing", "from": 4, "to": 6, "direction": "pingpong", "repeat": "1"}]
        (sources / "hero.json").write_text(json.dumps(aseprite_json("hero.png", frames, tags)))
        check("wrote animations/hero.animation.json" in
              pocket(game, "import", "aseprite", str(sources / "hero.json"), "--project", str(project), "--name", "hero"),
              "aseprite import")
        animation = json.loads((project / "animations/hero.animation.json").read_text())
        check((animation["columns"], animation["rows"], animation["tile_width_px"]) == (4, 2, 16), "atlas grid")
        walk, swing = animation["clips"]
        check(walk["loop"] and [(f["row"], f["column"], f["ticks"]) for f in walk["frames"]] ==
              [(1, 1, 6), (1, 2, 6), (1, 3, 6), (1, 4, 6)], "forward clip with 100 ms = 6 ticks")
        check(not swing["loop"] and [(f["row"], f["column"], f["ticks"]) for f in swing["frames"]] ==
              [(2, 1, 6), (2, 2, 3), (2, 3, 6), (2, 2, 3)], "pingpong clip plays once with 50 ms = 3 ticks")
        hashed = aseprite_json("hero.png", {"hero %d.ase" % i: frame for i, frame in enumerate(grid_frames(8, 100))}, [])
        (sources / "hash.json").write_text(json.dumps(hashed))
        pocket(game, "import", "aseprite", str(sources / "hash.json"), "--project", str(project), "--name", "all")
        all_frames = json.loads((project / "animations/all.animation.json").read_text())["clips"]
        check([clip["name"] for clip in all_frames] == ["default"] and len(all_frames[0]["frames"]) == 8,
              "untagged sheets become one default clip")
        trimmed = grid_frames(8, 100)
        trimmed[2]["trimmed"] = True
        (sources / "trimmed.json").write_text(json.dumps(aseprite_json("hero.png", trimmed, [])))
        check("trimmed" in pocket(game, "import", "aseprite", str(sources / "trimmed.json"), "--project", str(project),
                                  "--name", "bad", expect=1), "trimmed frames are refused")

        # LDtk: a tiles layer for ground and an auto-layer for walls.
        write_png(sources / "terrain.png", 32, 16)
        tiles = [{"px": [x * 8, y * 8], "src": [0, 0], "t": (x + y) % 8, "f": 0} for y in range(6) for x in range(10)]
        walls = [{"px": [x * 8, 0], "src": [0, 8], "t": 5, "f": 0} for x in range(10)]
        (sources / "world.ldtk").write_text(json.dumps(ldtk_project(tiles, walls)))
        pocket(game, "import", "ldtk", str(sources / "world.ldtk"), "--project", str(project), "--name", "level0",
               "--ground", "Ground", "--solid", "Walls")
        tilemap = json.loads((project / "maps/level0.tilemap.json").read_text())
        check((tilemap["width"], tilemap["height"]) == (10, 6) and tilemap["atlas"]["columns"] == 4 and
              tilemap["atlas"]["rows"] == 2, "map and atlas size")
        check(tilemap["layers"]["ground"]["cells"][2 * 10 + 3] == (3 + 2) % 8 + 1, "LDtk tile t becomes id t+1")
        check(tilemap["layers"]["solid"]["cells"][:10] == [6] * 10 and tilemap["layers"]["solid"]["cells"][10] == 0,
              "auto-layer walls become solid cells")
        flipped = [dict(tile, f=1) for tile in tiles[:1]] + tiles[1:]
        (sources / "flipped.ldtk").write_text(json.dumps(ldtk_project(flipped, walls)))
        check("flipped" in pocket(game, "import", "ldtk", str(sources / "flipped.ldtk"), "--project", str(project),
                                  "--name", "bad", "--ground", "Ground", expect=1), "flipped tiles are refused")

        # The runtime loads both assets.
        (project / "scenes/main.scene").write_text(json.dumps({"actors": [
            {"uid": 1, "name": "hero", "components": {
                "transform": {"type": "Transform"},
                "sprite": {"type": "SpriteRenderer", "sprite": "hero.png"},
                "animation": {"type": "SpriteAnimation", "animation_resource": "hero.animation.json",
                              "renderer_key": "sprite", "clip": "walk"}}},
            {"uid": 2, "name": "world", "components": {
                "transform": {"type": "Transform"},
                "map": {"type": "Tilemap", "map_resource": "level0.tilemap.json"}}}]}))
        report = [json.loads(line) for line in pocket(game, "check", str(project), "--json").splitlines()
                  if line.startswith("{")]
        check(not [entry for entry in report if entry["severity"] == "error"], "imported assets check: %r" % report)
        with Runtime(game, project, save_dir=root / "saves") as runtime:
            observed = runtime.request({"cmd": "observe", "tilemaps": [
                {"actor_uid": "2", "component_key": "map", "layer": "ground", "cells": [[3, 2]]},
                {"actor_uid": "2", "component_key": "map", "layer": "solid", "cells": [[4, 0]]}]})
            check([query["cells"][0]["tile"] for query in observed["tilemaps"]] == [6, 6],
                  "the runtime reads imported cells: %r" % observed["tilemaps"])
    print("asset import regressions passed")


if __name__ == "__main__":
    check(len(sys.argv) == 2, "usage: asset_import_test.py RUNTIME")
    run(str(Path(sys.argv[1]).resolve()))
