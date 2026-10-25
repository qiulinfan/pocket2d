# Typed host API and TypeScript authoring experiment

Phase 5, 2026-10-15. Implemented and verified; see checkpoints/phase5-checkpoint.md and status.md.
The contract adds a curated, tested host surface and compares one real
TypeScriptToLua component against a hand-authored Lua reference. This is an
optional authoring frontend, not a second VM or complete JS/npm compatibility.

## Single source and outputs

Resource worker owns `api/host-api.json`, `tools/generate_script_api.py`,
`api/types/pocket2d.d.ts`, `api/types/pocket2d.lua`, and generated
`src/engine/scripting/ScriptAPIContract.h`. The header exposes
`namespace PocketScriptAPI { inline constexpr std::string_view kContractJson=...; }`.
Generator supports `--check` to reject stale generated outputs. The schema must
explicitly distinguish instance self from namespace/static calls, nullable results,
arrays, mutable/readonly/runtime-engine-owned fields and native opaque values.
Use a small structured type representation, not two independently maintained SDKs.

Coverage is curated and explicitly versioned, not an assertion of full API parity:
Actor lookup/component access/validity (no lossy uint64 GetUID declaration),
Component identity/enable, Transform, SpriteRenderer, Rigidbody motion, Vector2
constructor/value API, Input keys, SpriteAnimation playback/state/events, Tilemap
cell API, Save.Write/Read/result data, and Debug.Log if used. Add only verified
members. Actor.GetComponent string-literal overloads may map known native types;
custom key lookup stays nullable/unknown or an explicit caller-chosen generic.
Native Vector2/Actor types need nominal markers so plain objects cannot pretend
to be native userdata. Engine-owned actor/key can be readonly in the authoring
SDK, but must be labelled engine-owned rather than falsely claimed runtime setter
rejection. Save.Read data requires runtime schema validation, not an unsafe generic.

LuaLS output is metadata only (`---@meta`), never loaded as a gameplay component.
TypeScript static namespace/constructor calls use `this:void`/appropriate noSelf
annotations; instance methods and callbacks keep correct self. No Node/DOM APIs
are advertised. Root exposes the generated JSON contract in agent describe under
`script_api`, without runtime file dependency or inventing RPC mutation commands.

## Compiler tooling

Root owns `tools/script-authoring/**` package/lock/compiler/tests, all CMake, and
AgentSession describe integration. Pin npm typescript-to-lua 1.37.1 and its peer
TypeScript 6.0.2 (registry verified). Node is already available; install locally
with scripts disabled, no global changes. Official docs are the source for
luaTarget=5.4, inline/nonbundle or embedded-minimal/bundled lualib, global component registration and self conventions.

Compile a project into a dedicated generated output directory. Type/transpiler
failure must leave the prior good output unchanged. Stage output before promotion;
do not erase a nonempty directory that the tool does not own. Return machine-
readable diagnostics with file/line/column/code/message. No automatic engine file
watcher/hot reload this slice. Generated experiment Lua lives in ignored build/
or test temporary directories, not a second manually maintained gameplay source.

CMake core tests remain independent of npm. An explicit
POCKET_ENABLE_TYPESCRIPT_TESTS option may require the locally installed pinned
toolchain and enable real compilation/replay; absent dependencies must be an
explicit configuration error, not a passing silent skip. Separate npm commands
validate generation and negative typing cases. No dependency download at CMake
configure/test time.

## Gameplay comparison and verification

QA owns `tests/fixtures/typed_gameplay/**` (handwritten Lua reference plus existing
self-authored animation geometry), `tests/script_authoring/**` (TS source/config),
and `tests/agent/agent_typed_gameplay_test.py`.

Implement the same real movement/animation one-shot attack/health/damage/save-load
controller in TS and Lua. Use actual Rigidbody, Input, SpriteAnimation callbacks,
Save and actor/component lookup; no private simulation mock. Include bounds/zero
health, held-key edge semantics, direction change, attack completion, pause/reset
and validated save recovery. Keep a concise deterministic input replay and exact
observable-property/position/event-count comparisons. Source image licensing is
already self-authored fixture geometry; no Unity asset migration.

Compiler tests include wrong scalar arguments, a plain object passed as Vector2,
readonly animation state writes, nullable actor access and unknown members. Assert
real diagnostics, not just `@ts-expect-error` suppression; ensure failed emission
does not overwrite last good Lua. Test valid generated code under the real
Debug/Release/sanitized engine where meaningful. Source-map runtime diagnostics
are not claimed until separately verified; TS compiler diagnostics are in scope.

Native worker owns `tests/engine/script_api_contract_test.cpp` and may write a standalone
runtime Lua probe fixture. It verifies declared runtime names/properties and real
representative calls/readonly failures/nullable behavior; compile-only nominal
markers and authoring-only readonly rules are distinguished. Coordinate manifest
shape with resource worker; report mismatches instead of silently widening types.
No production binding rewrite without root coordination.

All agents share the checkout and have disjoint ownership. Preserve other work,
no commit/push. Completion evidence must distinguish static typing, transpilation,
real gameplay parity, and production readiness. Current cross-engine/SOTA/full
migration gates remain open even when this experiment passes.
