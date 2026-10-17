# Packaging games

`pocket package` turns a project into a game players run without the engine:
a macOS app, a Windows or Linux folder, or a web page.

```bash
python3 tools/build_runtime.py mac            # once per platform, and after engine changes
python3 tools/pocket package PROJECT --platform mac
```

The package and its archive land in `build/packages/PLATFORM/` (`--out`
changes that; `--no-archive` skips the archive).

## Runtime kits

A runtime kit is a self-contained Release runtime for one platform, laid out
by `cmake --install BUILD --prefix KIT --component runtime_kit`:

| Platform | Kit contents |
|---|---|
| mac | `game`, `lib/*.dylib` (found through `@executable_path/lib`), `.engine/system/` |
| linux | `game`, `lib/*.so` (found through `$ORIGIN/lib`), `.engine/system/` |
| windows | `game.exe`, the SDL DLLs and llvm-mingw's `libc++.dll` and `libunwind.dll` beside it, `.engine/system/` |
| web | `game.html`, `game.js`, `game.wasm`, `.engine/system/` |

`tools/build_runtime.py PLATFORM` builds a kit into `build/runtime/PLATFORM`
(the default `--runtime` of `pocket package`). It configures with
`-DPOCKET_DISTRIBUTION=ON`, which makes the runtime independent of libraries a
player may not have: SDL_ttf builds its own FreeType (fetched from a pinned,
hashed release by `cmake/FetchFreetypeIntoSdlTtf.cmake`, without HarfBuzz) and
SDL_image decodes with its built-in stb loaders instead of system libpng and
libwebp. `mac` builds on a macOS host (for its CPU: an Apple silicon host makes
an arm64 app). `linux` (x86_64, cross-compiled by GCC 14 in Ubuntu 24.04, so
players need glibc 2.39 or newer), `windows` (x86_64, cross-compiled with
llvm-mingw; Windows 10 or newer, for the Universal CRT) and `web` (Emscripten)
build in the Docker images under `tools/runtime-images/`, natively on the
host's CPU, with the repository mounted at `/src`. Docker must be able to
mount the repository (colima mounts the home folder by default).

## What a package holds

- The runtime, renamed after the game on Windows and Linux.
- `project/`: the project's files, without hidden files, `__pycache__`,
  `tools/`, `scenarios/`, `source_art/`, Markdown and the patterns listed in
  `game.config`'s `package_exclude`.
- `.engine/system/`: the engine's fallback font and icon.

The macOS app keeps the runtime and its libraries in `Contents/MacOS` and the
project and engine resources in `Contents/Resources`, has an `Info.plist`, an
icon made from `game.config`'s `package_icon` (default: the Pocket2D icon) and
an ad-hoc signature (Apple silicon requires one; it is not notarized, so
players open it the first time with right-click, Open). Archives: a zip for
macOS (made with `ditto`, which keeps the signature), Windows and the web, a
tar.gz for Linux (which keeps the executable bit).

## How the packaged runtime starts

With no `--project`, the runtime looks for `project/game.config` in SDL's base
path: the executable's folder on Windows and Linux, the app's
`Contents/Resources` on macOS, and `/` on the web. When it finds one it
changes to that folder (where `.engine/system` is) and starts the project, so
no launcher script is needed. Saves go to the usual per-user place
(`SDL_GetPrefPath`), not into the package. A `"splash_screen": true` project
shows the "Made with Pocket2D" splash first.

## The web runtime

The web build links SDL statically, runs tasks inline (no threads) and uses
ASYNCIFY so the game loop yields to the browser until its next animation
frame: it draws at the display's refresh rate while gameplay ticks at 60 Hz. Its page (`web/shell.html`) fetches `project.pack` before
`main`: a little-endian uint32 manifest length, the JSON manifest (`title`,
`files` with `path` and `size`) and the files' bytes in order, written under
`/` (`project/` and `.engine/system/`). Saves live in IndexedDB, mounted at
`/libsdl` and synced every five seconds. Serve the folder over HTTP (browsers
do not fetch `project.pack` from `file://`), for example with
`python3 -m http.server` in it.

## Tests

`tests/package_test.py` (CTest `package`) packages a fixture project for each
platform from stand-in kits and checks the layouts, the exclusions and the
web pack, then copies this build's runtime beside a project folder and checks
that it runs and renders it with no arguments.
