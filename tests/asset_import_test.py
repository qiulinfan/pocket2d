#!/usr/bin/env python3
"""pocket import: Aseprite sheets, LDtk levels and Unity 2D scenes become assets the runtime loads."""
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
                            capture_output=True, text=True, encoding="utf-8", timeout=120)
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


GUIDS = {"tiles": "a" * 31 + "1", "rock": "a" * 31 + "2", "hero": "a" * 31 + "3",
         "wall": "a" * 31 + "4", "floor": "a" * 31 + "5"}


def sprite_meta(guid, sprites=None, alignment=0):
    """A texture importer .meta: single sprite, or a sheet of (name, internal id, x)."""
    text = "fileFormatVersion: 2\nguid: %s\nTextureImporter:\n" % guid
    if not sprites:
        return text + "  spriteMode: 1\n  alignment: %d\n  spritePixelsToUnits: 16\n" % alignment
    text += "  spriteMode: 2\n  spritePixelsToUnits: 16\n  spriteSheet:\n    serializedVersion: 2\n    sprites:\n"
    for name, internal, x in sprites:
        text += ("    - serializedVersion: 2\n      name: %s\n      rect:\n        serializedVersion: 2\n"
                 "        x: %d\n        y: 0\n        width: 16\n        height: 16\n      alignment: 0\n"
                 "      pivot: {x: 0.5, y: 0.5}\n      internalID: %d\n") % (name, x, internal)
    return text


