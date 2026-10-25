# Explicit script reload protocol

This slice reloads selected registered Lua component types without resetting the
world or advancing a simulation tick. It is an explicit local development action,
not a file watcher, whole-VM checkpoint, arbitrary module reloader, or general
rollback of Lua/host side effects. Runtime script execution remains trusted local
game code under the existing process supervisor and request budgets.

## Agent command and diagnostics

```
{"cmd":"reload_scripts","types":["ReloadProbe","ZDriver"]}
```

`types` must contain 1–64 distinct nonempty strings without NUL bytes. Missing,
wrongly typed, duplicate, empty or oversized lists return `invalid_argument`.
Unknown names and built-in native types fail the core validation phase. Resources
are resolved from the existing registration's exact known source, rather than
accepting arbitrary file paths in the protocol.

On success the response contains the ordinary complete observation and an extra
result object:

```
{"ok":true,"protocol":1,"frame":7,"scene":"reload","running":true,
 "actors":[],"reload":{"types":["ReloadProbe"],"instances_updated":2,
 "diagnostics":[]}}
```

The abbreviated actors list above is illustrative; the actual response includes
all ordinary observation fields and live actors. `instances_updated` reports the
selected live instances. The supervisor validates both the observation and the
reload-result shape, so this command retains the same worker correlation and
JSONL framing protections as other commands.

An ordinary failed reload returns `ok:false`, `code:"script_reload_failed"`, an
`error` summary, the unchanged `frame`, `requires_reset:false`, selected `types`,
and `diagnostics`. Each diagnostic has `type`, `resource`, `stage`, `message` and
integer `line`; zero means no line was available. Diagnostic text fields and the
summary are bounded to 4096 valid UTF-8 bytes so malformed or very large Lua
errors cannot invalidate or overwhelm the protocol. Stages distinguish compilation,
evaluation, validation and busy-state rejection. A failed candidate does not add
a live runtime-error poison that prevents the next ordinary step or reload retry.
Existing supervisor/protocol failures and live-VM resource failures retain their
separate reset-required behavior. An unexpected exception after commit starts is
not an ordinary rejection: the agent returns `script_reload_host_failure` with
`requires_reset:true` and terminates that worker. The editor persists a
`host_reset_required` diagnostic and explicitly asks for reset/reopen without
claiming rollback. This fatal classification is not evidence of successful
rollback under allocator failure; destructive failure injection is not claimed.

## State and staging contract

Reload runs on the runtime owner thread while callbacks are idle. All selected
sources are prepared before any selected prototype is committed; a failure in a
later source cannot leave an earlier one updated. Evaluation order is sorted
source order. A batch can refer to already evaluated selected exports or local
helpers. Unselected live prototypes are not imported into staging. Prototype
inheritance through executable metatables is unsupported, including inheritance
from another type selected in the same batch.

Candidate top-level code runs in a separate Lua VM with a 32 MiB heap bound and
one million instruction bound. Only the supported pure standard-library surface
is available there. Host mutation APIs, I/O, debug facilities, external `require`,
random APIs, `pcall`/`xpcall` and pattern-matching functions are unavailable at
top level. This prevents a candidate from
mutating live actors, native inventories or save files and then throwing an
ordinary evaluation error. Candidate globals stay in staging. Runtime functions
are transferred with their environment rebound to the live host; host calls must
therefore occur inside runtime callbacks/methods. Candidate Lua functions and
their shared upvalue graphs can transfer to the live VM. Old module-local mutable
state is not migrated: module locals restart from the candidate evaluation.
Self-contained compiled bundles must keep their required helper modules inside
the bundle. These are instruction/heap bounds, not an editor hard wall-clock
deadline; the editor invokes the operation synchronously.

