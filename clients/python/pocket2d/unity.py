"""Import a Unity scene's 2D world (tilemaps, sprites, colliders, entities)
into a Pocket2D project.

  import_unity(ASSETS_DIR, SCENE.unity, PROJECT, scene_name, skip_texture_folders)

Reads Unity's text YAML (scenes and prefabs, with prefab-instance overrides and
stripped-object aliases) and each texture's importer .meta. Conversions: Unity
units stay world units (one cell = 1.0); y is negated (Pocket2D y points
down). Each Unity Tilemap becomes one root actor with a Tilemap whose atlas
packs the sprites it uses (flips and rotations baked in); tiles whose Tile
asset has a collider go to the solid layer (as full cells), others to ground.
Tiles whose sprite is not one cell become SpriteRenderer actors, and so do
active SpriteRenderers (hierarchies flattened to world transforms; flips
become negative scales). Unity sorting layers map to sorting_order = layer
index * 100000 + order, and every sprite y-sorts. Solid Box/Circle/Capsule
colliders become static Rigidbody actors (triggers are left to gameplay).
GameObjects with a non-static Rigidbody2D are entities owned by gameplay:
each imports as one SpawnMarker actor (kind = texture folder of its sprite).
Inactive objects and disabled renderers are skipped; import_report.json lists
what could not be represented exactly. Hand-authored scene actors (uid below
1000) survive re-imports. Needs Pillow.
"""
import copy
import json
import math
import re
from collections import defaultdict
from pathlib import Path

Image = None  # PIL.Image, loaded by import_unity so the rest of the client needs no Pillow

# --- Unity YAML subset -------------------------------------------------------
# Unity writes a regular YAML subset (block mappings and sequences, flow
# mappings like {fileID: 1, guid: ab, type: 3}, plain scalars); this parser
# handles exactly that subset and is not a general YAML parser.

HEADER = re.compile(r"^--- !u!(\d+) &(-?\d+)( stripped)?\s*$")


def _scalar(text):
    text = text.strip()
    if text.startswith("{") and text.endswith("}"):
        return _flow(text[1:-1])
    if text.startswith("[") and text.endswith("]"):
        inner = text[1:-1].strip()
        return [] if not inner else [_scalar(part) for part in _split(inner)]
    if len(text) >= 2 and text[0] == text[-1] and text[0] in "'\"":
        return text[1:-1]
    try:
        return int(text)
    except ValueError:
        pass
    try:
        return float(text)
    except ValueError:
        return text


def _split(text):
    parts, depth, start = [], 0, 0
    for i, ch in enumerate(text):
        if ch in "{[":
            depth += 1
        elif ch in "}]":
            depth -= 1
        elif ch == "," and depth == 0:
            parts.append(text[start:i])
            start = i + 1
    parts.append(text[start:])
    return [p for p in (part.strip() for part in parts) if p]


def _flow(text):
    result = {}
    for part in _split(text):
        key, _, value = part.partition(":")
        result[key.strip()] = _scalar(value)
    return result


def _parse_block(lines, start, indent):
    """Parses lines[start:] at `indent`; returns (value, next index)."""
    if start < len(lines) and lines[start][0] == indent and lines[start][1].startswith("- "):
        items = []
        i = start
        while i < len(lines) and lines[i][0] == indent and lines[i][1].startswith("- "):
            content = lines[i][1][2:]
            item_indent = indent + 2
            if ":" in content and not content.startswith("{") and re.match(r"^[^:{\[]+:( |$)", content):
                # A mapping item: its first key sits on the dash line.
                sub = [(item_indent, content)]
                j = i + 1
                while j < len(lines) and lines[j][0] > indent:
                    sub.append(lines[j])
                    j += 1
                value, _ = _parse_block(sub, 0, item_indent)
                items.append(value)
                i = j
            else:
                items.append(_scalar(content))
                i += 1
        return items, i
    mapping = {}
    i = start
    while i < len(lines) and lines[i][0] == indent:
        text = lines[i][1]
        key, _, rest = text.partition(":")
        rest = rest.strip()
        if rest:
            mapping[key.strip()] = _scalar(rest)
            i += 1
            continue
        # Nested block (deeper indent, or a sequence at the same indent).
        j = i + 1
        if j < len(lines) and (lines[j][0] > indent or (lines[j][0] == indent and lines[j][1].startswith("- "))):
            child_indent = lines[j][0]
            value, i = _parse_block(lines, j, child_indent)
            mapping[key.strip()] = value
        else:
            mapping[key.strip()] = None
            i = j
    return mapping, i


def parse_document(body):
    lines = []
    for raw in body.splitlines():
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        stripped = raw.lstrip(" ")
        lines.append((len(raw) - len(stripped), stripped.rstrip()))
    if not lines:
        return {}
    value, _ = _parse_block(lines, 0, lines[0][0])
    return value


