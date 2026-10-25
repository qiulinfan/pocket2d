# Two World Village: integrated gameplay fixture

This self-authored fixture joins farming, native inventory, combat, a paired
portal and permanent invasion-route progress in one scene. It is a bounded
Village_Rim migration slice, not the full Unity game, its art, or a legacy-save
converter. The scene and both regions remain resident; portals do not call
Scene.Load or prove resource streaming.

Independent automated acceptance is recorded in
[Phase 15](../../../docs/development/checkpoints/phase15-checkpoint.md). Human playtesting
remains a separate gate; this contract defines behavior, not game feel.

## Regions, objects and controls

The village map is 16×10 cells with origin (-8,-5); the underworld map has the same
size and origin (16,-5). Their camera centers are (0,0) and (24,0). Village crop
plots are cells x=3..6, y=2..5, ordered row-first in the sixteen-entry save array.
The permanent route identifier is `island_1`, implicit in this fixture's schema
and namespace. World identifiers are exactly `village` and `underworld`.
The one island is considered visited at the village starting position; this slice
does not implement the original multi-island visit discovery state.

The village state actor (UID1) owns state/farm/route/save/view components; player
UID2, altar UID3, source UID4, portals UID5/6 and maps UID10/11 remain present.
The three invader actors, guardian and sixteen crop-pickup actors are fixed pools.
Ordinary restore never publishes a replacement actor or loads a template/resource.
Both portal endpoints must remain available before a saved world can be applied.

- Arrows move the real player body. E interacts with village soil/crops.
- Space attacks actual nearby combat targets with a 12-tick cooldown.
- Right-click within the world view to interact with soil under the player, as
  an alternative to E; the pointer does not select a distant plot. Hold the left
  mouse button over the world view to attack. HUD pointer input is excluded.
- 1 toggles the sword; Q consumes food when healing is useful.
- F5 saves explicitly; F9 loads explicitly. Reset returns to authored state and
  leaves the save file intact. Startup does not automatically load a slot.

The player starts with eight seeds and one unique sword in a 30-slot native bag.
The four equipment filters are subweapon/head/chest/legs; this catalog currently
contains only a subweapon. Seed stacks cap at 99, food stacks at 9. Food heals
three points; the sword adds two attack to base attack one. Health limits are
player6, altar8, source9, guardian6 and invader3.

Planting consumes one seed only on success. Growth takes 120 gameplay ticks;
harvesting yields one food in that plot's persistent ground remainder. Native
Inventory.Add's actual `moved` value reduces the remainder, so a full or partially
filled bag cannot silently discard food. Invasion enemies prioritize crops before
the altar. The first pending countdown is 900 ticks and later countdowns are 600;
a wave uses the three invader slots. Clearing a wave does not permanently win.
Destroying the source closes this route, cancels pending/active pressure and clears
its invaders. Village timers continue while the player visits the underworld.

Portals pair positions (5.5,2.5) and (18.5,2.5), with arrival points (4.25,2.5)
and (19.75,2.5). The transition lasts 42 gameplay ticks, warps at tick24 and finishes with nine
ticks of grace plus a leave-trigger rearm requirement. The game owns the movement
lock and camera update. A missing endpoint must fail safely rather than move to
an invalid world.

## Checkpoint policy and scope

VillageState is the only gameplay update driver. It handles F5/F9 before timers,
AI, farm, portal and combat actions: stop the player/guardian/invader velocities,
invoke VillageSave, then return. The engine still completes its normal native
frame. Failure comparisons therefore begin at this documented frozen-input
boundary; they do not imply that pre-command velocities or an entire engine frame
are rolled back.

Save requires ready, playing state, no transition and a non-active invasion route.
Ordinary playing-state Load has the same busy restrictions. Load additionally
allows recovery from failed state using a valid checkpoint. Busy Save does not
call Save.Write and preserves the previous file bytes. Corrupt or invalid Load
changes only status/error feedback after the frozen-input boundary.

`game.config` declares `save_namespace=two_world_village`; slot is `main`, game
schema version is 1. With `--save-dir ROOT`, the path is
`ROOT/two_world_village/main.save.json`. No save defaults into fixture sources.
The engine owns format_version and atomic file replacement; the game owns payload
validation. Unknown game versions are rejected, not silently migrated.

## Schema 1

The root has exactly these eight keys:

```text
world_id
player     {x, y, health}
bag        [30 × {item_id, count, instance_id}]
equipment  [4 × {item_id, count, instance_id}]
crops      [16 × {stage, remaining_ticks, drop_count}]
route      {closed, wave, remaining_ticks, altar_health, source_health}
guardian   {x, y, health}
stats      {planted, harvested, consumed, crops_destroyed, defeated}
```

All records have exact keys and no metatable; arrays are dense and fixed-length.
All counts, health, stages, wave and ticks must be actual Lua integers, matching
the native Inventory API. A JSON/Lua floating `4.0` is not accepted as a slot count.
Coordinates may be finite integer or floating values.

