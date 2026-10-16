"""Import Aseprite sprite sheets and LDtk levels into Pocket2D assets.

Both importers refuse inputs they cannot represent exactly instead of
approximating them: Pocket2D atlases are uniform grids without padding,
animations run at 60 ticks per second, and tilemaps have one ground and one
solid layer drawn from one atlas.
"""
import json
import shutil
import struct
from pathlib import Path

TICKS_PER_SECOND = 60


class ImportError_(ValueError):
    """An input that cannot be imported exactly."""


def _require(condition, message):
    if not condition:
        raise ImportError_(message)


def png_size(path):
    """(width, height) of a PNG file."""
    data = Path(path).read_bytes()[:24]
    _require(data[:8] == b"\x89PNG\r\n\x1a\n" and data[12:16] == b"IHDR", "%s is not a PNG image" % path)
    return struct.unpack(">II", data[16:24])


def _copy_image(source, project, image_name):
    target = project / "images" / image_name
    target.parent.mkdir(parents=True, exist_ok=True)
    if source.resolve() != target.resolve():
        shutil.copyfile(source, target)
    return target


def _write_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, separators=(",", ":")) + "\n", encoding="utf-8")
    return path


# ---------------------------------------------------------------------------
# Aseprite (File > Export Sprite Sheet, JSON data, array or hash)

def _ticks(duration_ms):
    return max(1, round(duration_ms * TICKS_PER_SECOND / 1000.0))


def import_aseprite(json_path, project, name, image_name=None):
    """Writes animations/NAME.animation.json and copies the sheet to images/.

    Each frame tag becomes a clip (forward, reverse, pingpong and
    pingpong_reverse directions; a finite "repeat" makes the clip play once);
    without tags all frames form one looping clip named "default". Frames
    must lie on a uniform grid without padding or trimming.
    """
    json_path, project = Path(json_path), Path(project)
    data = json.loads(json_path.read_text(encoding="utf-8"))
    frames = data.get("frames")
    _require(isinstance(frames, (list, dict)) and frames, "no frames in Aseprite JSON")
    frames = list(frames.values()) if isinstance(frames, dict) else frames
    meta = data.get("meta", {})
    sheet = json_path.parent / meta.get("image", "")
    _require(sheet.is_file(), "sprite sheet image not found: %s" % sheet)
    sheet_width, sheet_height = png_size(sheet)
    width, height = frames[0]["frame"]["w"], frames[0]["frame"]["h"]
    _require(width > 0 and height > 0, "frames must have a size")
    _require(sheet_width % width == 0 and sheet_height % height == 0,
             "sheet size must be a multiple of the frame size (no borders or padding)")
    cells = []
    for index, frame in enumerate(frames):
        box = frame["frame"]
        _require(box["w"] == width and box["h"] == height, "frame %d differs in size; export without trimming" % index)
        _require(not frame.get("rotated") and not frame.get("trimmed"), "frame %d is rotated or trimmed" % index)
        _require(box["x"] % width == 0 and box["y"] % height == 0, "frame %d is not on the grid (padding?)" % index)
        cells.append({"row": box["y"] // height + 1, "column": box["x"] // width + 1,
                      "ticks": _ticks(frame.get("duration", 100))})

    def clip(clip_name, order, loop):
        _require(1 <= len(clip_name.encode("utf-8")) <= 128, "clip name must be 1..128 bytes")
        return {"name": clip_name, "loop": loop, "frames": [dict(cells[i]) for i in order]}

    clips = []
    tags = meta.get("frameTags") or []
    names = set()
    for tag in tags:
        start, end = tag["from"], tag["to"]
        _require(0 <= start <= end < len(cells), "tag %s has an invalid frame range" % tag.get("name"))
        _require(tag["name"] not in names, "duplicate tag name %s" % tag["name"])
        names.add(tag["name"])
        forward = list(range(start, end + 1))
        direction = tag.get("direction", "forward")
        if direction == "forward":
            order = forward
        elif direction == "reverse":
            order = forward[::-1]
        elif direction == "pingpong":
            order = forward + forward[-2:0:-1]
        elif direction == "pingpong_reverse":
            backward = forward[::-1]
            order = backward + backward[-2:0:-1]
        else:
            raise ImportError_("unsupported tag direction %r" % direction)
        repeat = tag.get("repeat")
        loop = repeat in (None, "", 0, "0")
        clips.append(clip(tag["name"], order, loop))
    if not clips:
        clips.append(clip("default", range(len(cells)), True))
    image_name = image_name or sheet.name
    _copy_image(sheet, project, image_name)
    animation = {"version": 1, "image": image_name, "columns": sheet_width // width, "rows": sheet_height // height,
                 "tile_width_px": width, "tile_height_px": height, "clips": clips}
    return _write_json(project / "animations" / (name + ".animation.json"), animation)