class UnityFile:
    """Objects of one .unity/.prefab file: {file_id: (class_id, type_name, fields, stripped)}."""

    def __init__(self, path):
        self.path = Path(path)
        self.objects = {}
        text = self.path.read_text(encoding="utf-8", errors="replace")
        chunks = re.split(r"^(?=--- !u!)", text, flags=re.M)
        for chunk in chunks:
            first, _, body = chunk.partition("\n")
            match = HEADER.match(first)
            if not match:
                continue
            parsed = parse_document(body)
            type_name, fields = next(iter(parsed.items())) if parsed else ("", {})
            self.objects[int(match.group(2))] = (int(match.group(1)), type_name, fields or {}, bool(match.group(3)))

    def of_type(self, class_id):
        return [(fid, obj) for fid, obj in self.objects.items() if obj[0] == class_id]


def apply_modification(fields, path, value):
    """Applies one prefab modification (e.g. m_Tiles.Array.data[3].first.x)."""
    parts = path.split(".")
    node = fields
    parent, parent_key = None, None
    i = 0
    while i < len(parts):
        part = parts[i]
        last = i == len(parts) - 1
        if part == "Array":
            if not isinstance(node, list):
                # Unity writes empty arrays and maps as {}; an override makes it a list.
                if node or parent is None:
                    return
                node = []
                parent[parent_key] = node
            nxt = parts[i + 1]
            if nxt == "size":
                size = int(value)
                if not isinstance(node, list):
                    return
                while len(node) < size:
                    node.append(_clone_template(node))
                del node[size:]
                return
            index = int(re.match(r"data\[(\d+)\]", nxt).group(1))
            while len(node) <= index:
                node.append(_clone_template(node))
            if i + 1 == len(parts) - 1:
                node[index] = value
                return
            parent, parent_key = node, index
            node = node[index]
            i += 2
            continue
        if last:
            if isinstance(node, dict):
                node[part] = value
            return
        if isinstance(node, dict):
            if part not in node or node[part] is None:
                node[part] = [] if parts[i + 1] == "Array" else {}
            parent, parent_key = node, part
            node = node[part]
        elif isinstance(node, list):
            return
        i += 1


def _clone_template(items):
    import copy
    return copy.deepcopy(items[-1]) if items else {}


# --- World import ------------------------------------------------------------
GAME_OBJECT, TRANSFORM, PREFAB_INSTANCE = 1, 4, 1001
TILEMAP, TILEMAP_RENDERER, TILEMAP_COLLIDER = 1839735485, 483693784, 19719996
SPRITE_RENDERER = 212
RIGIDBODY_2D, BOX_COLLIDER, CIRCLE_COLLIDER, CAPSULE_COLLIDER = 50, 61, 58, 70
SORTING_LAYER_STRIDE = 100000  # Pocket2D sorting_order = layer index * stride + Unity order
PIXELS_PER_UNIT = 100.0  # Pocket2D world units to screen pixels at zoom 1


def guid_paths(assets):
    paths = {}
    for meta in Path(assets).rglob("*.meta"):
        m = re.search(r"^guid: ([0-9a-f]+)", meta.read_text(errors="replace"), re.M)
        if m:
            paths[m.group(1)] = meta.with_suffix("")
    return paths


# Unity SpriteAlignment presets as normalized pivots (y up); 9 is Custom.
ALIGNMENT_PIVOTS = {0: (0.5, 0.5), 1: (0, 1), 2: (0.5, 1), 3: (1, 1), 4: (0, 0.5),
                    5: (1, 0.5), 6: (0, 0), 7: (0.5, 0), 8: (1, 0)}


def _pivot(alignment, custom):
    return ALIGNMENT_PIVOTS.get(alignment, custom)


