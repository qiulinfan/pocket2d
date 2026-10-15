# Implement one Lua gameplay component: altar repair

You are evaluating Pocket2D's agent-native gameplay workflow. Complete this task
using only the copied project, API.md and the local runtime wrapper supplied here.
Do not inspect credentials, personal folders, other repositories, or engine source.
Do not install software, use the network, spawn other agents, or change any runtime
configuration. The engine executable is provided only for execution.

Your deliverable is `project/component_types/Repair.lua`. This file is already
attached to the player as component key `repair`. You may edit only that file.
You may create your own test scripts and copied test projects under `scratch/`;
do not modify the original other project files, API.md, TASK.md or run_game.py.
No changes outside this workspace are allowed. Work for at most five minutes.

The mechanic:

- The player repairs the altar on **E key down**, once per new press.
- Repair is allowed only when the player's Rigidbody position is at Euclidean
  distance **at most 1.25 world units** from the altar's Rigidbody position.
- The actor named `village` has a `Village` component. It owns `gathered`, `health`,
  and `state`. Repair requires `gathered >= 1`, `0 < health < 3`, and `state` is
  neither `won` nor `lost`.
- One valid press consumes exactly one `gathered` and adds exactly one `health`.
  Holding E across multiple simulation steps must not repeat the action. A later
  release and new press may repair again if all conditions still hold.
- Far away, no resource, full/dead altar, won/lost states, and no E press must leave
  both counters unchanged. Use the real Lua API; do not change the engine or
  implement a separate simulator. You can use OnStart and/or OnUpdate.

Read API.md. Implement the component, then actually run the local game JSONL
interface to verify it. `run_game.py` forwards stdin/stdout to the supplied runtime:

```sh
python3 run_game.py <<'JSONL'
{"cmd":"observe"}
{"cmd":"step","frames":1,"keys":["e"]}
{"cmd":"step","frames":10,"keys":["e"]}
{"cmd":"quit"}
JSONL
```

The supplied copied scene starts the player within range with health 1 and two
supplies, and delays invasion spawning so this isolated mechanic can be tested.
For additional cases, copy `project/` under `scratch/` and modify the copied
`scenes/defense.scene`; run `python3 run_game.py --project scratch/YOUR_COPY`.
Do not modify the authoritative source project or validator. Independent tests
will use pristine copies, so changes to the other project scripts do not count.

At the end, briefly report the implementation and the real commands you ran,
including any failures. Do not claim tests passed without running them.
