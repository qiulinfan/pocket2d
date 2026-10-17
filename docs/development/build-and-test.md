# Building and testing the modernized runtime

Requires a compiler with C++26 mode, CMake 3.30 or newer, and a native build tool.
The verified local toolchain is Apple Clang 21 and CMake 4.4. Unsupported C++26
compilers should fail configuration rather than silently lowering the standard.

SDL 3.4.18, SDL_image 3.4.8, SDL_ttf 3.2.2 and SDL_mixer 3.2.4 are fetched from
pinned official release tags. Development builds discover FreeType, HarfBuzz
and PNG development libraries on the host; `-DPOCKET_DISTRIBUTION=ON` (used for
runtime kits, see [packaging.md](packaging.md)) builds FreeType from a pinned
release and decodes images with SDL_image's stb loaders instead. Box2D 3.1.1 is
fetched from its release tag and linked statically; its CMake compiles it with
`-ffp-contract=off` for deterministic math. CMake's normal
`FETCHCONTENT_SOURCE_DIR_SDL3`, `_SDL3_IMAGE`, `_SDL3_TTF`, `_SDL3_MIXER` and
`_BOX2D` overrides can use existing offline checkouts of those releases.

```sh
cmake -S . -B build/modern-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/modern-debug --parallel
ctest --test-dir build/modern-debug --output-on-failure
```

Run the graphical editor or runtime from the repository root:

```sh
./build/modern-debug/pocket
./build/modern-debug/game --project .engine/templates/ballgame
```

