# <img src="docs/icon.png" alt="Pocket2D icon" width="10%"> Pocket2D

Pocket2D is a cross-platform 2D runtime + editor game engine, written in C++26 on top of
SDL3, Lua, Box2D 3, Dear ImGui, and JSON scene assets. It hosts Lua for game logic scripting.
It is a variant of the [A2 Engine](https://a2engine.org/). For basic usage, check the documentation of A2 engine for the APIs.

Pocket2D Lua API documentation lives here:
- local source: [docs/lua-api/index.md](docs/lua-api/index.md)
- GitHub Pages: https://qiulinfan.github.io/pocket2d/

> Below is a demo of the editor and runtime in action, running the example
> "ball game" included in `.engine/templates/ballgame/`. This example game originates from the
> [A2 Engine](https://a2engine.org/) ecosystem.

![Pocket2D Demo](docs/show.gif)

The game has a Unity-like editor layout and runtime integration with:
- `SceneView` panel with actor-picking and drag&drop editing;
- embedded `Viewport` panel for pure runtime output;
- `Project` browser rooted at the currently opened project folder with scene opening and asset drag&drop support;
- `Hierarchy` tree with create, duplicate, delete, rename, actor reparenting;
- `Inspector` for component add/remove/rename and property editing
- actor parenting with local/world `Transform` and physics hierarchy inspection
- play-mode live editing and automatic scene-backed actor UID persistence
- `Sprite Editor` for creating and editing sprite assets.

## Build
### Linux Release
Install:
```bash
git clone https://github.com/qiulinfan/pocket2d.git
```

Build:
```bash
cd pocket2d
cmake --preset unix-makefiles-release
cmake --build --preset unix-makefiles-release -j4
```

Run the editor from the project root:
```bash
./pocket
```

Run the game executable:
```bash
./game
```

### Windows Release
Requires:
- Visual Studio 2022 with `Desktop development with C++`

Download the source from GitHub and extract it. Then open PowerShell in the extracted `pocket2d` folder.

Build:

```powershell
cmake --preset vs2022-x64
cmake --build --preset vs-release
```

Run the editor:

```powershell
.\pocket.exe
```

Run the game executable:

```powershell
.\game.exe
```

## Repository Layout and Architecture

- `src/app/runtime`, `src/app/editor`: executable hosts
- `src/engine/*`: runtime systems
- `src/editor/*`: editor shell, documents, panels
- `src/shared/*`: config, scene format, resource helpers
- `include/*`: public headers for the same layers
- `.engine/system/*`: editor fonts and icons
- `.engine/templates/*`: project templates, including the ball game sample
- `thirdparty/*`: vendored dependencies; SDL3 (with SDL_image, SDL_ttf, SDL_mixer) and Box2D 3 are fetched at configure time
- `scripting/api/*`: Lua host API contract, agent command schema and asset JSON Schemas
- `clients/python/*`, `tools/*`: the Python agent client and the `pocket` command line
- `tests/*`: CTest suites and fixture projects

## Projects
Projects can live anywhere on disk. Like Unity Hub, the editor keeps a per-user list of project paths in `projects.config`, next to `editor.config`, in `~/Library/Application Support/Pocket2D/` on macOS, `%APPDATA%\Pocket2D\` on Windows, and `$XDG_CONFIG_HOME/pocket2d/` (default `~/.config/pocket2d/`) on Linux. Nothing about your projects is stored in the engine folder. `File -> Open Project...` adds an existing project folder and `File -> Recent Projects` reopens one.

`File -> New Project...` asks for a template, a project name and a location, then copies the template into `<location>/<name>`. The location starts as your home folder and is remembered for the next project. Linux users type the location; Windows/macOS users can also pick it with the system folder picker. Templates are never edited in place:
- `Empty` creates `game.config`, `rendering.config`, `scenes/main.scene`, `actor_templates/empty.template`, and the standard project directories: `audio/`, `component_types/`, `fonts/`, `images/`, `scenes/`, and `actor_templates/`.
- `ballgame` copies the sample in `.engine/templates/ballgame/`.

When there is no project to reopen, as on first launch, the editor creates a `ballgame` project in the new-project location and opens it. The `game` runtime runs the project given by `--project`, else the editor's current project, else the sample template.

## Architecture Illustration

All files under `docs/architecture/` are diagram-only for explaining the architecture.
- [Frame Pipeline](docs/architecture/frame-pipeline.md)
- [Asset Pipeline](docs/architecture/asset-pipeline.md)
- [Module Dependency](docs/architecture/module-dependency.md)
- [Editor Workflow](docs/architecture/editor-workflow.md)

## Sample Project
`.engine/templates/ballgame/` contains the sample project, offered as the `ballgame` template.

## License

Pocket2D is released under the [MIT License](LICENSE).

Third-party libraries under `thirdparty/` keep their own upstream licenses.
