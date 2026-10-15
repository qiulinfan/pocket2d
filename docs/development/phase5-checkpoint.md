# Phase 5 checkpoint

2026-10-15. A curated typed host contract and optional TypeScript authoring
frontend are implemented. Pocket2D still executes the existing Lua 5.4.9 VM;
no JavaScript VM or global tool installation was introduced.

`scripting/api/host-api.json` generates TypeScript declarations, LuaLS metadata
and embedded JSON exposed by agent describe.script_api. Generation checks reject
stale output. Native conformance tests verify the advertised binding surface,
representative real calls, readonly/native-value boundaries and nullable results.
Explicit instance this types also reject detached native-method calls at compile time.
Coverage is intentionally labelled curated; compile-only nominal markers and
engine-owned authoring fields are not misrepresented as extra runtime members.

The pinned local compiler is TypeScript 6.0.2 / TypeScriptToLua 1.37.1, installed
and reproduced with npm ci --ignore-scripts. The frontend emits structured source
diagnostics and promotes only good staged output. It rejects unowned directories,
unmanaged entries and concurrent writers, rechecks ownership before promotion,
restores previous output on installation failure, and reports cleanup warnings.
Tests cover 19 type/transpiler/output cases, including real compiler locations,
late user files, empty user subdirectories, rollback and trace-free JSON.

A real controller and SaveSchema helper module bundle into a single Lua component
with required lualib embedded and license retained. Handwritten Lua and freshly
compiled TypeScript pass 102 complete runtime snapshot comparisons: real movement,
animation events, health/death bounds, key edges, pause, identical save bytes,
invalid game-schema refusal, reset and new-process recovery. A source-visible
fixture copy plus the compiled bundle also launches and captures graphically.

With POCKET_ENABLE_TYPESCRIPT_TESTS=ON, Debug, Release and ASan/UBSan each pass all
38 tests. The ordinary core suite has 36; the extra two require the pinned local
Node toolchain and fail configuration when it is missing. No network dependency
installation happens in CMake. LeakSanitizer remains unsupported on this Mac.

This is a verified authoring slice, not full JS/npm compatibility, cross-bundle
module singleton semantics, verified runtime TS source maps or measured AI repair
advantages. Full API coverage, live-instance reload, maintained Lua/binding
experiments, content migration, platforms and SOTA comparisons remain open.
Use typescript-authoring.md, script-api-contract.md and status.md to resume.
All work is local/uncommitted; compiled Lua, diagnostics and captures remain ignored.
