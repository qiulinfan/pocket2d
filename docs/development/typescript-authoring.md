# Optional TypeScript authoring frontend

Pocket2D still executes Lua 5.4.9. This optional frontend compiles TypeScript into
that runtime and reuses its native actors, physics, animation, input and saves.
It is not a JS VM, Node/DOM environment, or a promise that arbitrary npm packages
and ECMAScript behavior work in Lua.

## Reproducible tools and declarations

Node.js 20+ is required. The local development package pins TypeScript 6.0.2 and
TypeScriptToLua 1.37.1 with a lockfile. No global install or runtime npm dependency
is needed:

```sh
npm --prefix tools/script-authoring ci --ignore-scripts --no-audit --no-fund
python3 tools/generate_script_api.py --check
npm --prefix tools/script-authoring test
node tools/script-authoring/compile.mjs --check-toolchain
```

Include `api/types/pocket2d.d.ts` in the TS project. It is generated from the curated
host contract described in [script-api-contract.md](script-api-contract.md).
Static calls use `this:void`; native instance methods/callbacks have explicit
receiver types, so detached native-method calls fail static checking. Native nominal markers prevent plain objects from masquerading as
Vector2/Actor userdata. Those markers are type-only. Treat native objects as
opaque handles; the SDK does not promise ordinary JS reflection/spread semantics
for Lua userdata. Save.Read success data remains unknown until validated.

## Compile and deploy one component

```sh
node tools/script-authoring/compile.mjs \
  --project tests/script_authoring/tsconfig.json \
  --out-dir build/typed-gameplay-lua
```

The compiler emits one JSON response on stdout, with `ok`, diagnostics and emitted
file names. Diagnostics carry category/code/message and source file/line/column
when available. Standard compiler tracing is suppressed so it cannot corrupt that
response. Third-party compiler plugins remain trusted executable code, not a
sandboxed service.

The supplied project imports `SaveSchema.ts` and uses a single `TypedController`
bundle. Its required lualib code and helper module are embedded; deployment needs
only `TypedController.lua` plus `TSTL-LICENSE.txt` in the game's component_types
folder. The Lua file name must match the global component table the engine loads.
The example explicitly assigns `globalThis.TypedController` inside module code;
its emitted Lua registers `_G.TypedController`. Local helpers stay local with
`noImplicitGlobalVariables=true`. Each bundle has its own module cache; this does
not establish a shared module singleton across independently built bundles.

Without bundling, used lualib helpers are inline. With `luaBundle` configured,
`RequireMinimal` lets the bundler embed required helpers in the single file.
Unbundled multi-file helper deployment and arbitrary Lua package resolution are
not validated by the single-component example. No file watcher or automatic
live-instance reload is installed.

## Output ownership and failure

Output must be a dedicated generated directory. The tool refuses nonempty
unowned directories, including directories that contain only empty subfolders.
An ownership marker records generated files and toolchain versions, without
source checksums or user environment settings. Extra files/directories prevent
replacement. The tool checks ownership again immediately before promotion,
so an added manual file is not discarded merely because the directory was clean
when compilation started.

A per-output lock rejects concurrent writers. Do not remove a lock while its
compiler is running; after a terminated compiler, inspect that process before
removing a stale lock. This is a local cooperative workflow, not protection
against an adversary racing filesystem operations.

Compilation writes to a fresh sibling staging directory. TypeScript errors,
transpiler errors and unsupported output paths do not replace last good output.
Successful promotion moves the previous output to an owned backup and installs
the staged directory, restoring the backup if installation fails. The two rename
steps are not an atomic directory exchange or a power-loss transaction. Cleanup
failures are reported as warnings with retained paths; they do not falsely turn
an already promoted output into a failed compile.

Pinned TSTL 1.37.1 has a relevant upstream behavior: its noEmitOnError branch
checks all pre-emit diagnostics, including warnings, and bundling can subsequently
emit an empty module map. The wrapper therefore checks TypeScript errors first,
emits only into staging with that upstream flag disabled, then refuses promotion
on any transpiler error. Bundles use embedded minimal lualib rather than the
inline/bundle warning path. This workaround can be revisited only after an
upstream fix passes the same compilation and real-engine replay tests.

## Verification and current limits

```sh
cmake --preset ninja-debug -DPOCKET_ENABLE_TYPESCRIPT_TESTS=ON
cmake --build --preset ninja-debug
ctest --preset ninja-debug
```

The core suite has 70 tests and needs no npm installation. Explicitly enabling
the option requires the pinned local toolchain at configure time; missing or
mismatched dependencies are a configuration error. It adds compiler/output checks
and a freshly compiled real gameplay replay, for 72 tests total. CMake never
installs/downloads npm dependencies.

The 27-case compiler suite checks real scalar/nullable/native-object/readonly/unknown
member/unsafe-save-data errors with locations, unsupported transpilation, rollback,
output ownership, concurrent user additions, lock behavior and trace-free JSON.
The gameplay test compares 102 complete snapshots between handwritten Lua and
compiled TypeScript: movement, native animation events, health/death, input edges,
pause, exact save bytes, invalid-schema rejection, reset and new-process restore.
The test validates its independent game expectations as well as equality.
It additionally performs 31 exact comparisons through hot reloading the freshly
compiled TypedController bundle, preserving the current frame/state, then using
its transferred module helper in movement, animation and save/load. This is an
actual runtime check of the pinned bundle, not general npm/JavaScript reload support.

These checks establish this authoring slice, not every TypeScript feature or
production migration readiness. Runtime errors are still attributed to generated
Lua unless a separate source-map integration is verified. No claim is made about
AI repair-round improvement, a TypeScript speedup, complete JS compatibility,
full LuaLS server verification, or a finished Village_Rim migration.

The inventory extension uses the same typed host contract: nominal catalog and
container userdata, copied item/slot records, one-based native indices, explicit
optional nil arguments and group restoration. Its TypeScript probe is compiled
and its emitted calling conventions checked; the current real TypeScript game
replay remains the typed_gameplay workload. Inventory runtime operations are
verified by the native/Lua contract and inventory gameplay suites, not claimed as
a complete TypeScript inventory game.
