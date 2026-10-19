#!/usr/bin/env python3
"""`pocket package` lays out each platform's game from a runtime kit:
a macOS .app (runtime and libraries in Contents/MacOS, project and engine
resources in Contents/Resources), Windows and Linux folders, and a web page
with project.pack; development files stay out. A packaged runtime (this
build's) starts the project beside it with no --project argument."""
import json
from pathlib import Path
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "clients/python"))
from pocket2d.package import package  # noqa: E402

FIXTURE = Path(__file__).resolve().parent / "fixtures" / "agent_session"


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def fake_kit(root, platform):
    kit = root / ("kit-" + platform)
    (kit / ".engine/system/fonts").mkdir(parents=True)
    (kit / ".engine/system/fonts/system.ttf").write_bytes(b"font")
    (kit / ".engine/system/icon.png").write_bytes(b"not a png")
    if platform == "windows":
        (kit / "game.exe").write_bytes(b"exe")
        (kit / "SDL3.dll").write_bytes(b"dll")
    elif platform == "web":
        (kit / "game.html").write_text("<title>Pocket2D</title>")
        (kit / "game.js").write_text("js")
        (kit / "game.wasm").write_bytes(b"wasm")
    else:
        (kit / "game").write_bytes(b"binary")
        (kit / "lib").mkdir()
        (kit / "lib/libSDL3.real").write_bytes(b"lib")
        (kit / "lib/libSDL3.link").symlink_to("libSDL3.real")
    return kit


def fake_project(root):
    project = root / "Fixture Game"
    shutil.copytree(FIXTURE, project)
    config = json.loads((project / "game.config").read_text())
    config.update({"game_title": "Fixture Game", "package_exclude": ["secret*"]})
    (project / "game.config").write_text(json.dumps(config))
    for junk in ("tools/build.py", "scenarios/a.json", "NOTES.md", ".hidden", "secret.txt"):
        (project / junk).parent.mkdir(parents=True, exist_ok=True)
        (project / junk).write_text("dev only")
    return project


def check_project(project_dir):
    check((project_dir / "game.config").is_file(), "the project's game.config ships")
    check((project_dir / "scenes").is_dir(), "the project's scenes ship")
    for junk in ("tools", "scenarios", "NOTES.md", ".hidden", "secret.txt"):
        check(not (project_dir / junk).exists(), "%s stays out of the package" % junk)


def check_layouts(root):
    project = fake_project(root)
    out = root / "out"
    app, archive = package(project, "mac", fake_kit(root, "mac"), out)
    contents = app / "Contents"
    check(app.name == "Fixture Game.app", "the app is named after the game")
    check(plistlib.loads((contents / "Info.plist").read_bytes())["CFBundleExecutable"] == "game", "Info.plist")
    check((contents / "MacOS/game").is_file() and (contents / "MacOS/lib/libSDL3.link").is_file(), "runtime")
    check(not (contents / "MacOS/lib/libSDL3.link").is_symlink(), "library links become files")
    check((contents / "Resources/.engine/system/fonts/system.ttf").is_file(), "engine resources")
    check_project(contents / "Resources/project")
    check(archive.is_file(), "a zip beside the app")

    folder, archive = package(project, "windows", fake_kit(root, "windows"), out)
    check((folder / "FixtureGame.exe").is_file() and (folder / "SDL3.dll").is_file(), "exe and DLLs together")
    check_project(folder / "project")
    check(archive.suffix == ".zip", "a Windows zip")

    folder, archive = package(project, "linux", fake_kit(root, "linux"), out)
    check((folder / "FixtureGame").is_file() and (folder / "lib/libSDL3.link").is_file(), "Linux layout")
    check_project(folder / "project")
    check(archive.name.endswith(".tar.gz"), "a Linux tar.gz")

    folder, archive = package(project, "web", fake_kit(root, "web"), out)
    check("<title>Fixture Game</title>" in (folder / "index.html").read_text(), "the page is titled")
    data = (folder / "project.pack").read_bytes()
    length = struct.unpack("<I", data[:4])[0]
    manifest = json.loads(data[4:4 + length])
    paths = [f["path"] for f in manifest["files"]]
    check("project/game.config" in paths and ".engine/system/fonts/system.ttf" in paths, "the pack's files")
    check(not any("tools/" in p or p.endswith(".md") for p in paths), "development files stay out of the pack")
    check(4 + length + sum(f["size"] for f in manifest["files"]) == len(data), "the pack's sizes add up")


def check_packaged_runtime(root, executable):
    """This build's runtime, beside a project folder, runs it without --project."""
    folder = root / "runnable"
    folder.mkdir()
    shutil.copy2(executable, folder / Path(executable).name)
    # Windows loads the SDL DLLs from beside the executable; without them the
    # loader's missing-DLL dialog blocks the run.
    for library in Path(executable).parent.glob("*.dll"):
        shutil.copy2(library, folder / library.name)
    shutil.copytree(FIXTURE, folder / "project")
    shutil.copytree(ROOT / ".engine/system", folder / ".engine/system")
    capture = root / "frame.png"
    result = subprocess.run([str(folder / Path(executable).name), "--frames", "3", "--capture", str(capture)],
                            cwd=root, capture_output=True, text=True, encoding="utf-8", timeout=60)
    check(result.returncode == 0, "the packaged runtime starts: " + result.stderr[-400:])
    check(capture.is_file() and capture.read_bytes()[:4] == b"\x89PNG", "and renders its project")


def main():
    with tempfile.TemporaryDirectory(prefix="pocket-package-") as directory:
        root = Path(directory)
        check_layouts(root)
        if len(sys.argv) > 1:
            check_packaged_runtime(root, sys.argv[1])
    print("package_test: ok")


if __name__ == "__main__":
    main()
