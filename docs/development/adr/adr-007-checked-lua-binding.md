# ADR 007: checked Lua binding with explicit host call policies

## Status

Accepted for implementation under the owner's autonomous Pocket2D direction,
2026-10-15. Production migration is implemented; its full validation is recorded in the
Phase 13 checkpoint. The isolated experiment alone did not establish acceptance.

## Context and dependencies

The owner requested a less primitive Lua embedding and possible TypeScript game
scripting. Existing work already provides native lifetime handles, instance-owned
defaults, bounded agent execution, prepared reload/publication/edit transactions
and a real TypeScriptToLua authoring path. Their contracts must survive a binding
upgrade; a new library cannot substitute for them.

Dependencies: ADR 001's runtime direction, ADR 002's owner-thread/lifetime rules,
ADRs 003/004's reload and scene preparation, and ADRs 005/006's publication/edit
boundaries. No Village_Rim gameplay rule or asset/save format changes here. The
requirements are in roadmap.md and scripting-options.md; no Unity-specific
architecture registry or new multi-VM control layer is introduced.

## Engine compatibility and evidence

Use C++26, C-built Lua 5.4.9 and LuaBridge3 prerelease 3.0-rc12 at
`53e031b7df6a14d43f92a54fd792b76dbadcc970`. Keep SDL3, Box2D and TypeScriptToLua's
Lua54 target unchanged. Official source inspection and actual baseline/candidate
experiments are documented in binding-experiment-results.md.

The four representative unmodified engine TUs compile with 2.7 and fail with rc12
(23 diagnostics). Explicitly adapted standalone value/ownership/call/budget probes
pass Debug, Release and ASan/UBSan. They reveal writable-member, result-bool,
return-arity, exception/panic and diagnostic differences that compilation alone
would miss. They do not certify a migrated production runtime. C++26 deprecation
warnings remain in the candidate's Expected.h; the candidate is a prerelease.

## Decision

Proceed with a coherent full LuaBridge3 integration while holding the VM fixed.
Retain a runnable baseline for comparison. Make checked conversion and protected
call policies explicit at real host boundaries; avoid a new generic VM abstraction
or permanently supporting two binding versions in the production binary.

- Check conversion results before extracting values. Preserve actual Lua type and
  native-domain validators; never use conversion success as an enabled flag or
  bulk-switch to unsafe_cast to hide failures.
- Preserve all 39 currently writable direct-member registrations, the 59 explicit
  getter/setter registrations and 15 readonly registrations. The latter counts
  include Rigidbody macro expansions. Engine-owned identity injection remains
  possible through the existing host path without making script identity mutable.
- Lifecycle/events still ignore callback return values. Protected calls must
  restore their saved Lua stack and retain original errors, source context and
  bounded diagnostics. A returned failure cannot bypass the runtime error sink.
- Initialization retains ScriptBudget's aborting panic callback and actual
  allocator/hook. Enable safe handling for library-registered raw C functions;
  manually installed C metamethods still need their own safe boundary.
- Preserve raw identity, Actor generation, default graph alias/cycle semantics,
  native ownership and GC/VM shutdown order. Preserve every prepared transaction's
  ordinary rejection versus fatal post-publication distinction.
- Keep the Lua-visible API and generated TypeScript contract stable. A binding
  migration must not require changing generated fixtures to new constructor,
  receiver or callback-return conventions.

Production scope currently includes 58 casts, 13 Stack gets, seven Stack pushes,
five discarded callback calls, two old class-level pcall calls, and exception-typed
reporting hooks. These counts describe the current source, not a mechanical
migration recipe. Use separate owners for manager/error boundaries, API registration,
and native/transaction callers, with the coordinator owning dependency/build and
final integration. Apply the same policies to tests without weakening assertions.

## Alternatives and consequences

Retaining 2.7 avoids migration cost but preserves its legacy unchecked conversion
and exception-centric surface. It remains the comparison baseline, not the selected
long-term endpoint. sol2 offers protected calls and ownership facilities, but would
replace the widely used LuaRef model; only its sources were researched here, so no
measured advantage justifies that larger simultaneous rewrite. Lua 5.5, Luau,
LuaJIT or a JS VM change language/runtime semantics and remain separate decisions.

The candidate's measured native property pair is faster in this isolated workload
(100.225 → 74.589 ns median), while method/callback rows show no general speedup.
Its single-TU compilation time and probe executable size increase. The decision
is based on checked APIs, maintainability and a promising specific path, not an
engine-wide speed claim. No SOTA inference follows.

## Validation and rollback

First verify API declarations/bindings, native numeric domains, enabled=false,
wrong-type rejection, lifecycle errors/return values, nested reentry, identities,
raw C boundaries, budgets and shutdown. Then verify reload, scene/actor/edit
preparation and complete compiled TypeScript replay/reload parity. Require the
full applicable 65-case Debug/Release/ASan matrix and representative real rendering
and gameplay checks. Measure the same whole-runtime workloads against the retained
baseline with identical inputs and safety policy; isolate any observed regression.

Production acceptance remains unproven until those checks run on migrated source.
Do not ship an exception-sink shim that silently discards errors, modify TSTL output
to mask incompatibility, or relabel four standalone tests as full engine coverage.
If the integration fails its contracts, keep the baseline runnable and repair or
reconsider the candidate using the recorded evidence. Preserve upstream licenses
and source pins; no commits or publication are authorized by this record.

## Integration findings

The real API contract rejected the Phase 12 opt-in assumption: LuaBridge's global
exception flag makes wrong method receivers throw outside its member wrapper,
so a Lua pcall cannot catch the error. Production therefore stays in the library's
result mode and does not call enableExceptions. C++ exception support and SAFE_C
handling remain enabled; LuaBoundary owns checked conversions, protected calls,
tracebacks and bounded host diagnostics. The allocator's panic hook is untouched.

After an initial Lua-heavy runtime regression, one-read enabled evaluation removed
redundant registry accesses without changing the default-true nonboolean policy.
A subsequent process sample identified repeated equalstates registry comparisons
under Release assertion expressions. One documented local upstream patch returns
true for identical non-null Lua-state pointers; different coroutines/VMs retain
the original comparison. Same-thread, cross-coroutine alias and independent-VM
predicate regressions are in lua_vm. No assertions or stack checks were disabled.
See binding-runtime-performance.md for initial and subsequent matched measurements;
the initial regression remains visible rather than being replaced by later data.

Explicit nil method receivers also crash the retained 2.7 Release baseline; the
migrated Debug process reproduces that existing flaw. This is separate from the
fixed wrong-type receiver regression. General nullable pointer arguments must
remain valid when repairing self-receiver guards in a later bounded change.

## Receiver follow-up

Phase 14 repairs the reproduced instance-self crash through a second documented
local patch. Typed member/property receivers are checked before native invocation;
class-bound proxies reject nil before constructing their arguments, while ordinary
nullable parameters and namespace/static functions retain their existing meaning.
The checked result lifetime ends before raising a Lua error. Raw class C callbacks
keep their closure shape and safe-C guard; raw index/newindex reject malformed
receiver structures through Lua errors. This is a bounded ordinary-API contract,
not an arbitrary debug/metatable/upvalue sandbox. See native-call-receivers.md and
../checkpoints/phase14-checkpoint.md. Matched performance ranges overlap on the principal valid
paths; native-receiver-performance.md retains the measured costs and variability.