def unity_scene():
    g = GUIDS
    transform = ("--- !u!4 &%d\nTransform:\n  m_GameObject: {fileID: %d}\n  m_LocalPosition: {x: %s, y: %s, z: 0}\n"
                 "  m_LocalScale: {x: 1, y: 1, z: 1}\n  m_LocalRotation: {x: 0, y: 0, z: 0, w: 1}\n  m_Father: {fileID: %d}\n")
    game_object = "--- !u!1 &%d\nGameObject:\n  m_Name: %s\n  m_IsActive: 1\n"
    tile = ("  - first: {x: %d, y: 0, z: 0}\n    second:\n      m_TileIndex: %d\n      m_TileSpriteIndex: %d\n"
            "      m_TileMatrixIndex: 0\n      m_TileColorIndex: 0\n")
    reference = "  - m_RefCount: 1\n    m_Data: {fileID: %d, guid: %s, type: %d}\n"
    return ("%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n" +
            game_object % (10, "Grid") + transform % (11, 10, 0, 0, 0) +
            game_object % (20, "Walls") + transform % (21, 20, 2, 3, 11) +
            "--- !u!1839735485 &22\nTilemap:\n  m_GameObject: {fileID: 20}\n  m_Enabled: 1\n  m_Tiles:\n" +
            tile % (0, 0, 0) + tile % (1, 1, 1) +
            "  m_TileAssetArray:\n" + reference % (11400000, g["wall"], 2) + reference % (11400000, g["floor"], 2) +
            "  m_TileSpriteArray:\n" + reference % (101, g["tiles"], 3) + reference % (102, g["tiles"], 3) +
            "  m_TileMatrixArray:\n  - m_RefCount: 2\n    m_Data:\n      e00: 1\n      e01: 0\n      e10: 0\n"
            "      e11: 1\n  m_TileColorArray:\n  - m_RefCount: 2\n    m_Data: {r: 1, g: 1, b: 1, a: 1}\n"
            "--- !u!483693784 &23\nTilemapRenderer:\n  m_GameObject: {fileID: 20}\n  m_Enabled: 1\n  m_SortingOrder: 2\n"
            "--- !u!19719996 &24\nTilemapCollider2D:\n  m_GameObject: {fileID: 20}\n  m_Enabled: 1\n" +
            game_object % (70, "Spring") + transform % (71, 70, 2, 6, 11) +
            "--- !u!1839735485 &72\nTilemap:\n  m_GameObject: {fileID: 70}\n  m_Enabled: 1\n  m_Tiles:\n" +
            tile % (0, 1, 1) +
            "  m_TileAssetArray:\n" + reference % (11400000, g["wall"], 2) + reference % (11400000, g["floor"], 2) +
            "  m_TileSpriteArray:\n" + reference % (101, g["tiles"], 3) + reference % (102, g["tiles"], 3) +
            "  m_TileMatrixArray:\n  - m_RefCount: 1\n    m_Data:\n      e00: 1\n      e01: 0\n      e10: 0\n"
            "      e11: 1\n  m_TileColorArray:\n  - m_RefCount: 1\n    m_Data: {r: 1, g: 1, b: 1, a: 1}\n"
            "--- !u!483693784 &73\nTilemapRenderer:\n  m_GameObject: {fileID: 70}\n  m_Enabled: 0\n  m_SortingOrder: 2\n" +
            game_object % (30, "Rock") + transform % (31, 30, 5, -1, 0) +
            "--- !u!212 &32\nSpriteRenderer:\n  m_GameObject: {fileID: 30}\n  m_Enabled: 1\n  m_SortingOrder: 0\n"
            "  m_Sprite: {fileID: 21300000, guid: %s, type: 3}\n  m_Color: {r: 1, g: 1, b: 1, a: 1}\n" % g["rock"] +
            "--- !u!61 &33\nBoxCollider2D:\n  m_GameObject: {fileID: 30}\n  m_Enabled: 1\n  m_IsTrigger: 0\n"
            "  m_Size: {x: 1, y: 2}\n  m_Offset: {x: 0, y: 1}\n" +
            game_object % (50, "spawnPoint1") + transform % (51, 50, 7, 2, 0) +
            game_object % (60, "spawnPoint2") + transform % (61, 60, 8, 2, 0) +
            "--- !u!212 &62\nSpriteRenderer:\n  m_GameObject: {fileID: 60}\n  m_Enabled: 1\n"
            "  m_Sprite: {fileID: 21300000, guid: %s, type: 3}\n" % g["rock"] +
            game_object % (40, "Hero") + transform % (41, 40, 0, -3, 0) +
            "--- !u!50 &42\nRigidbody2D:\n  m_GameObject: {fileID: 40}\n  m_BodyType: 0\n"
            "--- !u!212 &43\nSpriteRenderer:\n  m_GameObject: {fileID: 40}\n  m_Enabled: 1\n"
            "  m_Sprite: {fileID: 21300000, guid: %s, type: 3}\n" % g["hero"] +
            (game_object % (80, "Sleeper")).replace("m_IsActive: 1", "m_IsActive: 0") + transform % (81, 80, 4, -3, 0) +
            "--- !u!50 &82\nRigidbody2D:\n  m_GameObject: {fileID: 80}\n  m_BodyType: 0\n"
            "--- !u!212 &83\nSpriteRenderer:\n  m_GameObject: {fileID: 80}\n  m_Enabled: 1\n"
            "  m_Sprite: {fileID: 21300000, guid: %s, type: 3}\n" % g["hero"])