class Texture:
    """A texture's sprites (from its importer .meta) and pixels."""

    def __init__(self, path):
        self.path = path
        meta = Path(str(path) + ".meta").read_text(errors="replace")
        m = re.search(r"spritePixelsToUnits: ([\d.]+)", meta)
        self.ppu = float(m.group(1)) if m else 100.0
        self.sprites = {}
        mode = re.search(r"^  spriteMode: (\d+)", meta, re.M)
        if mode and mode.group(1) == "1":  # Single: the whole texture; any sheet entries are stale
            image = Image.open(path)
            align = re.search(r"^  alignment: (\d+)", meta, re.M)
            custom = re.search(r"^  spritePivot: \{x: ([-\d.]+), y: ([-\d.]+)\}", meta, re.M)
            pivot = _pivot(int(align.group(1)) if align else 0,
                           (float(custom.group(1)), float(custom.group(2))) if custom else (0.5, 0.5))
            self.sprites[21300000] = (Path(path).stem, (0, 0, image.width, image.height), pivot)
        for block in ([] if self.sprites else re.split(r"\n    - serializedVersion: 2\n", meta)):
            name = re.search(r"^\s*name: (.+)$", block, re.M)
            rect = re.search(r"rect:\s*\n\s*serializedVersion: 2\s*\n\s*x: ([\d.]+)\s*\n\s*y: ([\d.]+)\s*\n"
                             r"\s*width: ([\d.]+)\s*\n\s*height: ([\d.]+)", block)
            internal = re.search(r"^\s*internalID: (-?\d+)", block, re.M)
            pivot = re.search(r"pivot: \{x: ([-\d.]+), y: ([-\d.]+)\}", block)
            alignment = re.search(r"^\s*alignment: (\d+)", block, re.M)
            if name and rect and internal:
                custom = (float(pivot.group(1)), float(pivot.group(2))) if pivot else (0.5, 0.5)
                self.sprites[int(internal.group(1))] = (name.group(1).strip(),
                                                        tuple(int(float(v)) for v in rect.groups()),
                                                        _pivot(int(alignment.group(1)) if alignment else 9, custom))
        if not self.sprites:  # single-sprite texture
            image = Image.open(path)
            self.sprites[21300000] = (Path(path).stem, (0, 0, image.width, image.height), (0.5, 0.5))
        self._image = None

    def image(self):
        if self._image is None:
            self._image = Image.open(self.path).convert("RGBA")
        return self._image

    def crop(self, file_id):
        _, (x, y, w, h), _ = self.sprites[file_id]
        image = self.image()
        return image.crop((x, image.height - y - h, x + w, image.height - y))


def transform_tile(image, matrix):
    """Applies a Unity tile matrix's 2x2 part (flip/rotate in 90-degree steps)."""
    a, b, c, d = (round(matrix.get(k, v)) for k, v in (("e00", 1), ("e01", 0), ("e10", 0), ("e11", 1)))
    if (a, b, c, d) == (1, 0, 0, 1):
        return image
    if (a, b, c, d) == (-1, 0, 0, 1):
        return image.transpose(Image.FLIP_LEFT_RIGHT)
    if (a, b, c, d) == (1, 0, 0, -1):
        return image.transpose(Image.FLIP_TOP_BOTTOM)
    if (a, b, c, d) == (-1, 0, 0, -1):
        return image.transpose(Image.ROTATE_180)
    if (a, b, c, d) == (0, -1, 1, 0):  # 90 degrees counter-clockwise (Unity y up)
        return image.transpose(Image.ROTATE_90)
    if (a, b, c, d) == (0, 1, -1, 0):
        return image.transpose(Image.ROTATE_270)
    if (a, b, c, d) == (0, 1, 1, 0):
        return image.transpose(Image.TRANSPOSE)
    if (a, b, c, d) == (0, -1, -1, 0):
        return image.transpose(Image.TRANSVERSE)
    raise ValueError("unsupported tile matrix %r" % ((a, b, c, d),))


