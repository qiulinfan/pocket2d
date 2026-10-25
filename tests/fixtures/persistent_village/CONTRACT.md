# Persistent village verification fixture

This is a bounded persistence workload derived from Pocket2D's self-authored
`tilemap_farm` fixture. It does not claim to migrate Village_Rim's full game,
altar, demon tree, invasion system, inventory, or Unity assets.

Controls: arrows move the actual Box2D body; E plants/harvests; Space removes the
facing solid wall cell. Harvest one crop, stand within 1.5 world units of the
irrigation pump at (1.5, -0.5), and press B to spend one supply permanently.
Unlocked irrigation reduces future crop maturation from 120 to 60 simulation
ticks. Existing crops keep their original deadlines. The pump becomes brighter.

F5 explicitly writes slot `main`; F9 explicitly restores it. There is no automatic
save or load. Both command ticks pause the farm clock and ignore other farming
commands. The normal physics tick still runs; loading restores body position,
sets velocity to zero, and restores cardinal facing. It does not restore the
input latch, Box2D contact manifold, absolute engine clock, Lua VM, or render
queues. Live status fields `save_status`, `load_status`, and `last_error` expose
failures; the SAVE/LOAD HUD bars turn green on successful operations.

`save_namespace` is `persistent_village`. With `--save-dir ROOT`, the slot lives
at `ROOT/persistent_village/main.save.json`. Runtime defaults follow the engine's
platform preference directory; the fixture never chooses its source directory.
Reset returns to the authored farm and leaves disk data intact.

Game schema 1 explicitly stores every one of the 16 crop cells (tilled, growing,
or mature), remaining growth ticks, the four removable wall cells, total harvest,
spendable supplies, the irrigation upgrade, body position and facing. Loading
checks all keys, collection shapes, coordinates, numeric ranges, crop/time
consistency and inventory/upgrade relationships before any live mutation.
It reconstructs growth deadlines relative to the current farm clock and rebuilds
native tile collisions via the ordinary Tilemap pending-edit path. A corrupt,
unsupported or invalid game schema changes only failure-status fields.

Run the integration workload with:

```
python3 tests/agent/agent_savegame_test.py build/ninja-debug/game tests/fixtures/persistent_village
```

The test creates a temporary save root, uses real supervisor/worker processes,
restarts entire processes, verifies 120/60-tick growth and real harvests, walks
through a restored removed wall, checks reset persistence and rejected-load
atomicity, and cleans its temporary saves. Generated BMP geometry is reproduced
by `generate_fixture.py`; no external art or font dependency is used.
