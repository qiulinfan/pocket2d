# Phase 6: native inventory and a playable ownership loop

Implementation is local and uncommitted. The overall modernization goal remains
active. This slice implements reusable inventory rules and a representative game
workflow; it does not migrate all Village_Rim content or certify SOTA performance.

## Delivered boundaries

- `shared/inventory/ItemCatalog` loads strict, bounded, immutable version 1 item
  definitions with exact IDs and resource paths. Effective stack limits and typed
  food/weapon/armor data avoid contradictory stackability flags.
- `shared/inventory/InventoryModel` owns 1..100 slots and optional per-slot
  equipment filters. Add reports accepted/remainder amounts. Exact remove,
  targeted merge/swap, split, no-swap transfer and group restore preserve counts
  and unique instance IDs. Candidate validation precedes every commit; revisions
  increment only for changed containers. The generated 5000-operation test checks
  conservation across two 100-slot models as well as failed/no-op revisions.
- `InventoryAPI.cpp` exposes catalogs and containers as Lua-owned native values.
  Containers share immutable rules and survive disposal of the catalog handle.
  Shared ownership, type/lifetime checks and copied queries prevent dangling
  slot views. Restore parses raw dense tables without executing user getters.
  Actor registration or a frame callback is unnecessary for these values.
- The curated SDK and agent describe contract include nominal catalog/container
  types, copied records and actual instance/static call conventions. The compiler
  suite now has 27 checks. The inventory TS probe is a compile/interoperability
  probe; the full compiled gameplay replay remains the prior typed_gameplay
  workload. Do not call this a complete TypeScript inventory game.
- `inventory_village` uses the source defaults of 30 bag slots, nine hotbar
  references and four equipment slots, plus a 12-slot test chest. Mouse drag is a
  preview until drop; keyboard navigation and cancellation use the same model.
  Partial pickup subtracts exactly the accepted count. Food changes health,
  boots change real movement speed, weapons change target damage and armor changes
  training damage. Geometric PNGs and bitmap labels are original test content.
- Save/load covers exact slots and instance IDs, hotbar mapping/selection, player
  position/health, target health, world remainder and consumed items. Complete
  payload validation and required world-object preparation happen before native
  inventory/scalar commits. Failed candidate preparation is cleaned up. F5/F9
  pause movement for that tick. Fresh-process restoration still affects gameplay.

## Verification and limitations

The core suite adds item_catalog, inventory_model, inventory_lua,
inventory_render_software and agent_inventory. The inventory render test performs
actual pixel readback after keyboard equip and mouse drop, verifies the requested
backend, and checks player visibility and synchronized bag/equipment/hotbar icons.
Debug and Release software, Metal and OpenGL runs pass on this Mac. All 43 CTest
cases pass in Debug, Release and ASan/UBSan (leak detection disabled because local
macOS LeakSanitizer is unsupported). The core has 41 cases plus two optional
TypeScript checks. A final stronger rendered-success-label assertion passes
separately on all three builds; no runtime code changed after the full matrix.
A copy of all 61 source-visible fixture files launches and captures successfully
without ignored editor metadata. Captures and logs remain under ignored
build/phase6-* directories.

The original Unity source's PlaceSmart first fills every matching target stack,
then considers the clicked slot; neither targeted Move nor TransferAll claims that
combined atomic behavior. Splitting is a new feature. Original pickup duplication,
quick-loot loss, asymmetric gear swaps and dropped instance IDs are prevented,
not copied. No Unity save converter, original asset import, global instance
registry, item editor or responsive UI system is included. Containers in one
transaction must share the same opened catalog. Native allocations are outside
the Lua allocator quota. There are no new performance ranking claims.

Gameplay input integrity and pixels do not establish human usability. Follow
[inventory-playtest.md](../inventory-playtest.md) for manual feel and layout review.
The next engine work should address transactional script module reload and the
remaining maintained-binding evaluation, while subsequent gameplay slices connect
inventory to farming, drops, combat and persistent multi-world progress.

Contracts: [inventory-slice.md](../inventory-slice.md),
[Lua inventory API](../../lua-api/inventory.md),
[migration matrix](../village-rim-migration-matrix.md).