class World:
    """Scene objects with prefab instances expanded; keys are (namespace, fileID)."""

    def __init__(self, assets, scene):
        self.paths = guid_paths(assets)
        self.objects = {}  # key -> [class_id, fields]
        self.transform_parent = {}  # prefab-root transform key -> scene transform key
        self.aliases = {}  # stripped object key -> key of the prefab object it stands for
        self.report = []
        self.sprite_actors = {}  # transform key -> the root sprite actor imported for it
        self._load(UnityFile(scene), namespace=(), modifications=[], root_parent=None)

    def _load(self, unity_file, namespace, modifications, root_parent):
        by_target = defaultdict(list)
        for mod in modifications:
            target = mod.get("target") or {}
            by_target[target.get("fileID")].append(mod)
        for fid, (cls, _, fields, stripped) in unity_file.objects.items():
            if stripped:
                instance = (fields.get("m_PrefabInstance") or {}).get("fileID")
                source = (fields.get("m_CorrespondingSourceObject") or {}).get("fileID")
                if instance and source:
                    self.aliases[namespace + (fid,)] = namespace + (instance, source)
                continue
            if cls == PREFAB_INSTANCE:
                continue
            fields = copy.deepcopy(fields)
            for mod in by_target.get(fid, []):
                value = mod.get("value")
                if mod.get("objectReference") and mod["objectReference"].get("fileID"):
                    value = mod["objectReference"]
                apply_modification(fields, mod["propertyPath"], value)
            self.objects[namespace + (fid,)] = [cls, fields]
            if cls == TRANSFORM and root_parent is not None and not (fields.get("m_Father") or {}).get("fileID"):
                self.transform_parent[namespace + (fid,)] = root_parent
        for fid, (cls, _, fields, stripped) in unity_file.objects.items():
            if cls != PREFAB_INSTANCE:
                continue
            modification = fields.get("m_Modification") or {}
            source = (fields.get("m_SourcePrefab") or {}).get("guid")
            path = self.paths.get(source)
            if path is None:
                self.report.append("missing prefab %s" % source)
                continue
            parent = (modification.get("m_TransformParent") or {}).get("fileID")
            parent_key = (namespace + (parent,)) if parent else None
            self._load(UnityFile(path), namespace + (fid,), modification.get("m_Modifications") or [], parent_key)

    def resolve(self, key):
        seen = 0
        while key in self.aliases and seen < 16:
            key = self.aliases[key]
            seen += 1
        return key

    def components(self, game_object_key):
        namespace = game_object_key[:-1]
        for key, (cls, fields) in self.objects.items():
            if key[:-1] == namespace and (fields.get("m_GameObject") or {}).get("fileID") == game_object_key[-1]:
                yield cls, fields

    def active(self, transform_key):
        key = transform_key
        while key is not None and key in self.objects:
            cls, fields = self.objects[key]
            go_key = key[:-1] + ((fields.get("m_GameObject") or {}).get("fileID"),)
            go = self.objects.get(go_key)
            if go and int(go[1].get("m_IsActive", 1)) == 0:
                return False
            key = self.parent(key)
        return True

    def parent(self, transform_key):
        father = (self.objects[transform_key][1].get("m_Father") or {}).get("fileID")
        if father:
            parent = self.resolve(transform_key[:-1] + (father,))
        else:
            parent = self.transform_parent.get(transform_key)
            parent = self.resolve(parent) if parent is not None else None
        return parent if parent in self.objects else None

    def world_transform(self, transform_key):
        """(x, y, rotation_degrees, scale_x, scale_y) in Unity world space (y up).

        Composes parents as translate-rotate-scale; skew from non-uniform parent
        scale under rotation is not representable and is approximated."""
        fields = self.objects[transform_key][1]
        position = fields.get("m_LocalPosition") or {}
        scale = fields.get("m_LocalScale") or {}
        rotation = fields.get("m_LocalRotation") or {}
        lx, ly = float(position.get("x", 0)), float(position.get("y", 0))
        lsx, lsy = float(scale.get("x", 1)), float(scale.get("y", 1))
        lrot = math.degrees(2 * math.atan2(float(rotation.get("z", 0)), float(rotation.get("w", 1))))
        parent = self.parent(transform_key)
        if parent is None:
            return lx, ly, lrot, lsx, lsy
        px, py, prot, psx, psy = self.world_transform(parent)
        c, s = math.cos(math.radians(prot)), math.sin(math.radians(prot))
        x, y = lx * psx, ly * psy
        return px + c * x - s * y, py + s * x + c * y, prot + lrot, psx * lsx, psy * lsy

    def world_position(self, transform_key):
        x, y, rotation, sx, sy = self.world_transform(transform_key)
        if abs(sx - 1) > 1e-6 or abs(sy - 1) > 1e-6 or abs(rotation) > 1e-3:
            self.report.append("tilemap transform scale/rotation ignored on %s" % (transform_key,))
        return x, y


def sorting_layers(assets):
    """Unity sorting layer uniqueID -> draw index, from ProjectSettings/TagManager.asset."""
    path = Path(assets).parent / "ProjectSettings/TagManager.asset"
    text = path.read_text(errors="replace") if path.exists() else ""
    block = text.split("m_SortingLayers:", 1)[1] if "m_SortingLayers:" in text else ""
    ids = [int(v) for v in re.findall(r"uniqueID: (\d+)", block)]
    return {unique: index for index, unique in enumerate(ids)} or {0: 0}


def sort_key(layers, renderer):
    return layers.get(int(renderer.get("m_SortingLayerID", 0)), 0) * SORTING_LAYER_STRIDE + \
        int(renderer.get("m_SortingOrder", 0))


