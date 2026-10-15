/** Compile-time host interop probe; runtime behavior is tested by the Lua contract test. */
export function runInventoryProbe(this: void): InventoryResult {
    const catalog: ItemCatalog = Inventory.Open("contract.items.json");
    const bag: InventoryContainer = catalog.Create(4);
    const chest = catalog.Create(3, undefined);
    const equipment = catalog.Create(1, ["subweapon"]);
    const item: ItemDefinition | undefined = catalog.GetItem("wood");
    if (item !== undefined) {
        // Definitions are mutable copies, not live catalog setters.
        item.display_name = "Copy";
        item.attack = 0;
        item.move_speed_bonus = 0;
    }
    const capacity: number = bag.GetCapacity();
    const revision: number = bag.GetRevision();
    const added: InventoryResult = bag.Add("wood", 8);
    if (!added.ok || added.remaining !== 0 || capacity !== 4 || revision !== 0) return added;
    const slot: InventorySlot = bag.GetSlot(1); // Native slot arguments are 1-based.
    slot.count = 1; // A copy: applying it requires explicit RestoreGroup validation.
    const slots: InventorySlot[] = bag.Snapshot();
    const first: InventorySlot = slots[0]; // TS array indexing is translated to Lua's array convention.
    first.count = 8;
    bag.Split(1, chest, 1, 2);
    bag.Move(1, chest, 1, 1, false);
    chest.Move(1, bag, 2, 1);
    chest.Move(1, bag, 1);
    chest.Move(1, bag, 1, undefined, false);
    bag.Remove(1, 1);
    bag.Add("wood", 1, undefined);
    bag.Add("sword", 1, "sword-instance");
    bag.TransferAll(2, chest);
    const entries: InventoryRestoreEntry[] = [
        {inventory: bag, slots},
        {inventory: equipment, slots: equipment.Snapshot()},
    ];
    return Inventory.RestoreGroup(entries);
}
