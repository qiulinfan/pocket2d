# Inventory Village fixture and playtest

This is the phase 6 runnable inventory acceptance scene. It exercises the native
item catalog/container model through real Lua, SDL input, Box2D movement and UI
image rendering. It does not claim that Village_Rim's entire inventory UI,
crafting, equipment art, altar interaction, or combat has been migrated. The
source game's partial-pickup duplication and asymmetric swaps are not retained.

## Run and controls

```
build/sdl3-debug/game --project tests/fixtures/inventory_village --save-dir build/inventory-playtest-saves
python3 tests/agent/agent_inventory_test.py build/sdl3-debug/game tests/fixtures/inventory_village
```

The window is fixed at 960×720. The top strip contains the real player, training
target and herb pickup. The lower panels contain a 30-slot bag (6×5), 12-slot
chest (4×3), equipment slots HAND/HEAD/BODY/LEGS and nine hotbar references.
Equipment filters are respectively subweapon, head, chest and legs.

| Input | Behavior |
| --- | --- |
| WASD | Move the real Rigidbody; dragging pauses movement. |
| Left-button drag | Preview a whole stack, commit on release over a valid slot. |
| Right-button drag | Preview the rounded-up half; split requires an empty destination. |
| Escape, release outside, same-slot release | Cancel without removing items or advancing inventory revisions. |
| Tab, arrows, Enter | Cycle bag/chest/equipment focus, select a slot, pick/place through the same transaction path. |
| Shift+Enter | Keyboard split preview; Escape cancels. |
| 1–9, hotbar click, mouse wheel | Select a hotbar reference; wheel wraps once per input impulse. |
| Bag drag onto hotbar | Remap that reference, swapping an existing reference when needed; never move owned items. |
| E | Retry the nearby world pile; the initial pickup also uses a real trigger callback. |
| U | Consume selected food if below maximum health, or equip the selected item. |
| Q | Unequip the focused equipment slot by stack/empty-slot transfer; a full bag leaves it equipped. |
| H | Apply training damage: max(1, 2 − defense). |
| Space | Hit the nearby training target using the actual current attack statistic. |
| F5 / F9 | Explicitly save / load slot `main`; both pause movement for the command tick. |

A yellow border indicates keyboard focus; cyan indicates the selected hotbar's
bag reference. A dim source icon and cursor ghost indicate a pending drag. The
source retains ownership until a successful drop. Pickup/use/unequip input is
blocked during a preview; source revision checking additionally rejects a stale
preview if another gameplay callback modifies that native inventory.

## Concrete gameplay and conservation

The bag begins with herb×4, ore×5, one `boots-1`, one empty slot and 26 additional
ore×5 stacks. The chest contains sword/head/body equipment, herb×4 and ore×4. The
world pile contains herb×8. First contact can accept six herbs, leaving two in the
actual world actor. A full-bag retry must accept zero. Depleting the remainder
removes the actor; loading recreates only the saved remainder when necessary.

Herbs restore one health up to three and consume one unit per input edge; holding
U does not repeatedly consume, and full-health use consumes nothing. Boots change
actual movement from two to three world units per 60 simulation ticks. Sword
changes training-target damage from one to three. Helmet/body/boots contribute
four defense when all worn and reduce a training hit from two health to one.

Every observed test step independently decodes all 46 slots and checks these
conservation totals: herbs in containers + world remainder + consumed = 16;
ore = 139; exactly one each of `sword-1`, `helmet-1`, `armor-1`, `boots-1` remains
in the three containers. Gear instances survive swap, equip, unequip and reload.
Hotbar entries are nine distinct bag indices, not extra item ownership.

Transfers target the chosen slot. Same-item moves merge up to capacity; whole
moves may swap; split requires an empty destination. This differs from Unity's
cross-container service, which first fills other compatible stacks before
handling the clicked destination. `TransferAll` supplies stack-first/empty-slot
behavior for unequipping and is separately tested by the native model suite.

## Persistence and failure behavior

The namespace is `inventory_village`; with `--save-dir ROOT`, saves live at
`ROOT/inventory_village/main.save.json`. No save is written into fixture sources.
Schema 1 stores bag/chest/equipment snapshots, all nine hotbar references,
selection, health, target health, player position, world remainder and consumed
herb count. Derived attack/defense/speed are recalculated from equipment.

The loader validates all fields, stack ranges, equipment filters, instance IDs,
hotbar uniqueness and conservation before any commit. If a pickup must be
recreated, it stages and validates that world actor before `RestoreGroup` commits
all three native inventories. Preparation or restore failure cleans up the
candidate, preserves the previous containers/scalars/pose and returns a status.
Successful loading resets movement and drag preview; F5 cancels the preview
without changing item ownership. Runtime input latches, physics contacts and
arbitrary Lua state are not serialized.

## Verification and remaining human checks

The real agent replay covers mouse hold/move/release, cancellation, half split,
partial merge, two-way swaps, equipment filters including reverse-swap rejection,
full-bag unequip, keyboard transactions, reference-only hotbar remapping,
consumption, measured movement and combat statistics. It also checks save bytes,
late-invalid fields, corrupt/future schemas, reset and entirely fresh processes.

Temporary scene variants test a real cooperating auto-heal callback changing the
source revision during drag, missing/malformed pickup templates and a native
restore rejection after candidate preparation. Both the failed frame and next
frame must preserve actor lists and native container revisions. The test-only
`InventoryRevisionProbe` component is absent from the playable scene.

Debug headless replay has passed. Separate renderer checks have exercised the
software, Metal and OpenGL backends; final build-matrix results belong to their
run artifacts. Automated integrity does not establish usability. A human should
still check item recognition, counts and focus visibility, drag/cancel feel,
keyboard discoverability, hotbar remapping clarity and the readability of save
failure feedback at their actual display scale.

All PNG geometry, icons and bitmap glyphs are authored for this fixture and can
be reproduced with `tests/fixtures/inventory_village/generate_geometry.py`; no
Unity assets, external fonts or third-party game art are imported.
