# Inventory slice contract

Status: implemented and verified in the phase 6 checkpoint; not a completed
Village_Rim migration.

The source requirements are Village_Rim's InventoryComponent, InventoryMoveOps,
ChestPlayerTransferService, EquipmentComponent, SlotData, Item subclasses and
InventorySaveManager. Defaults are 30 bag slots, nine hotbar references, and four
equipment slots (subweapon, head, chest, legs). The fixture adds a 12-slot chest.
Splitting stacks is a new capability. Existing partial-pickup duplication,
full-bag quick-loot loss, asymmetric equipment swaps and dropped instance IDs are
regressions to prevent, not behavior to reproduce.

## Ownership and boundaries

An immutable validated ItemCatalog is shared by independently owned native
inventory values. Lua opens a catalog and creates containers; these are userdata,
not actor components. A container has no frame callback, physics dependency or
actor lifetime, so a component adapter would add an unnecessary second ownership
model. Lua components own gameplay references and explicitly snapshot pure data
for Save. Scene reset recreates them. All mutation stays on the Lua owner thread.

The pure C++ model owns 1..100 slots, optional equipment filters, and a revision.
It invokes no Lua callbacks. Operations prepare candidates, validate both sides,
and commit once per changed container. Failed operations and no-ops do not change
revisions. Stack limits are effective limits from the catalog, not a separate
possibly contradictory stackable flag. Nonstackable items require a preserved
instance ID; stackable items cannot carry one. Identity uniqueness is enforced
within a container and the containers participating in a transaction. It is not a
global registry; the game validates all owned containers together when restoring.

## Data and operations

Catalog resources are exact `items/*.items.json` files, version 1, at most 4 MiB
and 4096 definitions. IDs are case-sensitive ASCII identifiers. Definitions have
id, display_name, kind, max_stack, optional description, equipment_slot and typed
health_restore/attack/defense/move_speed_bonus statistics. No icon search or Unity
prefab conversion is implied. Fixture artwork maps known IDs to authored shapes.

Add fills matching stacks then empty slots and reports accepted/remainder counts.
The world subtracts only the accepted count. Remove is exact. Move merges into
compatible space; whole moves can swap otherwise, including a full same-item
stack, matching the source behavior. Swaps validate both destination filters.
TransferAll fills destination stacks and empty slots without swapping. Split
requires an empty destination and an explicit proper fraction of a stack.
Same-slot moves are no-ops. RestoreGroup validates every supplied container and
cross-container identity before any commit. Lua indices are one-based.

Lua surface: Inventory.Open(resource), catalog:GetItem(id), catalog:Create(capacity,
filters?), inventory:GetSlot(index), GetCapacity(), GetRevision(), Snapshot(),
Add(id,count,instance_id?), Remove(index,count), Move(index,destination,index,count?,
allowSwap?), Split(index,destination,index,count), TransferAll(index,destination),
and Inventory.RestoreGroup({{inventory=container,slots=snapshot},...}). Mutations
return {ok,code,moved,remaining,changed}; queries return copied data. Invalid query
arguments raise a Lua error; invalid mutation input returns a failed result.

## End-to-end acceptance

The playable fixture uses real input, UI draw calls and actor motion. Mouse drag
keeps source ownership until drop, rejects stale revisions, and supports cancel;
keyboard controls provide equivalent operations. Hotbar entries reference bag
indices and never duplicate item ownership. Food changes actual health; boots
change measured movement speed; equipment filtering applies at the native model.

Save/load validates the complete game payload before an atomic inventory restore
and scalar-state application. Tests cover conservation (including world remainder
and consumption), partial pickup, full bags, split/merge/swap/cancel, unique IDs,
reverse equipment swaps, malformed late slots, unchanged failed operations,
fresh-process persistence and reset. Rendering must be inspected separately from
headless integrity checks. Human playtesting remains necessary for UI usability.