def import_sprites(world, project, textures, layers, index, actors, stats, next_uid, skip_folders=()):
    """Active, enabled SpriteRenderers outside entities become root sprite actors
    (hierarchy flattened)."""
    transforms, names, entity_of = index
    root_sprites = {}
    for key, (cls, fields) in sorted(world.objects.items(), key=lambda item: str(item[0])):
        if cls != SPRITE_RENDERER:
            continue
        go_key = key[:-1] + ((fields.get("m_GameObject") or {}).get("fileID"),)
        transform = transforms.get(go_key)
        sprite_ref = fields.get("m_Sprite") or {}
        guid, file_id = sprite_ref.get("guid"), sprite_ref.get("fileID")
        if transform is None or not world.active(transform) or int(fields.get("m_Enabled", 1)) == 0 or not guid:
            stats["skipped"] += 1
            continue
        root = entity_of.get(transform)
        if root is not None and root != transform:
            continue
        if guid not in world.paths:
            world.report.append("%s: sprite texture %s not found" % (names.get(go_key), guid))
            continue
        if world.paths[guid].parent.name in skip_folders:
            stats["skipped"] += 1
            continue
        if guid not in textures:
            textures[guid] = Texture(world.paths[guid])
        texture = textures[guid]
        if file_id not in texture.sprites:
            world.report.append("%s: sprite %s missing in %s" % (names.get(go_key), file_id, texture.path.name))
            continue
        if int(fields.get("m_DrawMode", 0)) != 0:
            world.report.append("%s: sliced/tiled draw mode drawn as simple" % names.get(go_key))
        image_name = "vr_props/%s_%s.png" % (safe(texture.path.stem), file_id & 0xFFFFFFFF)
        target = project / "images" / image_name
        if not target.exists():
            target.parent.mkdir(parents=True, exist_ok=True)
            texture.crop(file_id).save(target)
        x, y, rotation, sx, sy = world.world_transform(transform)
        _, _, pivot = texture.sprites[file_id]
        color = fields.get("m_Color") or {}
        unit = PIXELS_PER_UNIT / texture.ppu
        sprite = {"type": "SpriteRenderer", "sprite": image_name, "pivot_x": pivot[0], "pivot_y": 1.0 - pivot[1],
                       "scale_x": round(sx * unit * (-1 if int(fields.get("m_FlipX", 0)) else 1), 4),
                       "scale_y": round(sy * unit * (-1 if int(fields.get("m_FlipY", 0)) else 1), 4),
                       "r": round(255 * float(color.get("r", 1))), "g": round(255 * float(color.get("g", 1))),
                       "b": round(255 * float(color.get("b", 1))), "a": round(255 * float(color.get("a", 1))),
                       "sorting_order": sort_key(layers, fields), "auto_sorting_order": True}
        if root is not None:  # an entity's own sprite goes on its SpawnMarker
            root_sprites[root] = (world.paths[guid].parent.name, sprite)
            continue
        next_uid += 1
        actors.append({"uid": next_uid, "name": names.get(go_key, "Sprite"), "components": {
            "transform": {"type": "Transform", "x": round(x, 4), "y": round(-y, 4), "rotation": round(-rotation, 3)},
            "sprite": sprite}})
        world.sprite_actors[transform] = actors[-1]
        stats["sprites"] += 1
    return next_uid, root_sprites


def scene_index(world):
    """GameObject key -> transform key, names, and the entity root of every transform.

    An entity is a GameObject with a non-static Rigidbody2D (player, enemies,
    NPCs): gameplay scripts own it, so its subtree imports as one SpawnMarker
    instead of sprites and colliders."""
    transforms, names, entity_roots = {}, {}, set()
    for key, (cls, fields) in world.objects.items():
        if cls == TRANSFORM:
            transforms[key[:-1] + ((fields.get("m_GameObject") or {}).get("fileID"),)] = key
        elif cls == GAME_OBJECT:
            names[key] = str(fields.get("m_Name", "Object"))
    for key, (cls, fields) in world.objects.items():
        if cls == RIGIDBODY_2D and int(fields.get("m_BodyType", 0)) != 2:
            transform = transforms.get(key[:-1] + ((fields.get("m_GameObject") or {}).get("fileID"),))
            if transform is not None and world.active(transform):
                entity_roots.add(transform)
    entity_of = {}
    for transform in transforms.values():
        key, root = transform, None
        while key is not None:
            if key in entity_roots:
                root = key
            key = world.parent(key)
        entity_of[transform] = root
    return transforms, names, entity_of


def owner(world, transforms, fields, key):
    go_key = key[:-1] + ((fields.get("m_GameObject") or {}).get("fileID"),)
    return go_key, transforms.get(go_key)


