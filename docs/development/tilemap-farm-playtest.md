# Native Tilemap farm fixture

Run from the repository root:

```sh
./build/modern-debug/game --project tests/fixtures/tilemap_farm
```

Use arrow keys to move. `E` plants the brown tilled cell under the player, or
harvests a mature crop. Crops mature after 120 simulation ticks (two seconds at
60 Hz). Hold does not repeat the action: release and press again. Grass does
not accept planting. `Space` removes the wall cell immediately in front of the
player; `R` reloads the initial farm. The HUD counts planted and harvested crops.

The 64×64 map has separate ground and solid layers. The farm occupies cells
8–11 on both axes; the wall lies at x=16, y=8–11. The origin is (-8,-8), so the
player starts over cell (8,8). Atlas tiles and bitmap labels are self-authored
test geometry; their generators and resulting BMPs are included with fixtures.

Automated runtime/protocol tests cover plant→grow→harvest→replant, held input,
invalid ground, real wall collision, wall removal and passage. They also check
deferred edits from collision callbacks and reset. Render tests independently
check exact atlas pixels, transparency, sorting, culling and viewport scaling.
These checks do not establish human playability: a human still needs to assess
the selected-cell feedback, movement/camera comfort, crop readability and controls.

The editor Inspector allows selecting a Tilemap resource and editing an explicit
layer/x/y/tile ID. The separate Map Asset Draft window provides Save, Discard,
Undo and Redo even if the selected actor or current scene no longer has that map.
Unsaved drafts block play, project switching and closing; explicit Discard can
clear the draft even if its source file has disappeared. Gameplay writes affect
live map copies and do not save the authored map. Scene save and map save are
separate operations. The overlay regression exercises preview and reload via
real ImGui drawing; full manual window/shortcut interaction remains a separate
verification step.
