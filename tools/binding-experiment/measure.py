#!/usr/bin/env python3
"""Measure already verified, matching Release probes. Output belongs under build/."""
import argparse
import csv
import io
import json
import platform
import resource
import shlex
import statistics
import subprocess
import time
from pathlib import Path


def run(argv, cwd=None):
    return subprocess.run(argv, cwd=cwd, text=True, capture_output=True, check=True, timeout=120)


def compile_command(entry, output):
    source = entry.get("arguments") or shlex.split(entry["command"])
    result = []
    skip = False
    for arg in source:
        if skip:
            skip = False
            continue
        if arg in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
            continue
        if arg in ("-MD", "-MMD"):
            continue
        result.append(arg)
    result.extend(["-o", str(output)])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--iterations", type=int, default=200000)
    parser.add_argument("--runs", type=int, default=3)
    args = parser.parse_args()
    if not 1 <= args.runs <= 10 or not 1000 <= args.iterations <= 10000000:
        parser.error("runs must be 1..10 and iterations 1000..10000000")
    build, output = args.build.resolve(), args.output.resolve()
    cache = (build / "CMakeCache.txt").read_text()
    if "CMAKE_BUILD_TYPE:STRING=Release\n" not in cache or "POCKET_BINDING_SANITIZE:BOOL=ON" in cache:
        parser.error("measure only the unsanitized Release build")
    output.mkdir(parents=True, exist_ok=True)
    entries = json.loads((build / "compile_commands.json").read_text())
    commands = {}
    for variant in ("baseline", "candidate"):
        target = f"binding_benchmark_{variant}.dir"
        commands[variant] = next(e for e in entries if target in e.get("command", " ".join(e.get("arguments", []))))
    compiler = (commands["baseline"].get("arguments") or shlex.split(commands["baseline"]["command"]))[0]
    metadata = {
        "platform": platform.platform(), "machine": platform.machine(),
        "compiler": run([compiler, "--version"]).stdout,
        "iterations": args.iterations, "warmups_per_process": 3,
        "samples_per_process": 7, "interleaved_runs": args.runs,
        "compile_commands": commands,
        "candidate_config": {"SAFE_LUA_C_EXCEPTION_HANDLING": 1, "SAFE_STACK_CHECKS": 1, "STRICT_STACK_CONVERSIONS": 0},
        "binary_bytes": {v: (build / f"binding_benchmark_{v}").stat().st_size for v in commands},
        "scope": "One C++ probe TU compile; isolated binding operations, no engine/frame/render workload",
    }
    rows = []
    for index in range(args.runs):
        order = ("baseline", "candidate") if index % 2 == 0 else ("candidate", "baseline")
        for variant in order:
            result = run([str(build / f"binding_benchmark_{variant}"), str(args.iterations), "7"])
            (output / f"runtime-{index}-{variant}.csv").write_text(result.stdout)
            for row in csv.DictReader(io.StringIO(result.stdout)):
                row.update(run=index, variant=variant)
                rows.append(row)
    summaries = []
    for variant in commands:
        for workload in sorted({row["workload"] for row in rows}):
            values = sorted(float(row["ns_per_operation"]) for row in rows if row["variant"] == variant and row["workload"] == workload)
            summaries.append({"variant": variant, "workload": workload, "samples": len(values),
                              "median_ns": statistics.median(values), "min_ns": min(values),
                              "p95_ns": values[min(len(values)-1, (95 * len(values) + 99)//100 - 1)]})
    compile_rows = []
    # One untimed cache warmup plus three fresh object compilations per variant.
    # No runtime measurements run concurrently with the compiler.
    for sample in range(-1, 3):
        order = ("baseline", "candidate") if sample % 2 == 0 else ("candidate", "baseline")
        for variant in order:
            entry = commands[variant]
            before = resource.getrusage(resource.RUSAGE_CHILDREN)
            start = time.perf_counter()
            result = run(compile_command(entry, output / f"compile-{sample}-{variant}.o"), cwd=entry["directory"])
            elapsed = time.perf_counter() - start
            after = resource.getrusage(resource.RUSAGE_CHILDREN)
            (output / f"compile-{sample}-{variant}.stderr").write_text(result.stderr)
            if sample >= 0:
                compile_rows.append({"variant": variant, "sample": sample, "wall_seconds": elapsed,
                                     "cpu_seconds": after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime})
    result = {"metadata": metadata, "runtime": summaries, "compilation": compile_rows}
    (output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"runtime": summaries, "binary_bytes": metadata["binary_bytes"],
                      "compile_median_seconds": {v: statistics.median(r["wall_seconds"] for r in compile_rows if r["variant"] == v) for v in commands}}, indent=2))


if __name__ == "__main__":
    main()