# ---------------------------------------------------------------------------
# LDtk (project .ldtk with embedded levels)

def _find_level(data, level_name):
    levels = data.get("levels") or []
    _require(levels, "the LDtk project has no levels")
    if level_name is None:
        return levels[0]
    for level in levels:
        if level.get("identifier") == level_name:
            return level
    raise ImportError_("level %r not found; levels: %s" % (level_name, ", ".join(l.get("identifier", "?") for l in levels)))


def _layer(level, layer_name):
    layers = level.get("layerInstances")
    _require(layers is not None, "level %s stores layers externally; save the project with embedded levels"
             % level.get("identifier"))
    for layer in layers:
        if layer.get("__identifier") == layer_name:
            _require(layer.get("__type") in ("Tiles", "AutoLayer", "IntGrid"),
                     "layer %s is %s, not a tile layer" % (layer_name, layer.get("__type")))
            return layer
    raise ImportError_("layer %r not found in level %s" % (layer_name, level.get("identifier")))


def _tileset(data, layer):
    uid = layer.get("__tilesetDefUid")
    _require(uid is not None, "layer %s has no tileset" % layer["__identifier"])
    for tileset in data.get("defs", {}).get("tilesets", []):
        if tileset.get("uid") == uid:
            return tileset
    raise ImportError_("tileset %s not found" % uid)


def _cells(layer, columns_in_tileset):
    width, height, grid = layer["__cWid"], layer["__cHei"], layer["__gridSize"]
    cells = [0] * (width * height)
    tiles = layer.get("gridTiles", []) + layer.get("autoLayerTiles", [])
    for tile in tiles:
        _require(tile.get("f", 0) == 0, "flipped tiles are not supported (layer %s)" % layer["__identifier"])
        x, y = tile["px"]
        _require(x % grid == 0 and y % grid == 0, "tile at %r is not on the layer grid" % (tile["px"],))
        column, row = x // grid, y // grid
        if 0 <= column < width and 0 <= row < height:
            # Later tiles draw on top in LDtk; keep the topmost.
            cells[row * width + column] = tile["t"] + 1
    return cells


def import_ldtk(ldtk_path, project, name, ground, solid=None, level_name=None, image_name=None):
    """Writes maps/NAME.tilemap.json from one level and copies its tileset.

    `ground` and `solid` name Tiles or AutoLayer layers (an IntGrid layer
    with auto-tiles works too); solid cells also collide. Both layers must
    use the same tileset, without spacing or padding, on the level grid.
    """
    ldtk_path, project = Path(ldtk_path), Path(project)
    data = json.loads(ldtk_path.read_text(encoding="utf-8"))
    level = _find_level(data, level_name)
    ground_layer = _layer(level, ground)
    solid_layer = _layer(level, solid) if solid else None
    tileset = _tileset(data, ground_layer)
    if solid_layer is not None:
        _require(_tileset(data, solid_layer).get("uid") == tileset.get("uid"), "ground and solid layers use different tilesets")
        _require((solid_layer["__cWid"], solid_layer["__cHei"]) == (ground_layer["__cWid"], ground_layer["__cHei"]),
                 "ground and solid layers have different sizes")
    _require(tileset.get("spacing", 0) == 0 and tileset.get("padding", 0) == 0, "tileset spacing/padding is not supported")
    grid = tileset["tileGridSize"]
    _require(ground_layer["__gridSize"] == grid, "the layer grid differs from the tileset grid")
    rel = tileset.get("relPath")
    _require(rel, "tileset %s has no image" % tileset.get("identifier"))
    image = (ldtk_path.parent / rel).resolve()
    _require(image.is_file(), "tileset image not found: %s" % image)
    image_width, image_height = png_size(image)
    _require(image_width % grid == 0 and image_height % grid == 0, "tileset image is not a whole number of tiles")
    columns, rows = image_width // grid, image_height // grid
    width, height = ground_layer["__cWid"], ground_layer["__cHei"]
    _require(1 <= width <= 512 and 1 <= height <= 512, "level must be 1..512 cells on each side")
    image_name = image_name or image.name
    _copy_image(image, project, image_name)
    tilemap = {"version": 1, "width": width, "height": height, "cell_size": 1.0,
               "atlas": {"image": image_name, "columns": columns, "rows": rows,
                         "tile_width_px": grid, "tile_height_px": grid},
               "layers": {"ground": {"sorting_order": -100, "cells": _cells(ground_layer, columns)},
                          "solid": {"sorting_order": 0,
                                    "cells": _cells(solid_layer, columns) if solid_layer else [0] * (width * height)}}}
    return _write_json(project / "maps" / (name + ".tilemap.json"), tilemap)
