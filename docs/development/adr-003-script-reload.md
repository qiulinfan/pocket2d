# ADR 003: isolated candidates and an idle-boundary script reload transaction

## Status

Accepted for implementation under the owner's autonomous Pocket2D development
and Lua-embedding improvement direction. Verification is recorded separately in
the phase checkpoint. This does not certify arbitrary Lua hot swapping.

## Date

2026-10-15.

## Engine Compatibility

Pocket2D C++26, pinned Lua 5.4.9, existing LuaBridge 2.7 and owner-thread SDL3/Box2D.
The relevant sources are thirdparty/Lua/lua.h, lapi.c, ldo.c and lgc.c, together
with the [Lua 5.4 manual](https://www.lua.org/manual/5.4/manual.html), including
lua_dump, lua_load, lua_upvalueid and lua_upvaluejoin. Function bytecode alone
cannot preserve captured data or shared upvalue cells; these are transferred as a
validated graph. No downloaded bytecode or cross-version bytecode format is an
asset contract: candidates are generated and consumed by the same pinned runtime.

The Village_Rim ADR skill supplies the decision/review structure. Its Unity
reference registry and guided approval workflow are inapplicable to this standalone
engine and superseded by the owner's explicit autonomous planning instruction.
Existing Pocket2D ADRs and authoritative source are the architecture context.

## ADR Dependencies

Depends on ADR 001's shared deterministic owner-thread simulation and ADR 002's
native lifetime rules. Preserves the ScriptBudget supervisor reset boundary,
per-instance defaults and EventBus ordering already covered by regression tests.
Enables live agent/editor iteration; does not require a second gameplay VM.

## Context and Requirements

The legacy ReloadComponentTypes clears its registry then executes files in the
live global environment. Failed evaluation can mutate globals or host state, old
instances retain old prototype tables, lifecycle caches remain stale, and event
subscriptions retain old function identities. This cannot be advertised as a
transactional live-code replacement.

An explicit reload must preserve the frame, actor/native identities, current
instance data, pending start state and unaffected subscribers. Syntax, evaluation
and validation rejection must keep the running version usable. It must use the
same implementation from agent commands and the editor, with durable diagnostics.

## Decision

1. Resolve requested registered Lua types to their known project source paths;
   validate bounds, duplicates and confinement. Read bounded source text.
2. Compile and evaluate the whole candidate batch in a separate bounded Lua state
   exposing only a controlled pure-library environment. No host bindings, IO,
   package loading, debug access or shared live RNG are exposed at top level.
   Reject unsupported exports and stdlib mutation. Source order is deterministic.
   Unknown ambient global reads fail; the explicit require=nil exception supports
   the pinned TSTL bundle's local loader without enabling external imports.
   Native string-pattern functions are unavailable during staging because a Lua
   instruction hook cannot preempt their C execution.
3. Validate and transfer the candidate graph into the existing gameplay VM.
   Preserve table cycles/aliases, Lua functions and shared upvalue cells. Map the
   candidate global environment to the existing live environment for future
   runtime calls. Validate C-function mappings; reject userdata, continuations,
   unknown C closures and finalizer/weak-table behavior in candidate data.
4. Prepare all instance default additions, metatables, lifecycle flags/lists and
   EventBus replacements before changing live objects. Keep the original instance
   tables and native userdata. Root missing defaults are cloned per instance;
   aliases to preserved tables and self references retain their intended identity.
   Conflicting alias topology is an explicit migration failure.
5. At an idle host boundary, publish through raw Lua writes and prepared C++ swaps,
   without executing user callbacks. Preserve on_start_called/on_destroy_called;
   do not rerun lifecycle side effects. Named prototype event callbacks are rebound
   in both active and pending operations, preserving order. Removed handlers are
   removed; ambiguous or unmapped callbacks reject the batch.

The manager refuses active Lua call stacks and EventBus dispatch. Main-VM GC is
stopped during preparation/commit to prevent finalizer reentry. The staging VM
has its own allocator/instruction accounting and cannot poison a live request's
budget just by rejecting a candidate. Diagnostics are bounded to 4096 UTF-8-safe bytes with invalid-byte escaping.
Main-VM allocator exhaustion remains a
fatal/reset boundary; a post-commit exception must never be reported as an ordinary
validation rejection pretending to have preserved the previous world.

## Key Interfaces

- ComponentManager::ReloadScripts(types) returns ok, selected types,
  instances_updated and typed resource/stage/message/line diagnostics.
- ScriptReloadCandidate::Stage(live, sources) returns unpublished exports.
- EventBus::PrepareReload / CommitReload separates validation and publication.
- Agent command reload_scripts returns a same-frame observation plus reload
  metadata, or a recoverable structured failure for ordinary candidate rejection.
- The editor's explicit script-reload tool invokes the same manager entry point;
  it does not repurpose project Refresh, scene load, Play or Stop.

## Alternatives Considered

Executing in the live global table and restoring its direct entries cannot undo
mutated nested tables, file writes, native operations, RNG state or retained
closures. A live `_ENV` proxy falling back to `_G` still exposes those capabilities.
Both were rejected as isolation mechanisms.

Restarting the full scene/VM is useful for reset but loses running gameplay state;
therefore it remains a separate operation. A fully persistent VM fork or arbitrary
heap/continuation migration would require a different runtime contract and much
larger verification scope. It is not implemented by this change.

## Consequences and Boundaries

Reloadable prototypes are plain tables with methods on the root prototype.
Instance-owned function overrides/cached closures, arbitrary prototype metatables,
threads and anonymous event callbacks are rejected rather than silently retaining
old code. Module-local captured values restart with the new code; persistent game
state belongs on instances or native values. Nested state-schema migrations and
external cached closures/package.loaded are not automatically migrated. A batch
must include its declaration-time dependencies; missing dependencies reject.

At this decision's original checkpoint, the legacy startup/scene replacement
loader remained separate. ADR 004 subsequently replaces scene-time reevaluation
with prepared declarations and canonical-source caching. Trusted startup still
has full capabilities; arbitrary external effects and postcommit callbacks are
not made reversible by either transaction.

## GDD Requirements Addressed

The owner's agent-native gameplay requirement needs real iteration without losing
an in-progress game. Village_Rim's persistent inventory, health, actor references
and callbacks provide representative retained state. No gameplay rule is renamed.

## Performance Implications

Reload is explicit, synchronous owner-thread work, not a per-frame path. Candidate
source count/bytes, isolated VM heap/instructions and transferable graph size are
bounded. It temporarily holds old and new code simultaneously. Measure large-game
editor latency before promising responsiveness or performance parity.

## Migration and Validation

The original migration kept legacy loading while exposing the explicit, constrained
reload path. ADR 004 now supersedes scene-time loading and its rollback boundary.
The explicit reload path retains actionable diagnostics. Tests exercise real edited files, shared captured
state, cyclic/aliased defaults, unchanged state after batch failures, native handles,
new/removed lifecycle methods, active/pending event replacement, no duplicate starts,
idle-boundary rejection, staged loops/OOM, same-frame agent observations and the
actual graphical editor tool. Run existing Debug, Release and sanitizer regressions.

At the original checkpoint, rollback could remove the explicit reload entry points
without an asset format or save conversion. Current source-cache integration must
also follow ADR 004; reverting to the old live scene loader would reintroduce its
known side effects. Changes remain uncommitted.
