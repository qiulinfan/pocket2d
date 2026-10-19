#!/usr/bin/env python3
"""Independent checks of a supplied Repair.lua through real Pocket2D JSONL.

No model code is repaired here. Only the candidate is copied into pristine
fixture projects. Runtime output and modified case projects stay in output-dir.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess


CASES = [
    ("repair_hold_release", 1, 2, "countdown", 0.0, 3.0),
    ("last_resource", 1, 1, "countdown", 0.0, 3.0),
    ("exact_distance_boundary", 2, 1, "countdown", 0.0, 2.75),
    ("outside_distance_boundary", 2, 1, "countdown", 0.0, 2.749),
    ("far_away", 2, 1, "countdown", 0.0, 0.0),
    ("no_resources", 2, 0, "countdown", 0.0, 3.0),
    ("negative_resources", 2, -1, "countdown", 0.0, 3.0),
    ("full_altar", 3, 1, "countdown", 0.0, 3.0),
    ("overfull_altar", 4, 1, "countdown", 0.0, 3.0),
    ("dead_altar", 0, 1, "countdown", 0.0, 3.0),
    ("won", 2, 1, "won", 0.0, 3.0),
    ("lost", 2, 1, "lost", 0.0, 3.0),
    ("other_nonterminal_state", 2, 1, "waiting", 0.0, 3.0),
    ("diagonal_boundary", 2, 1, "countdown", 0.75, 3.0),
    ("diagonal_outside", 2, 1, "countdown", 0.9, 3.0),
    ("translated_altar", 2, 1, "countdown", 3.0, 0.0, 3.0, 1.0),
]


def prepare_case(source, destination, candidate, case):
    shutil.copytree(source, destination)
    shutil.copyfile(candidate, destination / "component_types/Repair.lua")
    name, health, supplies, state, x, y, *altar_position = case
    path = destination / "scenes/defense.scene"
    scene = json.loads(path.read_text())
    for actor in scene["actors"]:
        if actor["name"] == "player":
            actor["components"]["repair"] = {"type": "Repair"}
            for key in ("body", "transform"):
                actor["components"][key].update(x=x, y=y)
        elif actor["name"] == "village":
            actor["components"]["logic"].update(
                health=health, gathered=supplies, state=state,
                next_wave=100000, interval=100000,
            )
        elif actor["name"] == "altar" and altar_position:
            for key in ("body", "transform"):
                actor["components"][key].update(x=altar_position[0], y=altar_position[1])
    path.write_text(json.dumps(scene, indent=2) + "\n")


def village(snapshot):
    actors = [a for a in snapshot["actors"] if a["name"] == "village"]
    assert len(actors) == 1, "missing or duplicate village"
    components = [c for c in actors[0]["components"] if c["type"] == "Village"]
    assert len(components) == 1, "missing or duplicate Village component"
    return components[0]["properties"]


def run_case(runtime, fixture, candidate, output, case):
    name, health, supplies, state, x, y, *altar_position = case
    case_dir = output / name
    project = case_dir / "project"
    case_dir.mkdir(parents=True)
    prepare_case(fixture, project, candidate, case)
    commands = [
        {"cmd": "observe"},
        {"cmd": "step", "frames": 2, "keys": []},
        {"cmd": "step", "frames": 1, "keys": ["e"]},
        {"cmd": "step", "frames": 10, "keys": ["e"]},
        {"cmd": "step", "frames": 1, "keys": []},
        {"cmd": "step", "frames": 1, "keys": ["e"]},
        {"cmd": "quit"},
    ]
    encoded = "".join(json.dumps(command) + "\n" for command in commands)
    (case_dir / "commands.jsonl").write_text(encoded)
    run = subprocess.run(
        [str(runtime), "--headless", "--agent", "--project", str(project), "--seed", "42"],
        input=encoded, text=True, encoding="utf-8", capture_output=True, timeout=20,
    )
    (case_dir / "stdout.jsonl").write_text(run.stdout)
    (case_dir / "stderr.log").write_text(run.stderr)
    assert run.returncode == 0, f"runtime exit {run.returncode}"
    responses = [json.loads(line) for line in run.stdout.splitlines()]
    assert len(responses) == len(commands), "one response per command required"
    for response in responses:
        assert response.get("ok") is True, response.get("error", "runtime failed")
    altar_x, altar_y = altar_position or (0, 4)
    near = (x-altar_x)**2 + (y-altar_y)**2 <= 1.25*1.25
    expected = [(health, supplies), (health, supplies)]
    if near and supplies >= 1 and 0 < health < 3 and state not in ("won", "lost"):
        health, supplies = health+1, supplies-1
    expected += [(health, supplies)] * 3  # press, hold, release
    if near and supplies >= 1 and 0 < health < 3 and state not in ("won", "lost"):
        health, supplies = health+1, supplies-1
    expected.append((health, supplies))
    for index, (expected_health, expected_supplies) in enumerate(expected):
        props = village(responses[index])
        assert props["health"] == expected_health, (
            f"checkpoint {index}: health {props['health']}, expected {expected_health}"
        )
        assert props["gathered"] == expected_supplies, (
            f"checkpoint {index}: gathered {props['gathered']}, expected {expected_supplies}"
        )
    return {"case": name, "passed": True, "checkpoints": len(expected)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    results = []
    for case in CASES:
        try:
            results.append(run_case(args.runtime.resolve(), args.fixture.resolve(),
                                    args.candidate.resolve(), output, case))
        except (AssertionError, KeyError, ValueError, OSError, subprocess.TimeoutExpired) as error:
            results.append({"case": case[0], "passed": False, "error": str(error)})
    report = {"passed": sum(result["passed"] for result in results),
              "total": len(results), "cases": results}
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))
    return 0 if report["passed"] == report["total"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
