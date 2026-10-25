# Phase 15: integrated two-world village and activation fixes

The new `tests/fixtures/two_world_village` project connects existing native engine
capabilities into one input-driven gameplay loop. It is a bounded migration
workload, not a completed Village_Rim port. Original rules and explicit tuning
choices are linked in [the integration plan](../two-world-integration-plan.md) and
[fixture contract](../../../tests/fixtures/two_world_village/CONTRACT.md).

## Delivered behavior

- Native seeds are consumed by planting. Crops grow, harvest into persistent
  ground remainders and enter the native bag only for the amount actually moved.
  Full bags preserve produce; empty seed stocks cannot create crops.
- A real equipped sword changes damage to enemies and the invasion source.
  Harvested food restores health lost to the underground guardian. Dead targets
  settle once. Keyboard and mouse paths produce identical gameplay observations.
- One scene contains two resident tilemap regions, a paired portal, distinct HUD
  feedback and a visible fade/camera transition. Village crops and invasion
  pressure keep updating while the player is underground. Holding input, standing
  after arrival and repeated return trips do not cause portal ping-pong.
- Invaders target living crops before the altar. Clearing a wave schedules another;
  destroying the associated underground source permanently closes that route,
  including cancellation of an already active wave. An undefended altar fails.
- Stable checkpoints use the actual SaveStore and native inventory group restore.
  A new supervisor process restores world, items, crop timers and permanent route
  closure. Reset retains disk data but restores authored state until explicit load.
  Active waves/transitions reject saving; failure can recover a previous checkpoint.
  Failure during a fade clears the transition lock so loading remains available.
- All actors and pickups use a fixed pool, avoiding actor publication during load.
  Complete schema/native preflight precedes the actual inventory transaction and
  fixed-world assignments. Bad late fields, future formats, illegal instances,
  float counts, missing portal references and actual incompatible-catalog native
  rejection preserve the previous live model. This is not rollback after an
  unexpected post-commit native exception or an arbitrary mid-combat snapshot.

The scene has 28 fixed actors and two 16×10 maps. Its geometric PNGs and bitmap
labels are generated reproducibly using Python's standard library. No Unity art,
scene, script or existing game save was modified or imported.

## Engine defects exposed by integration

The first real guardian encounter revealed that enabling an initially disabled
body did not advance its position. Lua enabled/type writes left stale physics
hierarchy decisions; disabled components could also consume their one-time start
without running it. The repair retains pending startup, invalidates changed
hierarchy flags and synchronizes Box2D participation at safe frame boundaries.
Existing bodies survive repeated toggles without reconstruction.

Independent review then reproduced two actual worker SIGSEGVs: a body-disable exit
callback directly destroying its Tilemap, and a map-disable exit callback directly
destroying its Rigidbody. A shared owner-thread contact-mutation scope covers
native state changes/destruction and rejects those direct reentrant destruction
entry points. Deferred actor/component removal and queued cell edits continue to
work. See [activation contract and measurements](../rigidbody-activation.md).

## Acceptance matrix

The complete suite now has **72 tests: 70 core tests and two optional TypeScript
compiler/replay tests**. New tests are `agent_two_world_village`,
`two_world_render_software`, `rigidbody_activation` and `tilemap_activation`.

| Final gate | Result |
| --- | --- |
| Debug, all 72 | 72/72 passed, 77.83 s |
| Release, all 72 | 72/72 passed, 51.35 s |
| ASan/UBSan, all 72 | 72/72 passed, 162.19 s; `detect_leaks=0` |
| Metal/OpenGL integrated readback | Both passed; village/underworld/closure captures inspected |
| Source-visible-only fixture copy | 78 files; Metal launch and complete input/readback passed |

The positive agent loop uses actual player inputs throughout. Valid checkpoint
variants only establish bounded failure/combat comparison starting conditions;
they do not replace that positive loop. Native rejection tests use real distinct
catalog objects rather than a fabricated failing return value. Render tests drive
real input and check world/player/item pixels, the portal blackout/camera and the
permanent closure HUD. Full-suite logs, captures and measurements remain ignored
under `build/phase15-*`; test durations are not engine speed measurements.

Human playtesting, multiple islands, complete original items/actions, elemental
combat, bosses, quests, old save conversion, arbitrary combat snapshots and
streaming remain open. The same-scene regions do not prove a cross-scene offline
simulation system. Windows/Linux packaging and cross-engine SOTA comparisons also
remain unverified. The owner requested autonomous development to stop after this verified round.
The broader modernization goal is not complete.

## Performance follow-up

The initial matched animation-only comparison observed a 5.02% native 10k-actor
p50 increase with non-overlapping run ranges. A narrow early return now avoids
the new activation scan when no physics world exists, while preserving the
post-callback checks. The final three-pair comparison passed all 24 correctness
rows but had large timing variability and overlapping ranges in every group.
Its higher medians do not establish a stable cost, and the measurement does not
confirm that the earlier regression has been eliminated. Both comparisons and
the intermediate executable remain locally preserved; the activation document
contains the full numbers. No further optimization or measurement was started
after the owner's stop-at-this-round instruction. LeakSanitizer and cross-engine
SOTA conclusions remain outside the evidence.
