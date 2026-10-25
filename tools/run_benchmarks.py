#!/usr/bin/env python3
"""Run Pocket2D's fixed benchmark workloads and record one JSON result.

Workloads (docs/development/benchmark-harness.md):
  W1 static sprites   tilemap_benchmark 256 BACKEND (sprite path, 65,536 sprites)
  W2 bunnymark        bunnymark_benchmark BACKEND N (per_entity and batch)
  W3 Box2D pyramid    physics_benchmark 100 (5,050 bodies, collapsing)
  W4 tilemap scroll   tilemap_benchmark 256 BACKEND (native path, scrolling)
  W5 mixed village    two_world_village stepped headlessly over the agent protocol
  text                text_benchmark BACKEND (280 Text.Draw per frame)

Results go to build/bench/<UTC time>.json (ignored by Git). Use a Release
build. Each benchmark does its own warmup; this script only gathers results.
"""
import argparse
import csv
import datetime
import io
import json
import os
import platform
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[1]


def run(command, env=None):
    result = subprocess.run(command, capture_output=True, text=True, timeout=1800,
                            env=dict(os.environ, **(env or {})))
    if result.returncode != 0:
        raise RuntimeError("%s failed: %s" % (" ".join(map(str, command)), result.stderr[-2000:]))
    return result.stdout


def rows(output):
    lines = [line for line in output.splitlines() if line and not line.startswith("#")]
    return list(csv.DictReader(io.StringIO("\n".join(lines))))


def machine():
    info = {"platform": platform.platform(), "python": platform.python_version(), "cpus": os.cpu_count()}
    if sys.platform == "darwin":
        for key, name in (("machdep.cpu.brand_string", "cpu"), ("hw.memsize", "memory_bytes")):
            try:
                info[name] = subprocess.run(["sysctl", "-n", key], capture_output=True, text=True).stdout.strip()
            except OSError:
                pass
    try:
        info["commit"] = subprocess.run(["git", "-C", str(REPOSITORY), "rev-parse", "HEAD"],
                                        capture_output=True, text=True).stdout.strip()
        info["dirty"] = bool(subprocess.run(["git", "-C", str(REPOSITORY), "status", "--porcelain"],
                                            capture_output=True, text=True).stdout.strip())
    except OSError:
        pass
    return info


def village(game, frames):
    """Per-frame wall time of stepping two_world_village headlessly, held right."""
    project = REPOSITORY / "tests/fixtures/two_world_village"
    with tempfile.TemporaryDirectory(prefix="pocket-bench-village-") as saves:
        process = subprocess.Popen([game, "--headless", "--agent", "--project", str(project), "--save-dir", saves],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        try:
            def request(command):
                process.stdin.write(json.dumps(command) + "\n")
                process.stdin.flush()
                return json.loads(process.stdout.readline())
            request({"cmd": "step", "frames": 60})  # warmup
            start = time.perf_counter()
            response = request({"cmd": "step", "frames": frames, "keys": ["right"], "select": {"limit": 0}})
            elapsed = time.perf_counter() - start
            if not response.get("ok"):
                raise RuntimeError("village step failed: %s" % response.get("error"))
            request({"cmd": "quit"})
        finally:
            if process.poll() is None:
                process.kill()
    return {"frames": frames, "ms_per_frame": elapsed * 1000.0 / frames}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", default=str(REPOSITORY / "build/ninja-release"), help="Release build directory")
    parser.add_argument("--backend", default="gpu", help="POCKET_RENDER_DRIVER for rendered workloads")
    parser.add_argument("--quick", action="store_true", help="fewer samples (smoke test, not for comparison)")
    options = parser.parse_args()
    build = Path(options.build)
    tests = build / "tests"
    samples = "20" if options.quick else "120"
    result = {"started": datetime.datetime.now(datetime.timezone.utc).isoformat(), "backend": options.backend,
              "quick": options.quick, "machine": machine(), "workloads": {}}
    workloads = result["workloads"]

    tilemap = rows(run([tests / "tilemap_benchmark", "256", options.backend, samples]))
    workloads["W1_static_sprites"] = [r for r in tilemap if r["path"] == "sprites"]
    workloads["W4_tilemap"] = [r for r in tilemap if r["path"] == "native"]
    workloads["W2_bunnymark"] = []
    for count in ("10000", "50000"):
        for mode in ("per_entity", "batch"):
            workloads["W2_bunnymark"] += rows(run([tests / "bunnymark_benchmark", options.backend, count, "30",
                                                   samples, mode]))
    workloads["W3_box2d_pyramid"] = []
    for workers in ("0", "auto"):
        workloads["W3_box2d_pyramid"] += rows(run([tests / "physics_benchmark", "100", "0", samples],
                                                  env={"POCKET_PHYSICS_WORKERS": workers}))
    workloads["W5_village"] = village(str(build / "game"), 300 if options.quick else 1200)
    workloads["text"] = rows(run([tests / "text_benchmark", options.backend, samples]))

    output = REPOSITORY / "build/bench"
    output.mkdir(parents=True, exist_ok=True)
    path = output / (result["started"].replace(":", "").replace("+0000", "Z").split(".")[0] + ".json")
    path.write_text(json.dumps(result, indent=2) + "\n")
    print("W1 static sprites cpu p50 ms:", ", ".join("%s %s" % (r["scenario"], r["cpu_p50_ms"])
                                                     for r in workloads["W1_static_sprites"]))
    print("W2 bunnymark cpu p50 ms:", ", ".join("%s/%s %s" % (r["bunnies"], r["mode"], r["cpu_p50_ms"])
                                                for r in workloads["W2_bunnymark"]))
    print("W3 Box2D step p50 ms:", ", ".join("workers=%s %s" % (r["workers"], r["step_p50_ms"])
                                             for r in workloads["W3_box2d_pyramid"]))
    print("W4 tilemap cpu p50 ms:", ", ".join("%s %s" % (r["scenario"], r["cpu_p50_ms"]) for r in workloads["W4_tilemap"]))
    print("W5 village ms/frame: %.3f" % workloads["W5_village"]["ms_per_frame"])
    print("text cpu p50 ms:", workloads["text"][0]["cpu_p50_ms"])
    print("wrote", path.relative_to(REPOSITORY))
    return 0


if __name__ == "__main__":
    sys.exit(main())
