#!/usr/bin/env python3
"""Replay files: record a session, replay it frame-exactly, and locate divergence."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from agent_protocol_test import check


def record(executable, project, path, requests):
    command = [executable, "--headless", "--agent", "--project", str(project), "--seed", "42", "--record", str(path)]
    lines = "".join(json.dumps(request) + "\n" for request in requests)
    result = subprocess.run(command, input=lines, capture_output=True, text=True, encoding="utf-8", timeout=120)
    check(result.returncode == 0, "recording session failed: " + result.stderr[-2000:])
    responses = [json.loads(line) for line in result.stdout.splitlines()]
    check(all(response["ok"] for response in responses), "recorded requests succeed")
    return responses


def replay(executable, project, path):
    result = subprocess.run([executable, "--headless", "--replay", str(path), "--project", str(project)],
                            capture_output=True, text=True, encoding="utf-8", timeout=120)
    lines = result.stdout.splitlines()
    check(len(lines) == 1, "replay prints one summary line: " + result.stdout + result.stderr[-2000:])
    return result.returncode, json.loads(lines[0])


def run(executable, fixture):
    with tempfile.TemporaryDirectory(prefix="pocket-replay-") as directory:
        root = Path(directory)
        project = root / "project"
        shutil.copytree(fixture, project)
        path = root / "session.jsonl"
        responses = record(executable, project, path, [
            {"cmd": "step", "frames": 30, "keys": ["right"], "id": "a"},
            {"cmd": "describe"},
            {"cmd": "run_until", "max_frames": 120, "keys": [], "until": [{"actor_exists": "nobody"}]},
            {"cmd": "observe"},
            {"cmd": "reset", "seed": 7},
            {"cmd": "step", "frames": 45, "keys": ["left", "space"]},
            {"cmd": "quit"}])
        lines = [json.loads(line) for line in path.read_text().splitlines()]
        header = lines[0]
        check(header["pocket_replay"] == 1 and header["protocol"] == 2 and header["seed"] == 42 and
              header["project_digest"].startswith("fnv1a64:"), "replay header")
        check([line["request"]["cmd"] for line in lines[2:]] == ["step", "run_until", "observe", "reset", "step"],
              "simulating and observing requests are recorded; describe and quit are not")
        check("id" not in lines[2]["request"] and "diagnostics" not in lines[2]["response"],
              "session-only fields are not recorded")

        code, summary = replay(executable, project, path)
        check(code == 0 and summary == {"ok": True, "protocol": 2, "records": 5,
                                        "frame": responses[-2]["frame"], "project_changed": False},
              "replay matches: " + json.dumps(summary))

        tampered = json.loads(json.dumps(lines))
        actor = tampered[5]["response"]["actors"][0]
        actor["name"] = actor["name"] + "_changed"
        bad = root / "tampered.jsonl"
        bad.write_text("".join(json.dumps(line) + "\n" for line in tampered))
        code, summary = replay(executable, project, bad)
        check(code == 1 and summary["code"] == "replay_diverged" and summary["record"] == 4 and
              summary["command"] == "reset" and summary["path"] == "/actors/0/name" and
              summary["expected"] == actor["name"], "divergence names the record and field: " + json.dumps(summary))

        (project / "NOTES.txt").write_text("not gameplay")
        code, summary = replay(executable, project, path)
        check(code == 0 and summary["project_changed"], "project changes are reported without failing a match")

        (root / "invalid.jsonl").write_text('{"pocket_replay":1}\n')
        code, summary = replay(executable, project, root / "invalid.jsonl")
        check(code == 1 and summary["code"] == "replay_invalid", "malformed replays are rejected")
    print("agent replay record/verify regressions passed")


if __name__ == "__main__":
    check(len(sys.argv) == 3, "usage: agent_replay_test.py RUNTIME FIXTURE_PROJECT")
    run(str(Path(sys.argv[1]).resolve()), Path(sys.argv[2]).resolve())