def import_entities(world, transforms, names, entity_of, root_sprites, actors, stats, next_uid):
    """One SpawnMarker per entity root. kind is the texture folder of the root's
    own sprite (else of its first child sprite); the root sprite stays on the
    marker so entities without gameplay code still draw."""
    kinds = {root: kind for root, (kind, _) in root_sprites.items()}
    for key, (cls, fields) in sorted(world.objects.items(), key=lambda item: str(item[0])):
        if cls != SPRITE_RENDERER:
            continue
        _, transform = owner(world, transforms, fields, key)
        root = entity_of.get(transform)
        guid = (fields.get("m_Sprite") or {}).get("guid")
        if root is not None and root not in kinds and guid in world.paths and world.active(transform):
            kinds[root] = world.paths[guid].parent.name
    by_transform = {t: g for g, t in transforms.items()}
    for root in sorted(set(r for r in entity_of.values() if r is not None), key=str):
        x, y, _, _, _ = world.world_transform(root)
        components = {"transform": {"type": "Transform", "x": round(x, 4), "y": round(-y, 4)},
                      "spawn": {"type": "SpawnMarker", "kind": kinds.get(root, "")}}
        if root in root_sprites:
            components["sprite"] = root_sprites[root][1]
        next_uid += 1
        actors.append({"uid": next_uid, "name": names.get(by_transform.get(root), "Entity"), "components": components})
        stats["entities"] += 1
    return next_uid


def import_colliders(world, transforms, names, entity_of, actors, stats, next_uid):
    """Solid (non-trigger) colliders outside entities become static bodies."""
    for key, (cls, fields) in sorted(world.objects.items(), key=lambda item: str(item[0])):
        if cls not in (BOX_COLLIDER, CIRCLE_COLLIDER, CAPSULE_COLLIDER):
            continue
        go_key, transform = owner(world, transforms, fields, key)
        if (transform is None or not world.active(transform) or entity_of.get(transform) is not None
                or int(fields.get("m_Enabled", 1)) == 0):
            continue
        if int(fields.get("m_IsTrigger", 0)):
            stats["triggers_skipped"] += 1
            continue
        x, y, rotation, sx, sy = world.world_transform(transform)
        offset = fields.get("m_Offset") or {}
        ox, oy = float(offset.get("x", 0)) * sx, float(offset.get("y", 0)) * sy
        c, s = math.cos(math.radians(rotation)), math.sin(math.radians(rotation))
        x, y = x + c * ox - s * oy, y + s * ox + c * oy
        body = {"type": "Rigidbody", "body_type": "static", "x": round(x, 4), "y": round(-y, 4),
                "rotation": round(-rotation, 3), "has_collider": True, "has_trigger": False}
        size = fields.get("m_Size") or {}
        width, height = abs(float(size.get("x", 1)) * sx), abs(float(size.get("y", 1)) * sy)
        if cls == CIRCLE_COLLIDER:
            body.update(collider_type="circle", radius=round(float(fields.get("m_Radius", 0.5)) * max(abs(sx), abs(sy)), 4))
        elif cls == CAPSULE_COLLIDER and int(fields.get("m_Direction", 0)) == 0 and height >= width:
            body.update(collider_type="capsule", radius=round(width / 2, 4), height=round(height, 4))
        else:
            if cls == CAPSULE_COLLIDER:
                world.report.append("%s: horizontal capsule collider imported as a box" % names.get(go_key))
            body.update(collider_type="box", width=round(width, 4), height=round(height, 4))
        next_uid += 1
        actors.append({"uid": next_uid, "name": "%s_collider" % names.get(go_key, "Object"),
                       "components": {"body": body}})
        stats["colliders"] += 1
    return next_uid


_COLLIDER_TYPES = {}


def tile_collider_type(world, guid):
    """A Tile asset's m_ColliderType: 0 None, 1 Sprite, 2 Grid (1 is approximated as the full cell)."""
    if guid not in _COLLIDER_TYPES:
        path = world.paths.get(guid)
        text = path.read_text(errors="replace") if path is not None and path.suffix == ".asset" else ""
        match = re.search(r"m_ColliderType: (\d)", text)
        _COLLIDER_TYPES[guid] = int(match.group(1)) if match else 1
    return _COLLIDER_TYPES[guid]


def safe(name):
    return re.sub(r"[^A-Za-z0-9_]+", "_", name).strip("_") or "map"


def import_points(world, transforms, names, entity_of, prefixes, actors, stats, next_uid):
    """Active GameObjects named with one of the prefixes become marker actors
    (SpawnMarker kind "point"): spawn points and other positions gameplay uses.
    A point already imported as a sprite gets the marker on that actor."""
    if not prefixes:
        return next_uid
    for go_key, transform in sorted(transforms.items(), key=lambda item: str(item[0])):
        name = names.get(go_key, "")
        if not any(name.startswith(prefix) for prefix in prefixes):
            continue
        if not world.active(transform) or entity_of.get(transform) is not None:
            continue
        marker = {"type": "SpawnMarker", "kind": "point"}
        if transform in world.sprite_actors:  # a visible point keeps its sprite actor and gains the marker
            world.sprite_actors[transform]["components"]["spawn"] = marker
            stats["points"] += 1
            continue
        x, y, _, _, _ = world.world_transform(transform)
        next_uid += 1
        actors.append({"uid": next_uid, "name": name, "components": {
            "transform": {"type": "Transform", "x": round(x, 4), "y": round(-y, 4)},
            "spawn": marker}})
        stats["points"] += 1
    return next_uid


