#!/usr/bin/env python3
"""Compare real translation units against baseline/candidate LuaBridge headers.

Runs compile_commands.json argv without a shell. No production object, dependency
file, precompiled header or candidate shim is produced. Compiler transcripts and
machine-readable results belong in an ignored output directory.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys

SOURCES = (
    "src/engine/scripting/luaapi/ActorAPI.cpp",
    "src/engine/scripting/luaapi/RigidbodyAPI.cpp",
    "src/engine/scripting/manager/Shared.cpp",
    "src/engine/scripting/manager/SceneComponents.cpp",
)
DIAGNOSTIC = re.compile(r"^(.*?):(\d+):(?:(\d+):)?\s*(fatal error|error|warning):\s*(.*)$")
INCLUDE = re.compile(r"^\.+ (.+)$")


def resolve_under(repo: Path, value: Path) -> Path:
    return (value if value.is_absolute() else repo / value).resolve()


def syntax_arguments(entry: dict) -> list[str]:
    argv = list(entry["arguments"]) if "arguments" in entry else shlex.split(entry["command"])
    if not argv:
        raise ValueError("empty compilation command")
    result = [argv[0]]
    index = 1
    while index < len(argv):
        value = argv[index]
        if value in ("-o", "-MF", "-MT", "-MQ", "-MJ", "--serialize-diagnostics"):
            if index + 1 >= len(argv):
                raise ValueError(f"missing argument for {value}")
            index += 2
            continue
        if value in ("-c", "-MD", "-MMD", "-MP", "-MG", "-M", "-MM"):
            index += 1
            continue
        if value.startswith(("-o", "-MF", "-MT", "-MQ", "-MJ", "-save-temps", "--serialize-diagnostics=")):
            index += 1
            continue
        if value.startswith("@"):
            raise ValueError("response files are not supported: expose argv in compile_commands.json")
        result.append(value)
        index += 1
    return result


def run_to_log(argv: list[str], directory: Path, log: Path, *, preprocess: bool = False) -> int:
    with log.open("w", encoding="utf-8") as stream:
        try:
            result = subprocess.run(argv, cwd=directory, stdin=subprocess.DEVNULL,
                                    stdout=subprocess.DEVNULL if preprocess else stream,
                                    stderr=stream, timeout=90, check=False)
            return result.returncode
        except subprocess.TimeoutExpired:
            stream.write("\ninventory: compiler exceeded the 90-second per-command limit\n")
            return 124


def diagnostics(path: Path) -> tuple[int, int, list[dict]]:
    errors = warnings = 0
    first_errors: list[dict] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = DIAGNOSTIC.match(line)
        if not match:
            continue
        source, row, column, severity, message = match.groups()
        if severity == "warning":
            warnings += 1
        else:
            errors += 1
            if len(first_errors) < 8:
                first_errors.append({"file": source, "line": int(row),
                                     "column": int(column) if column else None,
                                     "message": message[:1200]})
    return errors, warnings, first_errors


def verify_headers(log: Path, directory: Path, expected: Path) -> dict:
    found: set[Path] = set()
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        match = INCLUDE.match(line)
        if not match:
            continue
        value = match.group(1).removesuffix(" (framework directory)")
        path = Path(value)
        if "LuaBridge" in path.parts:
            found.add((path if path.is_absolute() else directory / path).resolve())
    main = expected / "LuaBridge.h"
    outside = sorted(str(path) for path in found if not path.is_relative_to(expected))
    return {"verified": main in found and not outside,
            "resolved_header": str(main) if main in found else None,
            "luabridge_header_count": len(found), "mixed_header_paths": outside}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True, help="Build directory containing compile_commands.json")
    parser.add_argument("--candidate", type=Path, required=True, help="Candidate include root containing LuaBridge/LuaBridge.h")
    parser.add_argument("--output", type=Path, required=True, help="Ignored output directory; paths are relative to --repo")
    args = parser.parse_args()
    repo = args.repo.resolve()
    build, candidate, output = (resolve_under(repo, value) for value in (args.build, args.candidate, args.output))
    expected = {"baseline": repo / "thirdparty/LuaBridge", "candidate": candidate / "LuaBridge"}
    for directory in expected.values():
        if not (directory / "LuaBridge.h").is_file():
            raise ValueError(f"missing LuaBridge/LuaBridge.h under {directory}")
    if output.is_relative_to(repo):
        ignored = subprocess.run(["git", "check-ignore", "--quiet", "--", str(output)], cwd=repo,
                                 stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if ignored.returncode != 0:
            raise ValueError("output inside repository must be Git-ignored")
    entries = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
    selected = []
    for source in SOURCES:
        matches = [entry for entry in entries if resolve_under(Path(entry["directory"]), Path(entry["file"])) == repo / source]
        if len(matches) != 1:
            raise ValueError(f"expected one compilation entry for {source}, found {len(matches)}")
        selected.append((source, matches[0]))
    output.mkdir(parents=True, exist_ok=True)
    compiler = syntax_arguments(selected[0][1])[0]
    version = subprocess.run([compiler, "--version"], cwd=repo, stdin=subprocess.DEVNULL,
                             capture_output=True, text=True, timeout=15, check=True).stdout
    (output / "compiler.txt").write_text(version, encoding="utf-8")
    clang = "clang" in version.lower()
    if not clang and not any(name in version.lower() for name in ("gcc", "g++", "free software foundation")):
        raise ValueError("inventory currently supports Clang/GNU-style compile database entries")
    report = {"compiler": version.splitlines()[0], "repo": str(repo), "build": str(build),
              "candidate": str(candidate), "syntax_only": True, "results": []}
    for source, entry in selected:
        directory = Path(entry["directory"]).resolve()
        for variant in ("baseline", "candidate"):
            argv = syntax_arguments(entry)
            if variant == "candidate":
                argv[1:1] = ["-I", str(candidate)]  # precedes every existing normal/system include
            flags = ["-ferror-limit=0", "-fno-color-diagnostics"] if clang else ["-fmax-errors=0", "-fdiagnostics-color=never"]
            syntax = argv + flags + ["-fsyntax-only"]
            preprocessor = argv + ["-E", "-H"]
            stem = Path(source).stem + "." + variant
            (output / (stem + ".argv.json")).write_text(json.dumps({"cwd": str(directory), "syntax": syntax,
                "preprocessor": preprocessor}, indent=2) + "\n", encoding="utf-8")
            include_log = output / (stem + ".includes.log")
            preprocess_exit = run_to_log(preprocessor, directory, include_log, preprocess=True)
            headers = verify_headers(include_log, directory, expected[variant].resolve())
            compile_log = output / (stem + ".compile.log")
            compile_exit = run_to_log(syntax, directory, compile_log)
            errors, warnings, first_errors = diagnostics(compile_log)
            result = {"source": source, "variant": variant, "preprocess_exit": preprocess_exit,
                      **headers, "compile_exit": compile_exit, "error_count": errors,
                      "warning_count": warnings, "first_errors": first_errors,
                      "diagnostics": compile_log.name, "include_trace": include_log.name}
            report["results"].append(result)
            print(f"{Path(source).name:22} {variant:9} header={'verified' if headers['verified'] else 'UNVERIFIED'} "
                  f"exit={compile_exit} errors={errors} warnings={warnings}", flush=True)
    baseline = [row for row in report["results"] if row["variant"] == "baseline"]
    report["baseline_passed"] = all(row["compile_exit"] == 0 and row["preprocess_exit"] == 0 for row in baseline)
    report["headers_verified"] = all(row["verified"] for row in report["results"])
    report["candidate_syntax_compatible"] = all(row["compile_exit"] == 0 and row["preprocess_exit"] == 0 for row in report["results"] if row["variant"] == "candidate")
    # A candidate #error is a genuine incompatibility when its include path is
    # proven; it must not invalidate an otherwise passing baseline inventory.
    report["inventory_valid"] = report["baseline_passed"] and report["headers_verified"]
    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"inventory_valid={report['inventory_valid']} candidate_syntax_compatible={report['candidate_syntax_compatible']}")
    # Candidate compile failures are the inventory's result, not a tool failure.
    return 0 if report["inventory_valid"] else 2


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"compile inventory failed: {error}", file=sys.stderr)
        raise SystemExit(2)
