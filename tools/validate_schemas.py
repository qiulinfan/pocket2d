#!/usr/bin/env python3
"""Validate Pocket2D project assets against scripting/api/schemas.

A small JSON Schema (draft 2020-12) validator that supports only the keywords
the Pocket2D asset schemas use, plus a command line that checks every project
asset in the repository:

    python3 tools/validate_schemas.py [--root REPO]

A project is any directory that contains game.config (tests/fixtures/*,
Projects/*). Its assets are matched to schemas by the same patterns as
`pocket check`. The command also runs negative self-checks that must be
rejected, and exits nonzero when anything fails.

Semantics follow the engine's rapidjson loaders where JSON Schema leaves room:
"integer" accepts only JSON numbers written without a fraction or exponent
(1.0 is a number, not an integer), booleans are never numbers, and a pattern's
"$" anchors at the very end of the string. Files may not use NaN/Infinity and,
for formats whose loader rejects them, duplicate member names.

Only the standard library is used. Import it and call validate(instance,
schema) to get a list of "<JSON pointer>: <problem>" strings.
"""
import argparse
import json
import re
import sys
from pathlib import Path

ANNOTATIONS = {"$schema", "$id", "$defs", "$comment", "title", "description", "default"}
SUPPORTED = ANNOTATIONS | {
    "$ref", "type", "enum", "const",
    "properties", "required", "additionalProperties", "patternProperties", "propertyNames",
    "items", "prefixItems", "minItems", "maxItems",
    "minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum",
    "minLength", "maxLength", "pattern",
    "anyOf", "oneOf", "allOf", "not", "if", "then", "else",
}

# (glob relative to a project directory, schema name, loader rejects duplicate members)
ASSET_PATTERNS = [
    ("game.config", "game-config", True),
    ("rendering.config", "rendering-config", True),
    ("actions.json", "actions", False),
    ("scenes/**/*.scene", "scene", True),
    ("actor_templates/**/*.template", "actor-template", True),
    ("maps/**/*.tilemap.json", "tilemap", True),
    ("animations/**/*.animation.json", "animation", True),
    ("items/**/*.items.json", "items", True),
    ("scenarios/**/*.scenario.json", "scenario", False),
]
SKIPPED_DIRECTORIES = {".git", ".claude", "build", "thirdparty", "node_modules", "__pycache__"}


# ---------------------------------------------------------------------------
# Validator
# ---------------------------------------------------------------------------

def validate(instance, schema):
    """Return a list of errors, each "<JSON pointer>: <message>"; empty when valid."""
    errors = []
    _check(instance, schema, schema, "", errors)
    return errors


def _type_matches(value, name):
    if name == "null":
        return value is None
    if name == "boolean":
        return isinstance(value, bool)
    if name == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if name == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if name == "string":
        return isinstance(value, str)
    if name == "array":
        return isinstance(value, list)
    if name == "object":
        return isinstance(value, dict)
    raise ValueError("unknown schema type " + repr(name))


def _json_equal(a, b):
    """JSON equality: booleans differ from numbers, 1 equals 1.0."""
    if isinstance(a, bool) or isinstance(b, bool):
        return isinstance(a, bool) and isinstance(b, bool) and a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return a == b
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(_json_equal(x, y) for x, y in zip(a, b))
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(_json_equal(a[k], b[k]) for k in a)
    return type(a) is type(b) and a == b


_PATTERNS = {}


def _pattern(text):
    """Compile an ECMA-262 style pattern; "$" outside a class means end of string."""
    if text not in _PATTERNS:
        out, in_class, index = [], False, 0
        while index < len(text):
            char = text[index]
            if char == "\\":
                out.append(text[index:index + 2])
                index += 2
                continue
            if char == "[":
                in_class = True
            elif char == "]":
                in_class = False
            elif char == "$" and not in_class:
                char = r"\Z"
            out.append(char)
            index += 1
        _PATTERNS[text] = re.compile("".join(out))
    return _PATTERNS[text]


def _resolve(root, reference):
    if not reference.startswith("#/"):
        raise ValueError("only local references are supported: " + reference)
    target = root
    for part in reference[2:].split("/"):
        target = target[part.replace("~1", "/").replace("~0", "~")]
    return target


def _child(pointer, key):
    return pointer + "/" + str(key).replace("~", "~0").replace("/", "~1")


def _describe(value):
    text = json.dumps(value)
    return text if len(text) <= 40 else text[:37] + "..."


def _is_valid(instance, schema, root, pointer):
    errors = []
    _check(instance, schema, root, pointer, errors)
    return not errors


