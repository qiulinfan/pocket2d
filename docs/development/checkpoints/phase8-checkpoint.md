# Phase 8: prepared scene transitions and stable module lifetime

Local working-tree implementation; no commit or push. The overall Pocket2D goal
remains active. This slice closes reproduced scene-preflight side effects; it does
not certify complete game migration, all runtime mutation paths or SOTA performance.

## Delivered

- VM/API initialization is separate from trusted startup source evaluation. Initial
  scene preference is resolved first and each selected source executes once.
  Known canonical source paths retain prototypes and private module state across
  scenes. New declarations use the isolated candidate VM; successful explicit
  script reload also updates the source cache. Failed preferred sources do not
  fall back silently. Native component shadow files are skipped, while other
  host/stdlib name collisions reject before source evaluation. Canonical targets
  must be Lua files, and a committed source keeps its type ownership for the VM
  lifetime, preventing a later symlink alias from reusing another type's cached
  prototype. Failed preparation does not publish source ownership. Explicit reload
  rejects a registered canonical path that has since been redirected, preventing
  a source read from being committed under a different variant's cache key.
- Engine prepares actor data, templates, script selections, detached component
  instances, lifecycle membership, native configuration and Tilemap/Animation
  resources before replacing the old world. Templates have a per-preparation
  canonical-path cache, avoiding per-actor file reads without polluting the live
  Actor template cache. The C++ Scene::LoadScene helper is data-only.
- Core lifecycle membership uses raw/static prototype lookup during construction.
  Default graphs reject finalizer/close/weak behavior before cloning. Nested GC
  guards cover prepared Lua references until those references are released.
  Ordinary preparation failures preserve generations, Lua/native object identity,
  subscriptions, queued lifecycle work, physics state and global state.
- Native host specification edits prepare before destructive replacement. Invalid
  fields, identities, numeric domains, derived transforms and particle capacities
  reject without changing the existing component/spec. A reproduced finite but
  extreme gravity scale now rejects before its next tick could become non-finite.
  This is not blanket validation of direct Lua native-field assignments.
- DontDestroy remains explicit per actor. Retained-parent loss, incoming UID
  collisions, cross-scene authored parent links and source-variant changes for a
  retained component reject rather than inventing renumbering or reparenting.
- Editor scene documents and Play/Stop flags swap only after Engine success. Its
  outgoing dirty draft is saved through a precommit host gate after target
  validation; a real file-save failure leaves the world and documents intact.
  Actor selection clears only on successful open. Independent map drafts remain.
- Agent step compares scene-attempt revisions. A failed queued load reports once
  while the preserved world executes its ordinary tick; later requests can run
  their full frame count. Unexpected postcommit teardown failures stop the runtime,
  clear unsafe indexes and end the worker with requires_reset:true.

## Verification

All **52 CTest cases pass in Debug, Release and ASan/UBSan** after the final
source-identity fixes. The core has 50 tests plus two optional TypeScript authoring
tests. The sanitizer run uses ASAN_OPTIONS=detect_leaks=0 because LeakSanitizer is
unavailable on this local macOS toolchain. Final complete-suite times were
35.60 s / 31.19 s / 58.92 s respectively; these are verification durations, not
engine performance benchmarks. New
cases are scene_scripts, scene_component_preflight, scene_transition,
agent_scene_transition and scene_persistence. Existing explicit reload, compiled
TypeScript, inventory, save/load, animation, Tilemap, renderer, lifetime and
supervisor regressions remain enabled.

The scene regressions reproduce attempted Save writes, RNG consumption, body
teleports, scalar changes and delayed destruction from the former loader. They
cover cached variants and module-private state, failed candidate nonpublication,
source identity/confinement, retained graph conflicts, template freshness and
copy isolation, GC-delayed effects, host mutation preservation, precommit reentry,
editor dirty/paused states and recovery after a fatal teardown callback.

A three-frame Metal capture of the original default project also succeeds and
was visually inspected. This confirms startup and drawing, not player feel or
audio quality. Logs and screenshots remain ignored under build/phase8-*.

## Remaining boundaries

Trusted startup can execute full-capability Lua. Arbitrary startup, lifecycle or
host-gate external IO is not rolled back. Main-VM allocator exhaustion and
unexpected postcommit failures require reset. A new dynamic declaration needing
unrestricted top-level capabilities requires a fresh trusted startup instead.
Loaded module variants stay cached until VM shutdown; plans/tokens must be released
before shutdown. No aggregate native RAM cap or adversarial filesystem guarantee
is claimed.

The next runtime validation work starts with direct Rigidbody methods (including
SetGravityScale), ParticleSystem configuration arithmetic and manual Burst. Scene
preparation checks do not cover these mutation routes. Read-only audit found raw
float conversion, signed-int capacity arithmetic and dynamic burst counts here;
large-allocation probes were deliberately not executed.

Direct Lua native setters, runtime spawning/duplication failure paths, maintained
binding evaluation, source-aware diagnostics, platform packaging and matched
whole-game benchmarks remain further work. The outer Project-panel double-click
selection/notice flow still needs a manual UI check. Fatal scene-open errors are
also logged because the stopped editor loop may exit before drawing a notice.
Headless Session tests cover
the document/runtime transaction. Full Village_Rim content and human playtesting
remain separate gates.

See [ADR 004](../adr/adr-004-scene-preparation.md), [scene contract](../scene-transitions.md),
[agent protocol](../agent-protocol.md), and [status](../status.md).
