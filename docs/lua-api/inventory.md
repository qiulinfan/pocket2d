# Native inventory values

`Inventory.Open("village.items.json")` loads an immutable catalog from the current
project's `items/` directory. Open once and create containers from that catalog;
separately opened catalogs are separate rule domains and cannot exchange items.
Invalid resource paths, malformed catalogs and invalid constructor arguments raise
Lua errors. Empty catalogs are valid and can create empty containers.

```lua
local catalog = Inventory.Open("village.items.json")
local bag = catalog:Create(30)
local chest = catalog:Create(12)
local equipment = catalog:Create(4, {"subweapon", "head", "chest", "legs"})
local item = catalog:GetItem("herb") -- owned definition table, or nil
local accepted = bag:Add("herb", 8)
world_pickup_count = world_pickup_count - accepted.moved
local result = bag:Move(1, chest, 1) -- one-based slot indices
```

Containers are native userdata owned by Lua, independent of actors. Keep them in
component instance state and use explicit snapshots for saves. Native operations
run on the owner thread without invoking script callbacks. No inventory update
loop or per-frame polling is required. Returned item/slot tables are independent
copies; changing them does not change the catalog or container.

## Catalog

Version 1 resources use `{ "version": 1, "items": [...] }`. A definition requires
`id`, `display_name`, `kind`, and `max_stack`. Optional fields are `description`,
`equipment_slot`, `health_restore`, `attack`, `defense`, `move_speed_bonus`,
`icon`, `tool_power`, `places` and `use`. Kinds:

| Kind | Rules |
| --- | --- |
| `material` | No equipment, combat stats or healing. |
| `food` | Positive `health_restore`; no equipment or combat stats. |
| `weapon` | `subweapon` slot, `max_stack` 1, `attack`; no armor stats. |
| `armor` | `head`/`chest`/`legs` slot, `max_stack` 1, `defense`/`move_speed_bonus`; no attack. |
| `tool` | `max_stack` 1, `tool_power` >= 1 (mining/chopping strength), optional `attack`, no slot or `subweapon`. |
| `placeable` | Stackable; `places` names what it places (a terrain or object the game interprets). |
| `consumable` | Stackable; `use` names the effect it is used up for (for example `summon:eye`); may restore health. |

`icon` is an optional image path relative to `images/` for any item;
`tool_power`, `places` and `use` belong only to their kind. Numeric ranges and
strict parser rules are specified by the executable ItemCatalog validator.
Unknown/duplicate fields and invalid combinations are rejected. Gameplay applies
these values explicitly; the catalog has no implicit game effect.

IDs are case-sensitive ASCII `[A-Za-z0-9][A-Za-z0-9_.-]{0,63}`. Catalogs are bounded
to 4096 items and 4 MiB. Native catalog allocations are not part of the Lua
allocator quota; the agent supervisor's deadline is not an RSS sandbox.

## Queries and mutations

| Call | Result |
| --- | --- |
| `catalog:Create(capacity, filters?)` | New empty container, capacity 1..100. Filters omitted/empty or exactly one string per slot; empty string accepts any item. |
| `catalog:GetItem(id)` | Definition copy or nil. |
| `container:GetCapacity()` | Capacity. |
| `container:GetRevision()` | Monotonic integer; increments once per changed transaction. |
| `container:GetSlot(index)` | `{item_id, count, instance_id}`. Empty means `"", 0, ""`. |
| `container:Snapshot()` | Dense array of copied slots, including empties. |
| `container:Add(id, count, instance_id?)` | Fill existing stacks then empty slots. Report actual acceptance. |
| `container:Remove(index, count)` | Remove exact positive count; insufficient count fails without change. |
| `container:Move(source, destination, target, count?, allowSwap?)` | Count omitted/0 means whole stack; allowSwap defaults true. |
| `container:Split(source, destination, target, count)` | Move a positive proper fraction of a stack to an empty slot. |
| `container:TransferAll(source, destination)` | Fill all destination stacks then empty slots; never swap; same container rejected. |
| `Inventory.RestoreGroup(entries)` | Validate all candidate snapshots before committing any. |

Mutations return `{ok, code, moved, remaining, changed}`. `remaining` is the
unaccepted part of this operation's requested quantity, not necessarily the
source slot's final count. A partial Add/Transfer succeeds with a nonzero
remainder; no space fails with `moved=0`. Same-slot Move succeeds without changes.
Identical-value swaps also leave revisions unchanged. Normal failure codes include
invalid_argument, invalid_slot, invalid_count, unknown_item, invalid_instance,
no_space, insufficient_items, slot_filter, stack_limit, duplicate_instance,
capacity_mismatch, duplicate_model, incompatible_catalog, swap_not_allowed,
invalid_split, destination_occupied, same_container and empty_source.

A Move merges if the target has matching stack space. Otherwise a whole move may
swap, including a full matching stack. Both slot filters are checked. Targeted
Move and TransferAll are separate operations; Unity's combined PlaceSmart
(all matching stacks, then clicked-slot swap) is not a dedicated API in this
slice. UI behavior must not claim that combined ordering unless it implements it.

Items with max_stack 1 require a nonempty `instance_id` in the same identifier
syntax. Their quantity is exactly one. Stackable items require empty instance IDs.
IDs survive moves, swaps and saves. Uniqueness is enforced within a container and
across all containers participating in a transaction. The game owns allocation
and world-wide uniqueness; this API is not a global item identity registry.

## Atomic restoration

```lua
local result = Inventory.RestoreGroup({
    {inventory=bag, slots=loaded.bag},
    {inventory=chest, slots=loaded.chest},
    {inventory=equipment, slots=loaded.equipment}
})
if result.ok then
    -- Apply already-validated scalar game state; do not discover errors here.
end
```

Entries and slot arrays are dense raw tables. Every slot has exactly item_id,
count, instance_id. Extra fields, missing fields, sparse/mixed keys, bad counts,
unknown items, filters and duplicate instance IDs fail without mutations.
Parsing does not run table `__index`, `__len` or `__pairs`. Snapshots must match
container capacities. Validate health, pose, hotbar map and other game-owned
fields before RestoreGroup. Hotbar maps reference inventory indices; they do not
own copies of items. The native model does not provide Unity save compatibility.
