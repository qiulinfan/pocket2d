# Village defense runtime fixture

This small project exercises the same Lua gameplay, Box2D contacts, actor lifecycle,
and fixed simulation steps through graphical play and the JSONL agent interface.
It is a functional engine fixture, not a Village_Rim art proposal or a completed
game migration. The three-wave invasion, defendable altar, and destructible invasion
source are the deliberately bounded gameplay slice.

From the repository root, launch a built runtime:

```sh
build/sdl3-debug/game --project tests/fixtures/village_defense
```

Use arrow keys to move. Hold Space to attack nearby invaders or the purple devil
tree. Walk over the gold supply box to gather it. Press R to reload the scene.
Protect the blue altar through three waves of two invaders, or break the tree to
cancel future invasions. If three invaders reach the altar, the village loses.
The upper HUD shows altar health, tree health, wave progress, and gathered supply;
the bottom panel shows controls and the next-wave countdown. The pale attack ring
shows the attack radius during each strike.

The camera is fixed. Brown paths connect the starting position, altar, and tree;
invaders arrive from the right. The scene has no world-boundary collision and no
navigation obstacles: those systems are outside this fixture. Movement, attack
cooldown, and wave timing use the runtime's 60 Hz simulation steps.

All fixture BMPs are simple project-authored geometric shapes and 5x7 bitmap
letter patterns. They have no dependency on production art, paid generation, or
external fonts. Recreate them with:

```sh
python3 tests/fixtures/village_defense/generate_geometry.py
```

Run the matching automated scenarios through CTest:

```sh
ctest --test-dir build/sdl3-debug -R 'runtime_step|agent_protocol' --output-on-failure
```

The C++ scenario checks input edges and release, real trigger collection, three-wave
victory, tree victory, and collision-driven failure. The JSONL test also checks
read-only observation, structured errors, stable actor IDs, reset, and replay.
These establish runtime integrity. A human still needs to judge the controls,
attack readability, pacing, and clarity of the tiny geometric HUD.

Farming, elemental combat, NPC quests, persistent saves, two-world exploration,
production animation, and the complete Village_Rim content remain unported.
