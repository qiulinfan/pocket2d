#!/usr/bin/env python3
"""pocket CLI: new, check, test (scenario scripts and replays), render, replay."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

REPOSITORY = Path(__file__).resolve().parents[1]


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def pocket(game, *arguments, expect=0):
    result = subprocess.run([sys.executable, str(REPOSITORY / "tools/pocket"), "--game", game, *arguments],
                            capture_output=True, text=True, timeout=180)
    check(result.returncode == expect, "pocket %s exited %d, expected %d:\n%s%s" %
          (" ".join(arguments), result.returncode, expect, result.stdout, result.stderr))
    return result.stdout


def diagnostics(game, *arguments, expect=0):
    return [json.loads(line) for line in pocket(game, *arguments, "--json", expect=expect).splitlines()]


def run(game):
    with tempfile.TemporaryDirectory(prefix="pocket-cli-") as directory:
        root = Path(directory)
        project = root / "game"
        pocket(game, "new", str(project))
        check((project / "AGENTS.md").is_file() and (project / "actions.json").is_file(), "scaffold files")
        check("not empty" in subprocess.run([sys.executable, str(REPOSITORY / "tools/pocket"), "new", str(project)],
                                            capture_output=True, text=True).stderr, "new refuses a non-empty directory")
        report = diagnostics(game, "check", str(project))
        check(not [d for d in report if d["severity"] == "error"], "a new project checks clean: %r" % report)
        check(any(d["code"] == "runtime" for d in report), "check starts the runtime")
        report = diagnostics(game, "test", str(project))
        check([d["code"] for d in report] == ["scenario_passed"], "the scaffold scenario passes: %r" % report)

        # A replay recorded through `pocket run --record` is verified by `pocket test`.
        (project / "replays").mkdir()
        replay = project / "replays/walk.jsonl"
        session = subprocess.run([sys.executable, str(REPOSITORY / "tools/pocket"), "--game", game, "run",
                                  str(project), "--record", str(replay)],
                                 input='{"cmd":"step","frames":20,"keys":["d"]}\n{"cmd":"quit"}\n',
                                 capture_output=True, text=True, timeout=60)
        check(session.returncode == 0 and replay.is_file(), "pocket run records: " + session.stderr)
        report = diagnostics(game, "test", str(project))
        check(sorted(d["code"] for d in report) == ["replay_passed", "scenario_passed"], "replay passes: %r" % report)
        check("records" in pocket(game, "replay", str(project), str(replay)), "pocket replay prints its summary")

        # Failures are reported with codes and a nonzero exit.
        scenario = project / "scenarios/move_right.scenario.json"
        data = json.loads(scenario.read_text())
        data["script"] = [{"frames": 120, "actions": {"move": [-1, 0]}}]
        scenario.write_text(json.dumps(data))
        report = diagnostics(game, "test", str(project), expect=1)
        check(any(d["code"] == "scenario_failed" and "got truncated" in d["message"] for d in report),
              "a script that misses its goal fails: %r" % report)
        (project / "scenes/main.scene").write_text("{not json")
        report = diagnostics(game, "check", str(project), expect=1)
        check(any(d["code"] == "invalid_json" and d["file"] == "scenes/main.scene" for d in report),
              "invalid JSON is located: %r" % report)
        check(any(d["severity"] == "error" and d["code"] != "invalid_json" for d in report),
              "the runtime reports the broken scene too")
        (project / "scenes/main.scene").write_text(json.dumps({"actors": [
            {"uid": 1, "name": "player", "components": {"x": {"type": "Missing"}}}]}))
        report = diagnostics(game, "check", str(project), expect=1)
        check(any(d["severity"] == "error" for d in report), "unknown component types fail the check: %r" % report)

        image = root / "frame.png"
        pocket(game, "render", str(REPOSITORY / "tests/fixtures/village_defense"), "--out", str(image),
               "--frames", "5", "--width", "160", "--height", "90")
        check(image.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n", "render writes a PNG")
    print("pocket CLI regressions passed")


if __name__ == "__main__":
    check(len(sys.argv) == 2, "usage: pocket_cli_test.py RUNTIME")
    run(str(Path(sys.argv[1]).resolve()))
