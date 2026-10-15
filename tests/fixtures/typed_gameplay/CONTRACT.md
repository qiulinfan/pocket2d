# Typed gameplay comparison fixture

This is a bounded authoring experiment using Pocket2D's real Lua runtime, not a
second VM or a full Village_Rim migration. The tracked `TypedController.lua` is a
handwritten reference. `tests/script_authoring/TypedController.ts` independently
implements the same rules against the generated curated host declarations. Its
`SaveSchema.ts` helper is imported as a real module and bundled into the single
`TypedController.lua` entry; no extra runtime module file is deployed.
Compiled Lua stays in ignored compiler output directories. The replay copies the
fixture into two temporary projects and overlays exactly one compiled component
in the TypeScript variant; all scenes, assets, input and engine code are shared.

Arrows move the native Rigidbody at two units/second, including normalized
diagonals. Horizontal changes mirror the SpriteRenderer. Space starts a one-shot
attack on a key edge, receives real frame/hit/completion events, and returns to
idle after completion. Holding Space does not repeat attacks. P pauses movement,
attack input and animation; R resumes. H removes one health per key edge, G heals
one per edge up to three. At zero health the body stops, animation stops, and
movement, healing and attack input are ignored until loading an alive checkpoint
or resetting the scene. Animation events run before the controller's ordinary
update, matching the native tick contract.

F5 writes `typed_gameplay/main.save.json` under the selected save root; F9 reads
it. Game schema 1 stores bounded health, body position and horizontal facing.
Both save and load validate the complete game schema before writing persistent
data or applying loaded state. Loading reconstructs idle playback for an alive
actor and stopped playback for a dead actor; transient attack/pause/input state,
callback counters and the Lua VM are not checkpointed. Invalid or unsupported
game schemas report a status without applying their values. Normal engine ticks
continue while processing a load request.

The replay compares complete JSONL snapshots on every input tick, including
native body poses, component scalar fields and animation event traces. It also
compares serialized save bytes, schema rejection, reset behavior and checkpoints
loaded by entirely new supervisor/worker processes. Independent assertions ensure
that two equally broken implementations cannot pass merely by agreeing. The
result establishes parity for this workload, not complete JavaScript semantics,
complete SDK coverage, source-mapped runtime diagnostics, or production readiness.

The PNGs and animation JSON are copied from the self-authored `sprite_animation`
fixture; `generate_fixture.py` reproduces the geometric assets without a font or
external art dependency. No Unity assets are included. Human gameplay feel and
visual presentation remain separate from the automated parity assertions.