def _check(instance, schema, root, pointer, errors):
    if schema is True:
        return
    if schema is False:
        errors.append(pointer + ": no value is allowed here")
        return
    unsupported = set(schema) - SUPPORTED
    if unsupported:
        raise ValueError("unsupported schema keywords: " + ", ".join(sorted(unsupported)))
    where = pointer or "/"

    if "$ref" in schema:
        _check(instance, _resolve(root, schema["$ref"]), root, pointer, errors)

    if "type" in schema:
        names = schema["type"] if isinstance(schema["type"], list) else [schema["type"]]
        if not any(_type_matches(instance, name) for name in names):
            errors.append("%s: expected %s, got %s" % (where, " or ".join(names), _describe(instance)))
            return  # the remaining keywords would only repeat the type error
    if "const" in schema and not _json_equal(instance, schema["const"]):
        errors.append("%s: must be %s, got %s" % (where, _describe(schema["const"]), _describe(instance)))
    if "enum" in schema and not any(_json_equal(instance, option) for option in schema["enum"]):
        errors.append("%s: must be one of %s, got %s" % (
            where, ", ".join(_describe(option) for option in schema["enum"]), _describe(instance)))

    if _type_matches(instance, "number"):
        if "minimum" in schema and instance < schema["minimum"]:
            errors.append("%s: %s is less than the minimum %s" % (where, instance, schema["minimum"]))
        if "maximum" in schema and instance > schema["maximum"]:
            errors.append("%s: %s is greater than the maximum %s" % (where, instance, schema["maximum"]))
        if "exclusiveMinimum" in schema and instance <= schema["exclusiveMinimum"]:
            errors.append("%s: %s must be greater than %s" % (where, instance, schema["exclusiveMinimum"]))
        if "exclusiveMaximum" in schema and instance >= schema["exclusiveMaximum"]:
            errors.append("%s: %s must be less than %s" % (where, instance, schema["exclusiveMaximum"]))

    if isinstance(instance, str):
        if "minLength" in schema and len(instance) < schema["minLength"]:
            errors.append("%s: string shorter than %d characters" % (where, schema["minLength"]))
        if "maxLength" in schema and len(instance) > schema["maxLength"]:
            errors.append("%s: string longer than %d characters" % (where, schema["maxLength"]))
        if "pattern" in schema and not _pattern(schema["pattern"]).search(instance):
            errors.append("%s: %s does not match the pattern %s" % (where, _describe(instance), schema["pattern"]))

    if isinstance(instance, list):
        if "minItems" in schema and len(instance) < schema["minItems"]:
            errors.append("%s: array needs at least %d items" % (where, schema["minItems"]))
        if "maxItems" in schema and len(instance) > schema["maxItems"]:
            errors.append("%s: array allows at most %d items" % (where, schema["maxItems"]))
        prefix = schema.get("prefixItems", [])
        for index, item in enumerate(instance):
            if index < len(prefix):
                _check(item, prefix[index], root, _child(pointer, index), errors)
            elif "items" in schema:
                _check(item, schema["items"], root, _child(pointer, index), errors)

    if isinstance(instance, dict):
        for name in schema.get("required", []):
            if name not in instance:
                errors.append("%s: missing required member %s" % (where, json.dumps(name)))
        properties = schema.get("properties", {})
        patterns = schema.get("patternProperties", {})
        for name, value in instance.items():
            location = _child(pointer, name)
            if "propertyNames" in schema:
                for problem in validate(name, schema["propertyNames"]):
                    errors.append("%s: member name %s: %s" % (location, json.dumps(name), problem.split(": ", 1)[1]))
            matched = False
            if name in properties:
                matched = True
                _check(value, properties[name], root, location, errors)
            for expression, subschema in patterns.items():
                if _pattern(expression).search(name):
                    matched = True
                    _check(value, subschema, root, location, errors)
            if not matched and "additionalProperties" in schema:
                if schema["additionalProperties"] is False:
                    errors.append("%s: unknown member %s" % (where, json.dumps(name)))
                else:
                    _check(value, schema["additionalProperties"], root, location, errors)

    for subschema in schema.get("allOf", []):
        _check(instance, subschema, root, pointer, errors)
    if "anyOf" in schema:
        _check_alternatives(instance, schema["anyOf"], root, pointer, errors, exactly_one=False)
    if "oneOf" in schema:
        _check_alternatives(instance, schema["oneOf"], root, pointer, errors, exactly_one=True)
    if "not" in schema and _is_valid(instance, schema["not"], root, pointer):
        errors.append("%s: %s matches a schema it must not match" % (where, _describe(instance)))
    if "if" in schema:
        branch = "then" if _is_valid(instance, schema["if"], root, pointer) else "else"
        if branch in schema:
            _check(instance, schema[branch], root, pointer, errors)