def import_unity(assets, scene, project, scene_name="world", skip_texture_folders=(), point_prefixes=()):
    """Imports the scene; returns (scene path, report dict)."""
    global Image
    from PIL import Image as pil_image
    Image = pil_image

    project = Path(project)
    world = World(assets, scene)
    textures = {}
    actors = []
    used_names = defaultdict(int)
    stats = {"tilemaps": 0, "tiles": 0, "sprite_actors": 0, "sprites": 0, "colliders": 0, "entities": 0,
             "points": 0, "skipped": 0, "triggers_skipped": 0}
    layers = sorting_layers(assets)
    next_uid = 1000
    for key, (cls, fields) in sorted(world.objects.items(), key=lambda item: str(item[0])):
        if cls != TILEMAP:
            continue
        go_key = key[:-1] + ((fields.get("m_GameObject") or {}).get("fileID"),)
        go = world.objects.get(go_key, [0, {}])[1]
        name = str(go.get("m_Name", "Tilemap"))
        comps = list(world.components(go_key))
        transform = next((key[:-1] + (k[-1],) for k, (c, f) in world.objects.items()
                          if c == TRANSFORM and k[:-1] == key[:-1]
                          and (f.get("m_GameObject") or {}).get("fileID") == go_key[-1]), None)
        renderer = next((f for c, f in comps if c == TILEMAP_RENDERER), {})
        tiles = fields.get("m_Tiles") or []
        if (transform is None or not world.active(transform) or int(renderer.get("m_Enabled", 1)) == 0
                or int(fields.get("m_Enabled", 1)) == 0 or not tiles):
            stats["skipped"] += 1
            continue
        collider = any(c == TILEMAP_COLLIDER and int(f.get("m_Enabled", 1)) for c, f in comps)
        order = sort_key(layers, renderer)
        origin_x, origin_y = world.world_position(transform)
        sprites = fields.get("m_TileSpriteArray") or []
        tile_assets = fields.get("m_TileAssetArray") or []
        matrices = fields.get("m_TileMatrixArray") or []
        colors = fields.get("m_TileColorArray") or []
        cells = {}
        solid_cells = set()
        atlas_keys = []
        atlas_index = {}
        for entry in tiles:
            first, second = entry.get("first") or {}, entry.get("second") or {}
            sprite_ref = (sprites[int(second.get("m_TileSpriteIndex", 0))].get("m_Data") or {}) \
                if int(second.get("m_TileSpriteIndex", 0)) < len(sprites) else {}
            guid, file_id = sprite_ref.get("guid"), sprite_ref.get("fileID")
            if not guid or guid not in world.paths:
                world.report.append("%s: tile without a sprite at %r" % (name, first))
                continue
            if guid not in textures:
                textures[guid] = Texture(world.paths[guid])
            texture = textures[guid]
            if file_id not in texture.sprites:
                world.report.append("%s: sprite %s missing in %s" % (name, file_id, texture.path.name))
                continue
            matrix = (matrices[int(second.get("m_TileMatrixIndex", 0))].get("m_Data") or {}) if matrices else {}
            color = (colors[int(second.get("m_TileColorIndex", 0))].get("m_Data") or {}) if colors else {}
            tint = tuple(round(float(color.get(c, 1)), 3) for c in "rgba")
            atlas_key = (guid, file_id, tuple(round(float(matrix.get(k, d))) for k, d in
                                              (("e00", 1), ("e01", 0), ("e10", 0), ("e11", 1))), tint)
            cell = (int(first.get("x", 0)), int(first.get("y", 0)))
            if collider:
                tile_index = int(second.get("m_TileIndex", 0))
                asset_ref = (tile_assets[tile_index].get("m_Data") or {}) if tile_index < len(tile_assets) else {}
                if tile_collider_type(world, asset_ref.get("guid")) != 0:
                    solid_cells.add(cell)
            _, (_, _, w, h), pivot = texture.sprites[file_id]
            unit = texture.ppu
            if w != unit or h != unit:
                # Oversized or partial sprites draw as sprite actors at the cell centre.
                image_name = "vr_props/%s_%s_%d.png" % (safe(texture.path.stem), file_id & 0xFFFFFFFF, atlas_key[2][0] + 2 * atlas_key[2][3])
                target = project / "images" / image_name
                if not target.exists():
                    target.parent.mkdir(parents=True, exist_ok=True)
                    transform_tile(texture.crop(file_id), dict(zip(("e00", "e01", "e10", "e11"), atlas_key[2]))).save(target)
                world_x = origin_x + cell[0] + 0.5
                world_y = -(origin_y + cell[1] + 0.5)
                next_uid += 1
                actors.append({"uid": next_uid, "name": "%s_prop" % name, "components": {
                    "transform": {"type": "Transform", "x": round(world_x, 4), "y": round(world_y, 4)},
                    "sprite": {"type": "SpriteRenderer", "sprite": image_name, "pivot_x": pivot[0],
                               "pivot_y": 1.0 - pivot[1], "scale_x": PIXELS_PER_UNIT / unit,
                               "scale_y": PIXELS_PER_UNIT / unit, "sorting_order": order,
                               "auto_sorting_order": True}}})
                stats["sprite_actors"] += 1
                continue
            if atlas_key not in atlas_index:
                atlas_index[atlas_key] = len(atlas_keys) + 1
                atlas_keys.append(atlas_key)
            cells[cell] = atlas_index[atlas_key]
        if not cells:
            continue
        xs = [c[0] for c in cells]
        ys = [c[1] for c in cells]
        xmin, xmax, ymin, ymax = min(xs), max(xs), min(ys), max(ys)
        width, height = xmax - xmin + 1, ymax - ymin + 1
        if width > 512 or height > 512:
            world.report.append("%s: %dx%d exceeds 512 cells; skipped" % (name, width, height))
            continue
        used_names[safe(name)] += 1
        map_name = "%s_%d" % (safe(name), used_names[safe(name)])
        columns = max(1, math.ceil(math.sqrt(len(atlas_keys))))
        rows = math.ceil(len(atlas_keys) / columns)
        unit = 16
        atlas = Image.new("RGBA", (columns * unit, rows * unit))
        for index, (guid, file_id, mat, tint) in enumerate(atlas_keys):
            image = transform_tile(textures[guid].crop(file_id), dict(zip(("e00", "e01", "e10", "e11"), mat)))
            if tint != (1.0, 1.0, 1.0, 1.0):
                r, g, b, a = image.split()
                image = Image.merge("RGBA", [band.point(lambda v, k=k: int(v * k)) for band, k in zip((r, g, b, a), tint)])
            atlas.paste(image, ((index % columns) * unit, (index // columns) * unit))
        atlas_name = "vr_tiles/%s.png" % map_name
        (project / "images/vr_tiles").mkdir(parents=True, exist_ok=True)
        atlas.save(project / "images" / atlas_name)
        ground_cells = [0] * (width * height)
        solid_layer = [0] * (width * height)
        for (cx, cy), tile_id in cells.items():
            target = solid_layer if (cx, cy) in solid_cells else ground_cells
            target[(ymax - cy) * width + (cx - xmin)] = tile_id
        tilemap = {"version": 1, "width": width, "height": height, "cell_size": 1.0,
                   "atlas": {"image": atlas_name, "columns": columns, "rows": rows,
                             "tile_width_px": unit, "tile_height_px": unit},
                   "layers": {"ground": {"sorting_order": order, "cells": ground_cells},
                              "solid": {"sorting_order": order, "cells": solid_layer}}}
        (project / "maps").mkdir(parents=True, exist_ok=True)
        (project / "maps" / (map_name + ".tilemap.json")).write_text(json.dumps(tilemap, separators=(",", ":")))
        next_uid += 1
        actors.append({"uid": next_uid, "name": map_name, "components": {
            "transform": {"type": "Transform", "x": round(origin_x + xmin, 4), "y": round(-(origin_y + ymax + 1), 4)},
            "map": {"type": "Tilemap", "map_resource": map_name + ".tilemap.json"}}})
        stats["tilemaps"] += 1
        stats["tiles"] += len(cells)
    index = scene_index(world)
    next_uid, root_sprites = import_sprites(world, project, textures, layers, index, actors, stats, next_uid,
                                            tuple(skip_texture_folders))
    next_uid = import_colliders(world, *index, actors, stats, next_uid)
    next_uid = import_entities(world, *index, root_sprites, actors, stats, next_uid)
    next_uid = import_points(world, *index, tuple(point_prefixes), actors, stats, next_uid)
    (project / "scenes").mkdir(parents=True, exist_ok=True)
    scene_path = project / "scenes" / (scene_name + ".scene")
    # Hand-authored actors (uid below the importer's range) survive re-imports.
    authored = [a for a in json.loads(scene_path.read_text())["actors"] if a.get("uid", 0) < 1000] \
        if scene_path.exists() else []
    scene_path.write_text(json.dumps({"actors": authored + actors}, indent=1))
    report = {"stats": stats, "notes": sorted(set(world.report))}
    (project / "import_report.json").write_text(json.dumps(report, indent=2))
    return scene_path, report