Player health is 1..6. Village player X is -6.75..6.75; underworld player X is
17.25..30.75; Y is -3.75..3.75. Guardian health is 0..6 and its saved coordinates
remain within those underworld bounds even when dead. Guardian activity is derived
from health, not saved as a second potentially contradictory flag.

An empty slot is exactly `{item_id="",count=0,instance_id=""}`. Seed/food use
empty instance IDs and their stack limits. There is exactly one sword across bag
and equipment, with count1 and instance_id `sword-island-1`; equipment may contain
it only in slot1. All bag/equipment containers originate from the same catalog
object. Initial seed count eight is an initial condition, not a permanent global
save-data total: valid checkpoints may contain different bounded seed counts.

Crop stage2 is bare, stage3 growing and stage4 mature. Growing crops have 1..120
remaining ticks; other stages have zero. Each drop_count is 0..1,000,000. A drop can
remain while that plot is replanted, so it is not restricted to a bare stage.
Stats are integers 0..1,000,000 and satisfy:

```text
planted = harvested + crops_destroyed + number of growing/mature plots
harvested >= consumed
```

The schema does not add an unsupported fixed initial seed/food-total invariant.
Action-level tests independently check actual item deltas and pickup conservation.

Route wave is 1..1,000,000; altar health is 1..8. An open route has source health
1..9 and a pending countdown of 1..900 ticks. A closed route has source health zero
and countdown zero. A checkpoint never contains an active invasion snapshot.
Load restores the saved wave/countdown directly, without incrementing or scheduling
an extra wave. Crop time is relative remaining time, not an absolute engine frame.

Cooldowns, input latches, facing, portal transition state, attack effects, Box2D
contact manifolds and Lua/native identities are not serialized. Successful Load
resets transient action state and derives visuals/attack/active flags from restored
canonical data. It clears the fixed invader pool and restores the guardian and
player. Both worlds' canonical crop/drop/route state are restored, not just the
currently visible region.

## Core and save-module boundary

VillageSave has no OnUpdate and needs no startup callback. Core initializes its
catalog, real bag/equipment and fixed references before calling the save module,
including when F9 is pressed on the first gameplay frame.

- `IsCheckpointStable(for_load)` returns bool, code. True for a valid save boundary;
  the load form also permits failed-state recovery.
- `CaptureSaveState()` returns an independent plain payload. Crop state comes from
  authoritative gameplay data, not a same-callback Tilemap.GetCell read after
  SetCell has merely queued an edit.
- `CanApplySaveState(data)` only reads readiness, fixed player/body/map/portal/pool
  references and current load eligibility; it returns bool, code before mutation.
- `ApplySaveState(data)` first restores the actual bag and equipment in one native
  RestoreGroup. Ordinary failure returns before any world writes. Success then
  applies already-validated data to fixed handles/scalars/positions/tile queues,
  with no actor publication, template load or resource I/O, and refreshes derived
  state before the early-return input tick is observed.

VillageSave checks all schema fields and relationships, checks fixed world
availability, and performs a native semantic preflight on two scratch containers
created from the same catalog. Those temporary models are not the live inventories.
The final actual RestoreGroup is still checked: native group validation is atomic
only for its supplied containers, not an implicit transaction over the whole game.
All real participating containers must be included together; the fixture has only
bag and equipment. World scalar/native state is not changed before that group
succeeds.

Tilemap.SetCell may return false for an already-matching valid cell. It queues
changes rather than making GetCell immediately read the requested value. Fixed
indices/IDs and live maps are validated before apply; repeated identical saves
must not turn a legitimate no-op into a restore failure. The engine commits tile
changes through its normal pre-physics phase.

The guarantee is for ordinary I/O, schema, unavailable-reference and native
inventory-validation failures. Unexpected post-commit host allocation or script
failure is not advertised as whole-world rollback. VillageSave deliberately does
not swallow a post-commit exception and falsely report an unchanged world.

`save_status`, `load_status` and `last_error` are published on VillageState and
VillageSave. Success is `saved`/`loaded`; busy/readiness/world codes come from core,
file codes from Save, game schema rejection is `invalid_game_schema`, future game
schema is `unsupported_game_schema`, and native inventory preflight failure is
`invalid_inventory`. Save-module counters count only successful operations.

## Verification boundary

Run a built runtime from the repository root with an explicit local save folder:

```sh
build/ninja-debug/game --project tests/fixtures/two_world_village --save-dir build/two-world-playtest-saves
```

Independent QA must complete the real input-driven farm/equip/food/portal/combat
loop, permanent route closure, fresh-process recovery, busy save preserving old
bytes, corrupt/future/late-invalid records, illegal gear instances, float counts,
missing paired portals and native RestoreGroup rejection with unchanged real
inventories/revisions/world. Legal full-bag checkpoint variants are allowed for
failure-path setup; they do not replace the positive player-input loop.

Graphics/readback and human playtesting assess the two regions, portals, combat,
HUD, failure feedback and the risk of leaving the village. This fixture does not
migrate all islands, NPCs, quests, elements, bosses, weather, crop catalogs, arbitrary
combat checkpoints, Unity assets or old save formats.
