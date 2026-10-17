"""Packages a Pocket2D project as a game players can run: a macOS app, a
Windows or Linux folder, or a web page, from a runtime kit for that platform
(tools/build_runtime.py builds one; `cmake --install BUILD --component
runtime_kit` lays it out).

The packaged runtime finds the project as "project" beside it (in the app's
Resources on macOS, preloaded at /project on the web) and starts it directly.
Development files stay out: hidden files, __pycache__, tools/, scenarios/,
source_art/, Markdown, and the patterns game.config lists in
"package_exclude" (paths relative to the project, with * wildcards).
"""
import fnmatch
import json
import os
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path

PLATFORMS = ("mac", "windows", "linux", "web")
DEFAULT_EXCLUDE = (".*", "__pycache__", "*.pyc", "tools", "scenarios", "source_art", "*.md")


class PackageError(Exception):
    pass


def slug(title):
    return re.sub(r"[^A-Za-z0-9_-]+", "", title.replace(" ", "")) or "Game"


def project_files(project, extra_exclude=()):
    """The project's files to ship, as (relative POSIX path, absolute path), sorted."""
    patterns = list(DEFAULT_EXCLUDE) + list(extra_exclude)
    files = []
    for root, directories, names in os.walk(project):
        relative_root = Path(root).relative_to(project)

        def excluded(name):
            path = (relative_root / name).as_posix()
            return any(fnmatch.fnmatch(name, p) or fnmatch.fnmatch(path, p) for p in patterns)
        directories[:] = sorted(d for d in directories if not excluded(d))
        for name in sorted(names):
            if not excluded(name):
                files.append(((relative_root / name).as_posix(), Path(root) / name))
    return files


def copy_tree_resolved(source, destination):
    """Copies a folder, turning symlinks into the files they point to (library soname links)."""
    shutil.copytree(source, destination, symlinks=False, dirs_exist_ok=True)


def copy_project(project, files, destination):
    for relative, path in files:
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)


def require(kit, *names):
    for name in names:
        if not (kit / name).exists():
            raise PackageError("runtime kit %s has no %s (build it with tools/build_runtime.py)" % (kit, name))


def mac_icon(source_png, resources):
    """An .icns from a PNG (macOS hosts: sips and iconutil); returns the icon file name or None."""
    if not shutil.which("iconutil") or not shutil.which("sips"):
        return None
    with tempfile.TemporaryDirectory() as temp:
        iconset = Path(temp) / "icon.iconset"
        iconset.mkdir()
        try:
            for size in (16, 32, 64, 128, 256, 512):
                for scale in (1, 2):
                    name = "icon_%dx%d%s.png" % (size, size, "@2x" if scale == 2 else "")
                    subprocess.run(["sips", "-z", str(size * scale), str(size * scale), str(source_png), "--out",
                                    str(iconset / name)], check=True, capture_output=True)
            subprocess.run(["iconutil", "-c", "icns", str(iconset), "-o", str(resources / "icon.icns")],
                           check=True, capture_output=True)
        except subprocess.CalledProcessError:
            print("pocket package: %s is not a usable icon; the app keeps the default" % source_png, file=sys.stderr)
            return None
    return "icon.icns"


def package_mac(kit, project, files, config, stage):
    require(kit, "game", "lib", ".engine/system")
    title = config.get("game_title") or project.name
    app = stage / (title + ".app")
    contents = app / "Contents"
    macos, resources = contents / "MacOS", contents / "Resources"
    macos.mkdir(parents=True)
    resources.mkdir()
    shutil.copy2(kit / "game", macos / "game")
    copy_tree_resolved(kit / "lib", macos / "lib")
    copy_tree_resolved(kit / ".engine", resources / ".engine")
    copy_project(project, files, resources / "project")
    icon_png = project / config["package_icon"] if config.get("package_icon") else kit / ".engine/system/icon.png"
    icon = mac_icon(icon_png, resources) if icon_png.exists() else None
    info = {"CFBundleName": title, "CFBundleDisplayName": title, "CFBundleExecutable": "game",
            "CFBundleIdentifier": "org.pocket2d.game." + slug(title).lower(), "CFBundlePackageType": "APPL",
            "CFBundleShortVersionString": str(config.get("version", "1.0")), "CFBundleVersion": "1",
            "LSMinimumSystemVersion": "11.0", "NSHighResolutionCapable": True}
    if icon:
        info["CFBundleIconFile"] = icon
    (contents / "Info.plist").write_bytes(plistlib.dumps(info))
    if shutil.which("codesign"):   # an ad-hoc signature, which Apple silicon requires to run
        signed = subprocess.run(["codesign", "--force", "--deep", "--sign", "-", str(app)], capture_output=True)
        if signed.returncode != 0:
            print("pocket package: could not sign %s: %s" % (app.name, signed.stderr.decode(errors="replace").strip()),
                  file=sys.stderr)
    return app


