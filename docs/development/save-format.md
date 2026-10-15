# Gameplay save storage format

Phase 4 stores explicit gameplay data. It does not snapshot the Lua VM, closures,
native object identity, physics contacts, RNG internals or complete scene/editor
state. Each game owns the meaning and validation of its payload. Validate the
whole game schema before applying any live-world mutation.

## File and identity

A slot named `slot1` is stored as `slot1.save.json` in the directory selected by
the host. Namespace and slot identifiers match
`[A-Za-z0-9][A-Za-z0-9_-]{0,63}`. The host requires `save_namespace` in
`game.config`; missing configuration produces `not_configured` without affecting
ordinary gameplay. Default storage uses `SDL_GetPrefPath("Pocket2D", namespace)`.
`--save-dir ROOT` or `POCKET_SAVE_ROOT` selects `ROOT/namespace`. Agent workers
inherit that selection across reset. SaveStore itself is stateless and accepts an
explicit directory; it never chooses the project directory or working directory.

The envelope has exactly three members:

```json
{
  "format_version": 1,
  "schema_version": 3,
  "data": {
    "player_uid": "18446744073709551615",
    "harvested": 12,
    "gate_open": true,
    "crops": [{"x": 4, "y": 2, "stage": 1, "remaining_ticks": 80}]
  }
}
```

`format_version` belongs to the engine. Unsupported integer versions return
`unsupported_format`; malformed envelope fields return `invalid_data`.
`schema_version` is a positive signed C++ `int` owned by the game. The store
preserves newer game versions rather than migrating or rejecting them. A game
must reject a version it does not understand before changing gameplay state.
There is no automatic backup, migration, slot deletion or corruption repair API.

## Supported values and limits

The payload root is an object or array. Nested values may be objects, arrays,
UTF-8 strings, booleans or finite numbers. `null` is unsupported. Object keys are
UTF-8 strings; valid embedded NUL bytes in keys and values survive JSON escaping
and roundtrip. Identifiers and paths do not permit NUL.

Integer tokens and integer DOM values must fit signed 64-bit range, including
`-9223372036854775808` and `9223372036854775807`. The reader checks integer token
range before constructing the DOM, so an oversized integer cannot silently turn
into a rounded floating-point value. Numbers with a decimal point or exponent use
the normal finite double representation. Cross-language identities that may
exceed JavaScript's exact integer range should remain strings.

The shared `SaveStore.h` constants define these inclusive limits:

| Constant | Limit | Counting rule |
| --- | --- | --- |
| `kMaxDepth` | 32 | Payload root is depth 1; each child, including a scalar, increases depth. |
| `kMaxNodes` | 100,000 | Every payload container and scalar counts once; object key text is not a separate node. |
| `kMaxFileBytes` | 4 MiB | Entire encoded envelope, including escaped strings and whitespace on read. |

Duplicate object keys, unknown envelope members, invalid UTF-8, trailing non-JSON
bytes, unsupported values and limits are rejected. Read uses bounded chunks and a
SAX structural guard before DOM allocation. Write validates the DOM first and
uses a capped output buffer; escaped output cannot grow beyond the file limit.

Lua tables use raw iteration without invoking `__pairs` or `__index`. The host
rejects cycles, sparse or mixed-key arrays, functions, userdata and threads.
Shared references duplicate data rather than preserving identity. Empty Lua
tables serialize as objects. An empty JSON array loads as an empty Lua table and
may save again as an object; game schemas must tolerate that empty shape or store
an explicit shape discriminator. Detailed host tests belong to
[save_lua_test.cpp](../../tests/save_lua_test.cpp).

## Replacement and failure behavior

Write performs validation and bounded encoding before creating anything in the
slot directory. It then creates an exclusive temporary file in the same directory
with a timer and counter suffix; exclusivity, not secrecy,
protects ownership of that temporary filename. On POSIX it uses mode `0600`.
It writes all bytes, checks the result, calls `fsync` on POSIX or `_commit` on
Windows, and checks close before calling `SDL_RenamePath` to replace the target.
Current SDL3 implements that operation with POSIX `rename` or Windows
`MoveFileExW(..., MOVEFILE_REPLACE_EXISTING)`. There is no copy/truncate fallback.

Validation failure, temporary-file write/sync/close failure and failed replacement
leave previous valid slot bytes intact. The writer attempts to remove only its
own temporary file on failure. A successful replacement is the commit point;
later errors are not reported as if the previous file could be rolled back.
The implementation does not synchronize the containing directory after rename,
and file sync is not a universal power-loss guarantee. Abrupt termination may
leave a temporary file. It is not automatically deleted during a later read.
Concurrent writers have last-successful-replacement semantics, without locking
or conflict detection. Synchronous I/O can block on the underlying filesystem.

Existing slot symlinks, dangling slot symlinks, non-regular slot targets and a
symlink at the configured directory itself are rejected. The directory is
canonicalized, including trusted ancestor aliases such as macOS `/var`.
Destination checks are repeated immediately before replacement, but are not a
defense against an attacker replacing directories or files concurrently. This is
not a filesystem sandbox; the caller controls and trusts its storage root.

## Results and evidence

`Write` returns `Status {ok, code, message}`. `Read` returns that status plus
`schema_version` and an owned RapidJSON document. A failed read exposes no partial
payload. Both use structured results for ordinary validation and filesystem
errors; neither terminates the process. Read never modifies storage or gameplay.

| Code | Meaning |
| --- | --- |
| `invalid_argument` | Bad slot/directory argument or non-positive write schema version. |
| `not_configured` | Host has no valid save namespace; not emitted by the stateless store. |
| `not_found` | Requested slot does not exist. |
| `invalid_data` | Malformed envelope/payload, unsupported value or exceeded limit. |
| `unsupported_format` | Engine format version is not supported. |
| `io_error` | Filesystem/open/write/sync/replace failure or unsafe target type. |

[SaveStore tests](../../tests/save_store_test.cpp) exercise exact signed64,
UTF-8/NUL and nested roundtrip; exact depth/node/file-size boundaries; malformed,
duplicate, future and oversized input; slot confinement and symlinks; successful
replacement; and preserved old bytes on failure. POSIX tests use `RLIMIT_FSIZE`
to trigger a real kernel write failure. macOS additionally uses an owner-set
immutable file flag to trigger a real rename failure while allowing temporary
file creation and cleanup. Platform-specific checks report a skip where the
required OS capability is absent. Windows failure behavior and hardware power
loss have not been independently validated by these macOS runs.

See [savegame-slice.md](savegame-slice.md) for gameplay acceptance criteria and
[status.md](status.md) for the final integrated validation matrix.
