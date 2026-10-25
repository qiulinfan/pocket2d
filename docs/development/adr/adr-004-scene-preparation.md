# ADR 004: prepare scene state before replacing the running world

## Status and date

Accepted for implementation under the owner's autonomous engine-development
instruction, 2026-10-15. Final verification belongs to the phase 8 checkpoint.

## Context

The previous scene preflight executed every Lua component file in the live VM.
A rejected scene could change health, move a body, write Save data, consume RNG
and queue actor destruction. Startup executed scripts twice. Component defaults,
native property setters and physics configuration could also fail after old actors
were destroyed, leaving stale Engine actor pointers. The editor replaced its
SceneDocument before learning whether the runtime accepted that scene.

## Dependencies and engine compatibility

Builds on ADR 001's owner-thread simulation, ADR 002's stable actor identities and
ADR 003's isolated candidate VM. Uses existing C++26/Lua 5.4.9/SDL3/Box2D interfaces;
no new package or runtime is introduced. Existing regression tests define the
supported trusted-startup behavior and native construction rules.

## Decision

VM/API initialization and trusted project bootstrap are separate. Resolve the
initial scene preference first, then evaluate each selected source once. Preserve
trusted startup capabilities; do not pretend full-capability IO can be rolled
back. Snapshot host/stdlib global names before project code, reject conflicting
module filenames, and ignore legacy files named after the six native components
whose runtime implementation already took precedence.

Keep loaded modules for the VM lifetime, indexed by canonical source path. Scene
changes reuse registered source versions, including their module-local state;
ordinary scene loading does not act as implicit code reload. New paths/types use
the isolated candidate contract. Explicit ReloadScripts updates the same cache.
A bad preferred source never silently executes a lower-priority file. Different
type names cannot identify the same canonical source, including a previously
loaded variant no longer selected. Canonical targets must end in .lua, matching
explicit reload. Source ownership publishes only with successful preparation
commit and survives until VM shutdown. Scene variants that would
change a retained component's type definition are rejected.

A scene transaction prepares merged actor specs, script selection, detached
component instances, static lifecycle flags, native configuration, and existing
Tilemap/Animation assets. Default graphs are inspected before cloning to reject
finalizer/close/weak-table defaults that could run delayed effects after rejection.
Templates use a per-preparation cache and explicit path preference, leaving the
live Actor spawn cache and active scene preference unchanged. Core lifecycle
membership comes from explicit/static inherited fields; construction does not
execute function-valued __index to synthesize those callbacks. Ordinary dynamic
data and protected OnAnimationEvent lookup remain available at runtime.

Only after preparation succeeds does Engine clear old state, publish script
selection, splice preallocated actor nodes and install prepared components.
Original instances are retained on ordinary rejection. Nested scene transactions
and stepping within a precommit host gate are rejected. DontDestroy remains an
explicit actor flag: reject retained-parent loss, incoming UID collisions and
cross-scene authored parent references instead of guessing new identities.

The editor prepares its candidate document and uses a host precommit gate to save
the outgoing dirty draft only after target validation succeeds. Save failure
aborts before world mutation. Document/mode/selection changes occur only after
Engine success. Dirty map drafts are independent and are not discarded.

Native spec edits use the same detached preparation before replacing a component.
Invalid host configuration preserves both the old component and its authoring spec.
This does not validate every direct Lua native-field write or make every gameplay
mutation transactional.

## Failure classification

Each actual scene-load attempt has a revision. Agent stepping reports only a new
failed attempt; a retained last diagnostic does not fail unrelated later steps.
Ordinary preparation rejection preserves the world. Teardown callback errors or
unexpected exceptions after destructive commit begins stop the runtime, clear
Engine's derived pointer indices, and require a fresh runtime/reset. They are not
reported as successful rollback. Main-VM allocation exhaustion retains its existing
fatal/reset boundary. Arbitrary OnStart, OnDestroy and host-gate IO are not reversible.

## Alternatives

Moving only the old script reload later would still execute unvalidated code and
construct components after destroying the world. Replaying live Lua and restoring
_G cannot undo native effects, nested data or files. Restarting the entire VM for
every scene would break retained actors and project module state. All were rejected.

A fully unrestricted whole-world/VM transaction is not claimed. Trusted bootstrap
supports legacy scripts; dynamically introduced declarations use the stricter
candidate contract and may require a fresh runtime if they need startup capabilities.

## Performance and validation

Preparation temporarily retains old and new component state. Scene templates are
read once per canonical source within a preparation, not once per actor; this is
not a cross-engine benchmark claim. Native physics values must fit their finite
geometry/mass domain. Scene-authored particles use checked 64-bit capacity arithmetic
and a one-million-particle per-emitter reserve bound, not a process memory sandbox.

Rigidbody preparation also checks derived gravity rather than accepting any
finite `gravity_scale`. With the current world gravity `(0, 9.8f)`, it computes
`a = double(9.8f) * abs(double(gravity_scale))` and requires a finite result with
`a * a <= double(FLT_MAX) / 256`. The margin leaves headroom for float vector
norms and integration intermediates. Scene loads and host native-spec edits
apply this check before replacing live state. Regression coverage rejects
extreme finite scales while the retained world continues stepping with finite
body state. This bound is not a proof of numerical stability for every Box2D
configuration, and direct Lua native-field writes are outside this preparation
contract.

Validation covers previous side-effect reproducers, source variants/cache lifetime,
native fields/defaults, retained actor conflicts, failed editor open/save/Play/Stop,
per-attempt agent errors, fatal-reset classification, delayed GC effects and the
existing gameplay/renderer/reload matrix. Main runtime setters, native allocation
budgets, broader platform packaging and human editor interaction remain separate
completeness gates. See scene-transitions.md and the final checkpoint.