def run_unity(game, root):
    assets = root / "unity/Assets"
    for folder in ("Art", "Art/Props", "Art/Heroes", "Tiles", "Scenes"):
        (assets / folder).mkdir(parents=True, exist_ok=True)
    write_png(assets / "Art/tiles.png", 32, 16)
    (assets / "Art/tiles.png.meta").write_text(sprite_meta(GUIDS["tiles"], [("tiles_0", 101, 0), ("tiles_1", 102, 16)]))
    write_png(assets / "Art/Props/rock.png", 16, 32)
    (assets / "Art/Props/rock.png.meta").write_text(sprite_meta(GUIDS["rock"], alignment=7))
    write_png(assets / "Art/Heroes/hero.png", 16, 16)
    (assets / "Art/Heroes/hero.png.meta").write_text(sprite_meta(GUIDS["hero"]))
    for name, collider in (("wall", 1), ("floor", 0)):
        (assets / ("Tiles/%s.asset" % name)).write_text(
            "%%YAML 1.1\n--- !u!114 &11400000\nMonoBehaviour:\n  m_ColliderType: %d\n" % collider)
        (assets / ("Tiles/%s.asset.meta" % name)).write_text("fileFormatVersion: 2\nguid: %s\n" % GUIDS[name])
    (assets / "Scenes/test.unity").write_text(unity_scene())
    project = root / "unity_game"
    (project / "component_types").mkdir(parents=True)
    (project / "game.config").write_text('{"initial_scene":"imported"}')
    (project / "component_types/SpawnMarker.lua").write_text('SpawnMarker = {kind = ""}')
    pocket(game, "import", "unity", str(assets / "Scenes/test.unity"), "--assets", str(assets), "--project", str(project),
           "--name", "imported", "--point-prefix", "spawnPoint")
    scene_actors = json.loads((project / "scenes/imported.scene").read_text())["actors"]
    actors = {actor["name"]: actor["components"] for actor in scene_actors}
    visible_points = [actor["components"] for actor in scene_actors if actor["name"] == "spawnPoint2"]
    check(len(visible_points) == 1 and "sprite" in visible_points[0] and visible_points[0]["spawn"]["kind"] == "point",
          "a point with a sprite is one actor with both: %r" % visible_points)
    walls = json.loads((project / "maps/Walls_1.tilemap.json").read_text())
    check((walls["width"], walls["height"]) == (2, 1) and walls["layers"]["solid"]["cells"] == [1, 0] and
          walls["layers"]["ground"]["cells"] == [0, 2], "Tile asset colliders pick solid vs ground: %r" % walls["layers"])
    check(actors["Spring_1"]["map"].get("enabled") is False and "enabled" not in actors["Walls_1"]["map"],
          "a tilemap with its renderer off is imported disabled: %r" % actors["Spring_1"]["map"])
    check(actors["Walls_1"]["transform"]["x"] == 2 and actors["Walls_1"]["transform"]["y"] == -4,
          "tilemap origin flips y: %r" % actors["Walls_1"]["transform"])
    rock = actors["Rock"]["sprite"]
    check(rock["scale_x"] == 6.25 and rock["pivot_y"] == 1.0 and rock["auto_sorting_order"],
          "sprites keep 16 PPU scale, bottom pivot and y-sorting: %r" % rock)
    body = actors["Rock_collider"]["body"]
    check((body["body_type"], body["x"], body["width"], body["height"]) == ("static", 5, 1, 2) and abs(body["y"]) < 1e-9,
          "box colliders become static bodies with their offset: %r" % body)
    check(actors["spawnPoint1"]["spawn"]["kind"] == "point" and actors["spawnPoint1"]["transform"]["x"] == 7 and
          actors["spawnPoint1"]["transform"]["y"] == -2, "named empty objects become points: %r" % actors["spawnPoint1"])
    check(actors["Hero"]["spawn"]["kind"] == "Heroes" and actors["Hero"]["transform"]["y"] == 3,
          "Rigidbody2D entities become spawn markers: %r" % actors["Hero"])
    check("enabled" not in actors["Hero"]["spawn"] and actors["Sleeper"]["spawn"].get("enabled") is False and
          actors["Sleeper"]["sprite"].get("enabled") is False and actors["Sleeper"]["spawn"]["kind"] == "Heroes",
          "an inactive entity is imported disabled: %r" % actors["Sleeper"])
    with Runtime(game, project, save_dir=root / "unity_saves") as runtime:
        uid = next(a["uid"] for a in json.loads((project / "scenes/imported.scene").read_text())["actors"]
                   if a["name"] == "Walls_1")
        observed = runtime.request({"cmd": "observe", "tilemaps": [
            {"actor_uid": str(uid), "component_key": "map", "layer": "solid", "cells": [[0, 0], [1, 0]]}]})
        check([cell["tile"] for cell in observed["tilemaps"][0]["cells"]] == [1, 0], "the runtime loads the import")


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
        run_unity(game, root)
    print("asset import regressions passed")


if __name__ == "__main__":
    check(len(sys.argv) == 2, "usage: asset_import_test.py RUNTIME")
    run(str(Path(sys.argv[1]).resolve()))
