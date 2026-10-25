# Versioned gameplay saves: implementation contract

2026-10-15 phase 4. Implement explicit gameplay-data persistence and real
fresh-process recovery. This does not serialize Lua VM/closures, arbitrary native
objects, complete Box2D contacts, RNG internals, or an editor scene document.

## Identity and location

Games that use saves declare `save_namespace` in game.config. It and slot names
match `[A-Za-z0-9][A-Za-z0-9_-]{0,63}`; missing namespace makes Save calls return
not_configured. Existing games remain runnable without persistence configuration.
Normal default directory comes from SDL_GetPrefPath("Pocket2D", namespace).
`--save-dir ROOT` selects ROOT/namespace for runtime/agent tests and deployment;
POCKET_SAVE_ROOT supplies the same process-local override to runtime/editor.
Agent child workers inherit the explicit root, including reset. No source folder
or current-working-directory default and no recursive slot-name search.

## Store and file contract

New shared namespace SaveStore, src/shared/persistence/SaveStore.h and matching
src/shared/persistence/SaveStore.cpp. No new dependency; use existing RapidJSON.

Status {bool ok; string code; string message}; ReadResult {Status status;
int schema_version=0; rapidjson::Document data}. Free functions:
IsValidIdentifier(string)->bool;
Write(directory,slot,int schema_version,const rapidjson::Value& data)->Status;
Read(directory,slot)->ReadResult.

File SLOT.save.json envelope is exactly
`{"format_version":1,"schema_version":1,"data":{...}}`. format_version is owned
by engine; schema_version is a positive int owned by the game. Unknown engine
format returns unsupported_format; the game handles its own newer schema.
Write/read validate depth<=32, <=100000 payload nodes and <=4MiB complete encoded
file, strict UTF8, duplicate keys, finite numbers and signed64 integer range.
Root data is object/array, nested values are object/array/string/bool/number;
null is unsupported in this Lua data model. Reject malformed/unknown envelope
members. JSON arrays must be dense. Empty Lua table serializes as object; empty
JSON array loads to an empty Lua table and may re-save as object (game schemas
must treat those as the same empty collection or specify their shape separately).

Write fully validates and encodes before touching the current slot. Create an
exclusive temporary file in the same directory, write/check/flush/close, then
atomic replace. Prior valid bytes survive validation/write/rename failure; clean
own temp on failure. Use file sync where platform available, accurately document
power-loss/directory-sync limits rather than claim impossible rollback after
successful rename. Refuse path traversal and unsafe existing symlink targets;
do not claim an adversarial-filesystem sandbox. Read is bounded even if the file
changes while reading. No automatic deletion, migration or backup overwrite API.

Status codes: invalid_argument, not_configured (host only), not_found,
invalid_data (including size/UTF8/depth limits), unsupported_format, io_error.
Return useful message; no process exit, silent overwrite fallback or raw exception
for ordinary file/validation failures. No tracked save files/run logs/checksums.

## Lua host

Save.Write(slot, schema_version, data) returns `{ok=true}` or
`{ok=false,code=...,error=...}`. Save.Read(slot) returns
`{ok=true,schema_version=...,data=...}` or the same error shape. `Read` never mutates
the world. Tables serialize via raw iteration without executing __pairs/__index
or other metamethods; reject cycles, sparse/mixed/nonnumeric integer keys, function,
userdata and thread values. Shared table aliases duplicate data, not identity.
Integers preserve signed64 values; identities meant for TS should remain strings.

Store namespace is configured before component top-level scripts run, resets with
the project, and never retains the previous project's directory on failed config.
All conversion/I/O remains on the owner thread; bounded atomic synchronous saves
are the first real path. Async background writes require a future immutable-byte
ownership/cancellation design and must not capture LuaRefs.

## Playable persistence workload and tests

Extend copies of the existing farming/defense fixtures in an owned persistent
fixture (no Unity art). At minimum preserve actual map edits/wall removal, crop
stages and remaining growth ticks, harvest/player state and a meaningful permanent
world flag. F5 saves, F9 loads, observed status fields report success/failure.
Validate every game-schema field before applying any live mutation. Bad/unsupported
save must preserve the live world. Loading reconstructs explicitly declared state;
input/transient attacks/velocity may reset if documented, never claim arbitrary
checkpoint equivalence. Fresh process continues growing from saved remaining time.

Tests: exact nested/integer roundtrip, invalid values and metamethod nonexecution,
limits, namespace/slot confinement, corrupt/truncated/future format/schema,
failed replacement preserving previous bytes, project isolation, reset persistence,
new-process restore then real growth/harvest/wall passage, and wrong game schema
without partial mutation. Use independent temporary save roots. Run normal suites,
Release and ASan/UBSan. No flaky power-cut simulation or false fsync guarantees.

## Ownership

Resource worker owns SaveStore header/cpp, tests/engine/save_store_test.cpp and storage
format documentation. QA owns tests/fixtures/persistent_village/** and
 tests/agent/agent_savegame_test.py (may initially build a focused persistent farm plus
explicit permanent-world progress; report exact gameplay coverage). Native worker
independently audits/fixes older Lua getter/callback reentry boundaries in scripting
manager/Lifecycle/Binding/Runtime/Queries/Shared and EventBus with dedicated tests;
root owns all CMake, GameConfig, Engine, runtime/bootstrap/supervisor integration,
new SaveAPI.cpp and Lua table conversion tests. No cross-owner edits without
coordination. Preserve all existing work and no commit/push.