Successful reload preserves existing live component tables, raw scalar/nested
state, actor identity, physics bodies and native inventory userdata. New missing
defaults are cloned separately for each instance. Existing data is not reset to
new defaults. Aliases to retained default roots and nested tables are preserved.
Known table-valued keys can use this identity mapping; relationships whose key
identity cannot be inferred are rejected rather than assigned a new identity.
Prototype methods are replaced, removed methods cease scheduling,
and lifecycle flags are rebuilt. Reload does not call `OnStart` or `OnDestroy`
again. A subsequently created actor uses the new prototype and receives its
ordinary first `OnStart`.

Named prototype event callbacks are remapped to the replacement method; removing
a method unsubscribes it. Anonymous event callbacks and Lua functions cached
inside mutable instance state cannot be safely migrated and cause explicit
validation rejection. This differs from transferring new candidate functions
and their upvalues: arbitrary old closure/module-state migration, executable
prototype metatables and general state-schema migration remain outside this slice. Repair or
remove unsupported live state using the existing game behavior, or deliberately
reset to obtain a fresh VM. Reset loads the current files from disk and does not
preserve the old runtime state.

## End-to-end verification

`tests/agent/agent_reload_test.py RUNTIME` creates a complete temporary source project
and edits the actual registered files between JSONL requests. It tests two live
instances with changed health, nested data, consumed native inventory and a real
moving Rigidbody. It checks frame/pose preservation at reload, retained userdata
identity and subsequent operations, isolated new table defaults, new actor
initialization, method replacement, lifecycle removal/re-addition, named event
replacement/removal and start counts.

Failure cases include malformed command arguments, unknown/native types, syntax
errors, evaluation errors, missing exports, forbidden top-level host actions,
executable metatables, an atomic batch with a later invalid source, staging
infinite loops, staging allocation exhaustion, 20 MiB error strings and malformed
UTF-8 diagnostics. The old methods must remain
usable after every rejected candidate; fixing the file must work without reset.
Cached live methods and anonymous event subscriptions are rejected explicitly.
Finally, reset proves new disk defaults and clean subscriptions in a new worker.
The optional TypeScript replay also reloads its compiled controller bundle in a
live process and compares subsequent movement, animation and save/load helper
behavior with the handwritten Lua reference.

The test generates its own catalog and Lua files under a temporary directory;
no production fixture or user script is modified. Compiler/runtime diagnostics,
not just static API existence, are part of acceptance. The editor integration has its own real-render harness described below; human
feedback on reload discoverability remains separate from automated integrity.

## Editor action

Tools → Reload Lua Scripts opens an explicit tool window. It lists registered
Lua types with their complete registered source paths; built-in types are omitted.
Select one or more types and click Reload Selected Scripts. This calls the same
core operation as the agent command on the owner thread after game callbacks
finish. Project Refresh and external-editor double-click behavior are unchanged.

The last result and its per-source diagnostics remain in the window until another
reload or project change. The action does not mark a scene dirty, save it, rebuild
its runtime mirror, discard a map draft, enter/exit play, or advance a paused
simulation. Invalid candidates remain editable on disk and can be retried.

`tests/editor/script_reload_editor_test.cpp` uses a temporary copy of the tilemap farm,
a real SDL software renderer and ImGui. It clicks the actual reload button,
checks live component identity/start counts, preserves unsaved scene/map drafts,
renders persistent failure diagnostics in a paused context, retries through the
same button and checks the next gameplay update uses the repaired method.
The public `ExecuteScriptReload` handler is shared with the button; the harness
does not reproduce the core reload logic.

## Scene source lifetime

Scene transitions reuse previously loaded canonical source paths, including their
module-local state. An explicit successful reload updates both the active registry
and its source-cache entry, so switching away and back cannot resurrect the old
prototype. A canonical Lua source retains its component-type ownership for the
VM lifetime; a new filename alias cannot reuse it as another type. File edits
alone do not trigger reevaluation. New source variants are
prepared separately; see [scene transitions](scene-transitions.md).

If a registered canonical path is replaced by a symlink to another source,
explicit reload rejects it before publication. Restore the registered path, or
select a new source through scene preparation; reload does not silently rename
a source-cache entry. This is identity validation, not an adversarial filesystem
race guarantee.
