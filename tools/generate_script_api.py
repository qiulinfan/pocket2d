#!/usr/bin/env python3
"""Generate the curated TS/LuaLS/embedded host contract from one JSON source."""
import argparse
import json
from pathlib import Path
import re
import sys

IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
PRIMITIVES = {"string", "number", "integer", "boolean", "unknown", "void"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def validate(contract):
    require(contract["contract_version"] == 1 and contract["coverage"] == "curated", "unsupported contract version/coverage")
    types = contract["types"]
    names = {entry["name"] for entry in types}
    require(len(names) == len(types), "duplicate type name")

    def identifier(name):
        require(isinstance(name, str) and IDENTIFIER.fullmatch(name), "invalid identifier: " + repr(name))

    def type_ref(value, generics=()):
        if isinstance(value, str):
            require(value in names or value in PRIMITIVES or value in generics, "unknown type: " + value)
            return
        require(isinstance(value, dict) and len(value) == 1, "type expression must have one operator")
        operator, child = next(iter(value.items()))
        if operator in ("array", "nullable"):
            type_ref(child, generics)
        elif operator == "union":
            require(isinstance(child, list) and len(child) >= 2, "union requires at least two types")
            for item in child:
                type_ref(item, generics)
        elif operator == "literal":
            require(type(child) in (str, bool, int), "unsupported literal type")
        else:
            raise ValueError("unknown type operator: " + operator)

    def function(entry, expected_call):
        identifier(entry["name"])
        require(entry["call"] == expected_call, "incorrect self convention: " + entry["name"])
        generic_names = set()
        for generic in entry.get("generics", []):
            identifier(generic["name"])
            require(generic["name"] not in generic_names, "duplicate generic parameter")
            generic_names.add(generic["name"])
            type_ref(generic["extends"])
            type_ref(generic["default"])
        params = set()
        for parameter in entry["parameters"]:
            identifier(parameter["name"])
            require(parameter["name"] not in params, "duplicate parameter")
            params.add(parameter["name"])
            type_ref(parameter["type"], generic_names)
        type_ref(entry["returns"], generic_names)

    for entry in types:
        identifier(entry["name"])
        kind = entry["kind"]
        if kind == "alias":
            type_ref(entry["type"])
        elif kind == "dictionary":
            type_ref(entry["value"])
        else:
            require(kind in ("native", "component", "record"), "unsupported type kind")
            if "extends" in entry:
                type_ref(entry["extends"])
            fields = set()
            for field in entry["properties"]:
                identifier(field["name"])
                require(field["name"] not in fields, "duplicate property")
                fields.add(field["name"])
                require(field["access"] in ("mutable", "readonly", "engine_owned"), "unknown property access")
                type_ref(field["type"])
            for method in entry["methods"]:
                function(method, "instance")
    seen = set()
    for namespace in contract["namespaces"]:
        identifier(namespace["name"])
        require(namespace["name"] not in seen, "duplicate namespace")
        seen.add(namespace["name"])
        for entry in namespace["functions"]:
            function(entry, "static")
    for entry in contract["constructors"]:
        function(entry, "constructor")
    for entry in contract.get("callbacks", []):
        function(entry, "instance")
        type_ref(entry["receiver"])


def type_text(value, language):
    if isinstance(value, str):
        if language == "ts":
            return "number" if value == "integer" else value
        # LuaLS has no TypeScript-style unknown narrowing. Preserve the runtime
        # validation requirement in field documentation instead of inventing a type.
        return {"unknown": "any", "void": "nil"}.get(value, value)
    operator, child = next(iter(value.items()))
    if operator == "literal":
        return json.dumps(child, ensure_ascii=False)
    if operator == "nullable":
        return type_text(child, language) + (" | undefined" if language == "ts" else "|nil")
    if operator == "array":
        return "(" + type_text(child, language) + ")[]"
    if operator == "union":
        return (" | " if language == "ts" else "|").join(type_text(item, language) for item in child)
    raise ValueError("unhandled type operator")


def ts_signature(entry, prefix="", receiver=None):
    generics = entry.get("generics", [])
    generic_text = ""
    if generics:
        generic_text = "<" + ", ".join(g["name"] + " extends " + type_text(g["extends"], "ts") + " = " + type_text(g["default"], "ts") for g in generics) + ">"
    if entry["call"] == "instance":
        require(receiver is not None, "instance signature requires its receiver type")
        parameters = ["this: " + receiver]
    else:
        parameters = ["this: void"]
    parameters += [p["name"] + ": " + type_text(p["type"], "ts") for p in entry["parameters"]]
    return prefix + entry["name"] + generic_text + "(" + ", ".join(parameters) + "): " + type_text(entry["returns"], "ts") + ";"


def typescript(contract):
    lines = ["// Generated by tools/generate_script_api.py; edit api/host-api.json.",
             "// Curated Lua host API, contract version 1. No Node/DOM or full-engine parity.",
             "// Nominal marker fields exist only in the type system, never on Lua userdata.", ""]
    for entry in contract["types"]:
        name = entry["name"]
        if entry.get("description"):
            lines.append("/** " + entry["description"] + " */")
        if entry["kind"] == "alias":
            lines.append("type " + name + " = " + type_text(entry["type"], "ts") + ";")
        elif entry["kind"] == "dictionary":
            lines.append("interface " + name + " { [key: string]: " + type_text(entry["value"], "ts") + "; }")
        else:
            parent = " extends " + entry["extends"] if "extends" in entry else ""
            lines.append("interface " + name + parent + " {")
            if entry.get("nominal"):
                lines.append("    readonly __pocketNative" + name + ": unique symbol;")
            for field in entry["properties"]:
                note = {"engine_owned": "Engine-owned identity; authoring readonly, runtime setter protection is not implied.",
                        "readonly": "Runtime getter only.", "mutable": ""}[field["access"]]
                note = " ".join(filter(None, [note, field.get("description", "")]))
                if note:
                    lines.append("    /** " + note + " */")
                modifier = "readonly " if field["access"] != "mutable" else ""
                lines.append("    " + modifier + field["name"] + ": " + type_text(field["type"], "ts") + ";")
            for method in entry["methods"]:
                if method.get("description"):
                    lines.append("    /** " + method["description"] + " */")
                lines.append("    " + ts_signature(method, receiver=name))
            lines.append("}")
        lines.append("")
    for constructor in contract["constructors"]:
        lines.append(ts_signature(constructor, "declare function "))
    lines.append("")
    for namespace in contract["namespaces"]:
        lines.append("declare namespace " + namespace["name"] + " {")
        for entry in namespace["functions"]:
            if entry.get("description"):
                lines.append("    /** " + entry["description"] + " */")
            lines.append("    " + ts_signature(entry, "function "))
        lines.extend(["}", ""])
    for callback in contract.get("callbacks", []):
        parameters = ["this: T"] + [p["name"] + ": " + type_text(p["type"], "ts") for p in callback["parameters"]]
        lines.append("type " + callback["name"] + "Callback<T extends " + callback["receiver"] + "> = (" + ", ".join(parameters) + ") => " + type_text(callback["returns"], "ts") + ";")
    return "\n".join(lines) + "\n"


def lua_function_type(entry, receiver=None):
    parameters = (["self: " + receiver] if receiver else []) + [p["name"] + ": " + type_text(p["type"], "lua") for p in entry["parameters"]]
    result = "fun(" + ", ".join(parameters) + ")"
    if entry["returns"] != "void":
        result += ": " + type_text(entry["returns"], "lua")
    return result


def luals(contract):
    lines = ["---@meta", "-- Generated by tools/generate_script_api.py; edit api/host-api.json.",
             "-- Editor metadata only. Never load this file as gameplay code.",
             "-- Access labels document runtime/authoring rules; LuaLS is not a runtime guard.", ""]
    for entry in contract["types"]:
        name = entry["name"]
        if entry.get("description"):
            lines.append("---" + entry["description"])
        if entry["kind"] == "alias":
            lines.append("---@alias " + name + " " + type_text(entry["type"], "lua"))
        elif entry["kind"] == "dictionary":
            lines.append("---@alias " + name + " table<string, " + type_text(entry["value"], "lua") + ">")
        else:
            if entry.get("nominal"):
                lines.append("---Native opaque value; TypeScript nominal markers are compile-only.")
            parent = ": " + entry["extends"] if "extends" in entry else ""
            lines.append("---@class " + name + parent)
            for field in entry["properties"]:
                note = " # " + field["access"] + ("; " + field["description"] if field.get("description") else "")
                lines.append("---@field " + field["name"] + " " + type_text(field["type"], "lua") + note)
            lines.append("local _" + name + " = {}")
            methods = {}
            for method in entry["methods"]:
                methods.setdefault(method["name"], []).append(method)
            for method_name, overloads in methods.items():
                # Generic fallback is the final signature, with specific literal
                # signatures retained as overloads for native lookup completion.
                method = overloads[-1]
                for generic in method.get("generics", []):
                    lines.append("---@generic " + generic["name"] + ": " + type_text(generic["extends"], "lua"))
                for overload in overloads[:-1]:
                    lines.append("---@overload " + lua_function_type(overload, name))
                if method.get("description"):
                    lines.append("---" + method["description"])
                for parameter in method["parameters"]:
                    lines.append("---@param " + parameter["name"] + " " + type_text(parameter["type"], "lua"))
                if method["returns"] != "void":
                    lines.append("---@return " + type_text(method["returns"], "lua"))
                lines.append("function _" + name + ":" + method_name + "(" + ", ".join(p["name"] for p in method["parameters"]) + ") end")
        lines.append("")
    constructors = {entry["name"]: entry for entry in contract["constructors"]}
    for namespace in contract["namespaces"]:
        name = namespace["name"]
        lines.append("---@class Pocket" + name + "Namespace")
        for entry in namespace["functions"]:
            lines.append("---@field " + entry["name"] + " " + lua_function_type(entry))
        if name in constructors:
            lines.append("---@overload " + lua_function_type(constructors[name]))
        lines.append(name + " = {}")
        lines.append("")
    for callback in contract.get("callbacks", []):
        lines.append("---@alias " + callback["name"] + "Callback " + lua_function_type(callback, callback["receiver"]))
    return "\n".join(lines) + "\n"



def raw_pieces(text, delimiter):
    """Adjacent raw string literals of at most 8000 characters each: MSVC caps a
    single literal piece at about 16 KB (the concatenation may be longer)."""
    pieces = [text[i:i + 8000] for i in range(0, len(text), 8000)] or [""]
    return "\n    ".join("R\"" + delimiter + "(" + piece + ")" + delimiter + "\"" for piece in pieces)

def header(contract):
    encoded = json.dumps(contract, ensure_ascii=True, separators=(",", ":"))
    delimiter = "POCKET_API"
    require(")" + delimiter + '"' not in encoded, "contract conflicts with C++ raw string delimiter")
    return ("// Generated by tools/generate_script_api.py; edit api/host-api.json.\n"
            "#ifndef POCKET2D_SCRIPT_API_CONTRACT_H\n#define POCKET2D_SCRIPT_API_CONTRACT_H\n"
            "#include <string_view>\nnamespace PocketScriptAPI {\n"
            "inline constexpr std::string_view kContractJson = " + raw_pieces(encoded, delimiter) + ";\n"
            "} // namespace PocketScriptAPI\n#endif\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail if any generated file is missing or stale")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1], help="repository root (for isolated checks)")
    options = parser.parse_args()
    try:
        source = options.root / "api/host-api.json"
        contract = json.loads(source.read_text(encoding="utf-8"))
        validate(contract)
        outputs = {"api/types/pocket2d.d.ts": typescript(contract), "api/types/pocket2d.lua": luals(contract),
                   "src/engine/scripting/ScriptAPIContract.h": header(contract)}
        stale = []
        for relative, content in outputs.items():
            path = options.root / relative
            if options.check:
                if not path.is_file() or path.read_text(encoding="utf-8") != content:
                    stale.append(relative)
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content, encoding="utf-8")
        if stale:
            print("stale generated script API: " + ", ".join(stale), file=sys.stderr)
            return 1
        print("script API generation " + ("checked" if options.check else "complete"))
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print("script API generation failed: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
