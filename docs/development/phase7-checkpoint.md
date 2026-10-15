# Phase 7: explicit transactional component-script reload

Local working-tree implementation; no commit or push. The overall Pocket2D goal
remains active. This is a verified reloadable-component contract, not arbitrary
Lua heap/continuation migration or SOTA certification.

## Delivered

- A separate candidate Lua state compiles/evaluates selected registered source
  files under a 32 MiB heap and one-million-instruction batch limit. Controlled
  pure-library proxies reject host effects, IO, RNG, debug, native pattern matching,
  forbidden capability capture, stdlib writes and unknown ambient globals.
  `require=nil` is the deliberate exception for the pinned self-contained TSTL
  bundle's local loader; external require cannot execute in staging.
- Validated graph transfer preserves candidate Lua functions, shared upvalue
  cells, recursive functions and table cycles/aliases. The candidate environment
  becomes the existing live environment for future callbacks. C-function/library
  captures are checked against known live identities. Graph size/depth and source
  bounds are explicit. Staging never commits live globals.
- ComponentManager::ReloadScripts resolves registered confined sources, prepares
  the entire selected batch, and commits at an idle owner-thread boundary. It
  retains instance tables, raw state and native userdata; adds missing defaults
  per instance; preserves self references and aliases into retained nested state;
  rejects ambiguous aliases/table-key correspondence. Pending actor activation
  ordering remains the same with and without reload.
- Lifecycle flags/lists and EventBus active/pending operations are prepared before
  publication. Named prototype handlers switch together, deleted handlers are
  removed, and other components remain untouched. OnStart/OnDestroy are not
  repeated. Instance function overrides, anonymous event closures and custom
  prototype behavior reject explicitly instead of silently keeping old code.
- Agent reload_scripts and the editor Tools → Reload Lua Scripts window invoke
  the same manager implementation. Success reports a same-frame observation.
  Ordinary rejected candidates keep the old worker usable; persistent editor
  diagnostics do not discard scene/map drafts or change Play/Pause state.
- Diagnostic text is limited to 4096 bytes, preserves valid UTF-8 and escapes
  invalid bytes/controls with safe truncation. Real large/non-UTF-8 Lua errors no
  longer overflow/corrupt the supervised response. Unexpected host exceptions are
  explicitly classified as requiring reset; post-commit rollback is not invented.

## Verified evidence

All **47 CTest cases pass in Debug, Release and ASan/UBSan**. The core has 45 cases;
two optional frontend cases require the pinned local Node dependencies. A final
source-removal/symlink-confinement assertion was added to the runtime test after
the full matrix and passed separately on all three builds; no runtime code changed.
macOS
LeakSanitizer remains unsupported, so sanitizer runs use detect_leaks=0.

New cases are script_reload_candidate, script_reload_runtime, agent_reload and
script_reload_editor; EventBus tests also cover prepared plans, pending FIFO,
deleted/mapped/ambiguous callbacks, nested busy rejection and unchanged failures.
The editor test clicks the actual ImGui button with SDL software rendering and
checks retained instance identity, start count, frame number and both dirty drafts.
Its persistent diagnostic screenshot was inspected locally.

The optional TypeScript test retains 102 baseline exact Lua-vs-TS comparisons and
adds **31 exact comparisons** through hot reloading the real freshly compiled
TypedController bundle and subsequent movement, animation and save/load helper
execution. This is runtime evidence for the pinned bundle, not all JS/npm code.

Runtime tests cover two live instances, native vectors/inventory/physics handles,
private shared module captures, new/removed callbacks, self/nested/table-key alias
identity, alias conflicts, pending actor activation, batch failures, repair/retry,
source confinement, staged instruction/heap failures and ordinary failures leaving
the worker alive. Large ASCII/Chinese and invalid-byte errors have protocol checks.

Build/test logs and captures are ignored local artifacts under build/phase7-* and
build/verification/script-reload-editor.png. Test source and stable fixtures remain
reviewable. No benchmark ranking or cross-platform result is claimed.

## Remaining boundaries and next work

The legacy startup/scene-preflight LoadComponentTypes path is still separate and
executes full-capability Lua in the live state. Its top-level effects and failed
scene-load side effects are not covered by the explicit reload transaction. This
is the next scripting correctness task, followed by maintained-binding evaluation
and source-aware diagnostics. Do not silently route old arbitrary scripts through
the restricted candidate VM without an explicit compatibility/migration contract.

Old module-local state, external cached closures, package.loaded, arbitrary state
schema upgrades and continuations are not migrated. Compatible callbacks must use
preserved instance data or deliberately reset. Main-VM allocator exhaustion is a
fatal/reset boundary. The editor has no hard wall-clock preemption or OS sandbox;
the supervised agent has its separate request deadline. Human editor usability
and broader platform packaging remain unverified gates.

See [ADR 003](adr-003-script-reload.md), [reload contract](script-reload.md),
[agent protocol](agent-protocol.md), and [status](status.md).
