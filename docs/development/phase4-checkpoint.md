# Phase 4 checkpoint

2026-10-15. Versioned gameplay persistence and broader Lua host reentry fixes are
implemented. The overall modernization/migration goal remains active.

SaveStore bounds pure JSON data, preserves signed64/UTF8, rejects bad structure
and uses exclusive same-directory temporary files, file sync and native atomic
replacement. Ordinary failures preserve previous bytes. Lua Save.Write/Read
use raw table conversion and structured errors. game.config save_namespace and
--save-dir/POCKET_SAVE_ROOT keep saves separate from source and survive worker
reset. Full format, failure and power-loss limits are in save-format.md.

The persistent_village fixture restores 16 crop cells/remaining growth ticks,
four wall cells, player pose/facing, harvested/supply counts and a permanent
irrigation upgrade that actually changes future growth from 120 to 60 ticks.
The integration test starts four independent agent supervisor processes, grows,
harvests, purchases, removes walls, restores and walks through the saved gap.
Bad formats/schema/late invalid fields preserve live gameplay. This is a defined
persistence workload, not migration of every Village_Rim item/world system.

Older Lua lifecycle/metadata boundaries now use snapshots, protected getters,
identity rechecks and caller-stack restoration. A real OnDestroy-add-components
crash is covered, as are contacts, nested events, self-removal, metadata failure
and forged identities. Normal particle/lifecycle/hierarchy ordering is preserved.
A bounded raw-table prototype lookup avoids protected-call overhead where no Lua
getter executes; functions, userdata and cycles keep protected fallback.

Debug, Release and ASan/UBSan each passed all 34 CTest cases on the final source.
LeakSanitizer remains unsupported on this Mac. Real kernel write and immutable
rename failures preserve old slot bytes; other-platform and power-cut behavior
is not asserted. The source-visible-only fixture launches and captures without
ignored local metadata. No commit/push or Unity source/art migration occurred.

The host hardening initially raised the 10k Lua-loop benchmark median to 8.12 ms;
the prototype fast path reduced it to 5.69 ms with all final actor states equal.
See animation-performance.md for historical comparisons and limitations.
Next work: typed host declarations/diagnostics and a bounded TS authoring test,
transactional module reload, full gameplay content, editor/platform gates and
representative cross-engine benchmarks. All logs/saves/captures remain ignored.