def _check_alternatives(instance, alternatives, root, pointer, errors, exactly_one):
    results = []
    for subschema in alternatives:
        branch_errors = []
        _check(instance, subschema, root, pointer, branch_errors)
        results.append(branch_errors)
    passing = sum(1 for result in results if not result)
    where = pointer or "/"
    if passing == 0:
        # Prefer the form whose errors lie deeper than this value (its shape fit).
        at_value = where + ": "
        closest = min(results, key=lambda result: (sum(e.startswith(at_value) for e in result), len(result)))
        errors.append("%s: %s matches none of the %d allowed forms; closest form: %s" % (
            where, _describe(instance), len(alternatives), "; ".join(closest)))
    elif exactly_one and passing > 1:
        errors.append("%s: must match exactly one of %d forms, matched %d" % (where, len(alternatives), passing))


# ---------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------

def _reject_constant(name):
    raise ValueError("non-standard JSON constant " + name)


def _no_duplicates(pairs):
    result = {}
    for name, value in pairs:
        if name in result:
            raise ValueError("duplicate member name " + json.dumps(name))
        result[name] = value
    return result


def load_json(path, reject_duplicates):
    text = Path(path).read_text(encoding="utf-8")
    return json.loads(text, parse_constant=_reject_constant,
                      object_pairs_hook=_no_duplicates if reject_duplicates else None)


def find_projects(root):
    projects = []
    for config in sorted(root.rglob("game.config")):
        relative = config.relative_to(root).parts
        if not any(part in SKIPPED_DIRECTORIES for part in relative[:-1]):
            projects.append(config.parent)
    return projects


# Instances each schema must reject; proves the validator is not vacuous.
NEGATIVE_CHECKS = [
    ("scene", {"actors": [{"uid": 1.5, "components": {}}]}, "non-integer actor uid"),
    ("scene", {"actors": [{"components": {"body": {"type": "Rigidbody", "mask": [1, "a"]}}}]},
     "mixed-type property array"),
    ("actor-template", {"template": "Player", "components": {}}, "template inheritance"),
    ("tilemap", {"version": 1, "width": 1, "height": 1, "cell_size": 1,
                 "atlas": {"image": "../a.png", "columns": 1, "rows": 1, "tile_width_px": 8, "tile_height_px": 8},
                 "layers": {"ground": {"sorting_order": 0, "cells": [0]},
                            "solid": {"sorting_order": 0, "cells": [0]}}, "extra": 1},
     "escaping atlas path and unknown member"),
    ("actions", {"version": 1, "actions": {"jump": {"type": "button", "mouse": [4]}}}, "mouse button 4"),
    ("items", {"version": 1, "items": [{"id": "sword", "display_name": "Sword", "kind": "weapon",
                                         "max_stack": 5, "equipment_slot": "subweapon"}]},
     "stackable weapon"),
    ("scenario", {"version": 1, "name": "x", "max_frames": 10, "controls": {"keys": ["a"], "actions": ["b"]},
                  "success": [{"event": "done"}]}, "controls with both keys and actions"),
]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent,
                        help="repository root (default: this script's repository)")
    arguments = parser.parse_args(argv)
    root = arguments.root.resolve()
    schema_directory = root / "scripting/api/schemas"

    schemas = {}
    for _, name, _ in ASSET_PATTERNS:
        schemas[name] = load_json(schema_directory / (name + ".schema.json"), True)

    failures = 0
    counts = {name: 0 for _, name, _ in ASSET_PATTERNS}
    projects = find_projects(root)
    for project in projects:
        for pattern, name, reject_duplicates in ASSET_PATTERNS:
            for path in sorted(project.glob(pattern)):
                counts[name] += 1
                label = path.relative_to(root).as_posix()
                try:
                    instance = load_json(path, reject_duplicates)
                except (OSError, UnicodeDecodeError, ValueError) as error:
                    print("FAIL %s: %s" % (label, error))
                    failures += 1
                    continue
                problems = validate(instance, schemas[name])
                for problem in problems:
                    print("FAIL %s %s" % (label, problem))
                failures += 1 if problems else 0

    rejected = 0
    for name, instance, reason in NEGATIVE_CHECKS:
        if validate(instance, schemas[name]):
            rejected += 1
        else:
            print("FAIL self-check: %s schema accepted %s" % (name, reason))
            failures += 1

    total = sum(counts.values())
    print("checked %d assets in %d projects: %s" % (
        total, len(projects), ", ".join("%s %d" % (name, count) for name, count in counts.items() if count)))
    print("self-checks: %d of %d invalid instances rejected" % (rejected, len(NEGATIVE_CHECKS)))
    if failures:
        print("%d failure(s)" % failures)
        return 1
    print("all assets valid")
    return 0


if __name__ == "__main__":
    sys.exit(main())