def package_windows(kit, project, files, config, stage):
    require(kit, "game.exe", ".engine/system")
    title = config.get("game_title") or project.name
    folder = stage / slug(title)
    folder.mkdir(parents=True)
    shutil.copy2(kit / "game.exe", folder / (slug(title) + ".exe"))
    for dll in sorted(kit.glob("*.dll")):
        shutil.copy2(dll, folder / dll.name)
    copy_tree_resolved(kit / ".engine", folder / ".engine")
    copy_project(project, files, folder / "project")
    return folder


def package_linux(kit, project, files, config, stage):
    require(kit, "game", "lib", ".engine/system")
    title = config.get("game_title") or project.name
    folder = stage / slug(title)
    folder.mkdir(parents=True)
    shutil.copy2(kit / "game", folder / slug(title))
    copy_tree_resolved(kit / "lib", folder / "lib")
    copy_tree_resolved(kit / ".engine", folder / ".engine")
    copy_project(project, files, folder / "project")
    return folder


def write_pack(path, title, entries):
    """project.pack for the web shell: uint32 manifest length, JSON manifest, file bytes."""
    manifest = {"title": title, "files": [{"path": p, "size": len(b)} for p, b in entries]}
    head = json.dumps(manifest, separators=(",", ":")).encode()
    with open(path, "wb") as out:
        out.write(struct.pack("<I", len(head)))
        out.write(head)
        for _, data in entries:
            out.write(data)


def package_web(kit, project, files, config, stage):
    require(kit, "game.html", "game.js", "game.wasm", ".engine/system")
    title = config.get("game_title") or project.name
    folder = stage / (slug(title) + "-web")
    folder.mkdir(parents=True)
    html = (kit / "game.html").read_text(encoding="utf-8").replace("<title>Pocket2D</title>",
                                                                    "<title>%s</title>" % title)
    (folder / "index.html").write_text(html, encoding="utf-8")
    shutil.copy2(kit / "game.js", folder / "game.js")
    shutil.copy2(kit / "game.wasm", folder / "game.wasm")
    entries = [("project/" + relative, path.read_bytes()) for relative, path in files]
    for path in sorted((kit / ".engine").rglob("*")):
        if path.is_file():
            entries.append((path.relative_to(kit).as_posix(), path.read_bytes()))
    write_pack(folder / "project.pack", title, entries)
    return folder


def archive(folder, platform):
    """A zip (a tar.gz on Linux, which keeps the executable bit) beside the folder."""
    if platform == "linux":
        target = folder.with_name(folder.name + "-linux.tar.gz")
        with tarfile.open(target, "w:gz") as tar:
            tar.add(folder, arcname=folder.name)
        return target
    target = folder.with_name(folder.stem + "-%s.zip" % platform) if platform != "web" else folder.with_suffix(".zip")
    if platform == "mac" and shutil.which("ditto"):   # keeps the bundle's signature and permissions
        subprocess.run(["ditto", "-c", "-k", "--keepParent", str(folder), str(target)], check=True)
        return target
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as out:
        for path in sorted(folder.rglob("*")):
            info = zipfile.ZipInfo.from_file(path, path.relative_to(folder.parent).as_posix())
            if path.is_file():
                info.compress_type = zipfile.ZIP_DEFLATED
                out.writestr(info, path.read_bytes())
            else:
                out.writestr(info, b"")
    return target


def package(project, platform, kit, out, make_archive=True):
    """Builds the package; returns (folder or app, archive or None)."""
    if platform not in PLATFORMS:
        raise PackageError("platform must be one of " + ", ".join(PLATFORMS))
    project, kit, out = Path(project).resolve(), Path(kit).resolve(), Path(out).resolve()
    config_path = project / "game.config"
    if not config_path.is_file():
        raise PackageError("%s has no game.config" % project)
    config = json.loads(config_path.read_text(encoding="utf-8"))
    files = project_files(project, config.get("package_exclude", []))
    stage = out / platform
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    builder = {"mac": package_mac, "windows": package_windows, "linux": package_linux, "web": package_web}[platform]
    result = builder(kit, project, files, config, stage)
    return result, (archive(result, platform) if make_archive else None)