`POCKET_RENDER_DRIVER=metal`, `opengl`, `gpu` or `software` selects the
native SDL3 renderer; `gpu` is SDL 3.4's renderer on SDL_GPU (Metal on macOS,
Vulkan or Direct3D 12 elsewhere). Empty/`auto` lets SDL choose. An unavailable explicit driver is an
error; it does not silently substitute another backend. Available backends are
platform-dependent. `POCKET_RENDER_VSYNC=1` enables synchronization (and
overrides `game.config`'s `vsync`). Gameplay always ticks at 60 Hz: the
standalone `game` draws as many frames as it can between ticks unless
`game.config` sets `max_fps` or `vsync`, while the editor's play mode and
`--frames` runs draw one frame per tick at 60 Hz.

```sh
./build/modern-debug/tests/render_backend_test software
./build/modern-debug/tests/render_backend_test metal
./build/modern-debug/tests/render_backend_test opengl
./build/modern-debug/tests/render_backend_test gpu
```

`POCKET_ENABLE_GPU_RENDER_TESTS` (default on for Apple platforms) adds CTest
runs of the backend, inventory and two-world pixel tests on the `gpu`
renderer; enable it elsewhere when a GPU driver is available.

Optional backend tests return 77 only when backend initialization is unavailable;
a rendering mismatch is a failure. The final optional argument writes a PNG of
the generated test target, never the desktop.

Engine diagnostics go to stderr as `level [category] message`;
`POCKET_LOG_FORMAT=jsonl` writes one JSON object per line with level, category,
World id, frame and message. Each World also keeps its last 256 records, which
agent observations report.

Configure with `-DPOCKET_ENABLE_TRACY=ON` to compile Tracy 0.14.1 zones into
the engine and Box2D (`BOX2D_PROFILE`); Tracy runs on demand, so the build works
without a connected profiler. Zones cover the gameplay step, Lua lifecycle,
physics step and event dispatch, animation, tilemap commit, transform
resolution, queueing and rendering; a frame mark ends each gameplay frame.

Use Release for measurements:

```sh
cmake -S . -B build/modern-release -DCMAKE_BUILD_TYPE=Release
cmake --build build/modern-release --parallel
./build/modern-release/tests/hierarchy_benchmark
./build/modern-release/tests/particle_benchmark
```

Benchmark output belongs under ignored `build/`. Report workload counts,
compiler/build mode and repeated timing distributions. Do not turn machine
speed thresholds into correctness tests.

Dear ImGui core and SDL backends come from upstream tag `v1.93.0-docking`,
unmodified, with its license and stock `imconfig.h`. The historical SDL2 vendor
packages have been removed. `pocket package` produces macOS, Windows, Linux
and web builds ([packaging.md](packaging.md)); a package that starts is not
proof of cross-platform readiness.

A bounded graphical smoke can save the engine's rendered target before
presentation and exit without leaving a window open:

```sh
POCKET_RENDER_DRIVER=metal ./build/modern-debug/game \
  --project tests/fixtures/village_defense --frames 2 \
  --capture build/village-defense.png
```

The standard `ninja-debug` / `ninja-release` and `unix-makefiles-*` configure,
build and test presets are registered. `make test` runs the debug CTest suite.
For memory validation, an independent Debug build with
`-fsanitize=address,undefined -fno-omit-frame-pointer` in both C and C++ flags
exercises runtime and protocol tests without changing normal build settings.

On the verified local macOS toolchain, LeakSanitizer is unavailable; run that
build with `ASAN_OPTIONS=detect_leaks=0`. This disables leak detection only, not
AddressSanitizer or UndefinedBehaviorSanitizer. See `status.md` for the latest
completed build matrix rather than assuming an old result covers new code.

The native Tilemap fixture uses the same runtime and input protocol:

```sh
./build/modern-debug/game --project tests/fixtures/tilemap_farm
./build/modern-debug/game --headless --agent --project tests/fixtures/tilemap_farm
./build/modern-debug/tests/tilemap_render_test software
./build/modern-debug/tests/tilemap_render_test metal
./build/modern-debug/tests/tilemap_render_test opengl
```

Tilemap render tests verify actual readback, sorting, transparency, DPI/zoom and
atlas selection. `tilemap_overlay` exercises the real editor overlay with an
offscreen software renderer; it does not certify human OS-window interactions.
The fixed-camera/scroll/edit benchmark is `tests/tilemap_benchmark`; its final
image must match the SpriteRenderer baseline before timing can be used. Logs,
CSV output and capture files remain under ignored `build/`.

Versioned gameplay persistence has a separate playable fixture and explicit
storage root. Do not point it at the source project:

```sh
./build/modern-debug/game --project tests/fixtures/persistent_village \
  --save-dir build/local-game-saves
```

F5 saves and F9 loads. The fixture's configured namespace places its slot at
`build/local-game-saves/persistent_village/main.save.json`. Agent mode accepts
the same `--save-dir` option; workers inherit it across reset. Without the
override, configured games use SDL's platform preference directory. See
`tests/fixtures/persistent_village/CONTRACT.md` and `docs/lua-api/savegame.md` for
controls, payload scope and expected failures. Tests always use temporary roots.

`--scene NAME` starts a project in another scene than game.config's
`initial_scene` (a lab or test scene), in graphical and agent mode; agent workers
inherit it across reset, and a missing scene is a startup error.

The optional TypeScript frontend has pinned local development dependencies:

```sh
npm --prefix tools/script-authoring ci --ignore-scripts --no-audit --no-fund
cmake -S . -B build/modern-debug -DPOCKET_ENABLE_TYPESCRIPT_TESTS=ON
cmake --build build/modern-debug --parallel
ctest --test-dir build/modern-debug --output-on-failure
```

The option adds two real authoring tests to the 70-test core suite, with explicit
configuration failure if dependencies are missing. The core runtime does not
depend on Node. Details and generated-output ownership rules are in
`typescript-authoring.md`.

## Inventory slice

The core suite includes item_catalog, inventory_model, inventory_lua,
inventory_render_software and agent_inventory. The latter runs real supervised
keyboard/mouse gameplay against tests/fixtures/inventory_village. To repeat pixel
checks for an available local backend:

```sh
build/sdl3-debug/tests/inventory_render_test tests/fixtures/inventory_village metal
build/sdl3-debug/tests/inventory_render_test tests/fixtures/inventory_village opengl
```

The render test rejects a silently substituted backend. The optional third
argument after the backend is an output PNG path; keep captures under build/.
Controls and manual acceptance are in inventory-playtest.md. The runtime API
contract is ../lua-api/inventory.md. This fixture uses source-visible geometric
PNGs and needs no private editor metadata or Unity assets.

## Script reload slice

The core suite also includes script_reload_candidate, script_reload_runtime,
agent_reload and script_reload_editor. The editor test drives the actual ImGui
reload button with the software renderer and retains scene/map drafts. For an
ignored diagnostic screenshot:

```sh
build/sdl3-debug/tests/script_reload_editor_test tests/fixtures/tilemap_farm build/reload-editor.png
```

The optional compiled gameplay replay retains its 102 baseline comparisons and
adds 31 comparisons through a real compiled-bundle reload and subsequent gameplay.

## Editor command transactions

The core regressions include scene_edit_runtime, scene_document_transaction,
editor_command_transaction, prepared_native_adoption, editor_edit_status and
component_key_rename. They exercise complete command rejection, runtime/native
identity preservation, pending draft repair, real ImGui Retry/Dismiss clicks and
active/queued event subscriptions through component renames. See
`editor-command-transactions.md` and ADR 006 for the failure boundaries.

A manual Release hierarchy workload is built but is not a timing-gated test:

```sh
build/sdl3-release/tests/scene_edit_benchmark
```

It reports rename/reparent timings for flat and chain-shaped 100/1000-actor
scenes after three warmups and seven measured operations. Run it without parallel
build/test workloads. Results do not establish a comparison with other engines.
The editor status regression optionally writes a local screenshot:

```sh
build/sdl3-debug/tests/editor_edit_status_test "$PWD/build/editor-edit-status.png"
```

## Isolated binding comparison

`tools/binding-experiment` is a separate C++26 CMake project, not part of the
production CTest count. It compiles actual Lua/ScriptBudget sources with separate
LuaBridge 2.7 and pinned LuaBridge3 headers. Its four semantic/budget tests, manual
benchmark and real-production-TU compile inventory are described in
`binding-experiment-results.md`. No production dependency is switched by running
these tools. Candidate acquisition is explicit; logs and measurements stay in build/.

## Checked Lua binding

Production uses pinned LuaBridge3 rc12 with the documented same-state fast path
in thirdparty/LuaBridge/UPSTREAM.md. Lua remains 5.4.9. `lua_boundary` exercises
production checked conversions, result-mode policy, discard-return callbacks,
nonzero-stack preservation, bounded source/traceback diagnostics and real enabled
lifecycle/event behavior. `lua_vm` also checks same-thread, coroutine-shared and
independent-VM identities. Core CTest count is 70; the two optional TypeScript
checks bring it to 72. `lua_receiver` and `agent_receiver` cover invalid instance
receivers, legal nullable arguments and real-worker continuation after caught and
uncaught script errors. See phase14-checkpoint.md and native-call-receivers.md for
acceptance and scope, and native-receiver-performance.md for matched costs.
Earlier binding migration measurements remain in binding-runtime-performance.md.

## Integrated village and body activation

`rigidbody_activation` and `tilemap_activation` verify deferred startup, live
physics participation, native identity and cross-owner contact mutation guards.
`agent_two_world_village` runs farming, inventory, combat, portals and checkpoint
recovery through the supervisor. `two_world_render_software` checks the same
fixture through real input and pixel readback; its executable also accepts
`metal` or `opengl` after the fixture path for available local hardware checks.
See phase15-checkpoint.md and the fixture CONTRACT for scope and final evidence.

`POCKET_ENABLE_IMGUI_TEST_ENGINE` (default on) compiles Dear ImGui Test Engine
hooks into ImGui and builds `editor_ui_test`, which clicks real editor
controls. The engine's license is in `thirdparty/imgui_test_engine/LICENSE.txt`.
`editor_ui_tilemap` runs it with `--tilemap`: Scene-view drags paint a
1000x400 map's extra layer and autotile terrain on the offscreen software
renderer. A third argument writes a PNG of the final strokes from a real window
on the native renderer instead:

```sh
POCKET_WINDOW_DISPLAY=EDB323QU POCKET_WINDOW_BACKGROUND=1 ./build/sdl3-release/tests/editor_ui_test --tilemap tests/fixtures/tilemap_farm build/captures/map-paint.png
```

`POCKET_WINDOW_DISPLAY` places new windows on a display chosen by 0-based index
or by part of its name (for example a monitor model such as `EDB323QU`);
`POCKET_WINDOW_BACKGROUND=1` shows windows without activating the app, so
automated graphical runs do not take keyboard focus:

```sh
POCKET_WINDOW_DISPLAY=EDB323QU POCKET_WINDOW_BACKGROUND=1 ./build/sdl3-release/game --project tests/fixtures/village_defense --frames 120
```

It is for automation only: it makes the unbundled `game` executable a macOS
background app that never becomes active, so a person who clicks its window
still cannot type into it. Leave it out whenever someone plays.
