# Phase 3 checkpoint

2026-10-15. The native SpriteAnimation slice is implemented and verified; the
full Pocket2D modernization goal remains active. See status.md for the current
cumulative build matrix, open platform gates and next work.

- Strict bounded version-1 clip resources use exact contained image paths,
  validated atlas grids and shared immutable data. Each component owns its tick
  state. Scene preflight checks configured renderer targets and clip names before
  replacing the current world; invalid resources retain the old scene.
- Integer ticks drive loops and one-shots. Play is idempotent for the selected
  clip, Restart is explicit, pause/disable hold pose, completion fires once,
  Stop restores the original SpriteRenderer and empty resources are inert.
- A transient renderer override preserves authored sprite fields, tint, pivot,
  scale/mirroring and sorting. Target removal/replacement and stale actor or
  component references cannot control a new generation.
- OnAnimationEvent is delivered in component order with playback/identity checks
  around every Lua field lookup and callback. Receiver lists are copied before
  any executable Lua lookup. Protected getter calls reject metadata errors
  without panicking the worker. The regression includes the previously crashing
  __index getter that adds 32 components during callback discovery.
- The playable fixture uses real movement and idle/walk/one-shot attack. Native,
  C++ integration, JSONL and pixel tests cover timing, event reentry, pause,
  readonly fields, clear/restore, reset, malformed assets and live-target rebinding.

Debug, Release and ASan/UBSan each passed the existing 29 tests plus the added
getter-reentry test (30 distinct CTest cases, run as 29 + 1). Leak detection is
unsupported on this Mac and disabled. Software, Metal and OpenGL actual animation
pixel tests passed. A source-visible-only fixture copy launched and captured
without ignored local metadata. Human animation feel is not verified.

The Release update benchmark verifies each final actor state. Initial 10k medians
were 5.09 ms native and 4.95 ms for a minimal Lua loop; no native speedup is claimed.
Native production event/lifetime work remains enabled in that measurement.
See animation-performance.md for constraints and reproducible commands.

Next: explicit versioned gameplay state, atomic save, fresh-process restoration
of map/crop/player/world fields, and malformed/unsupported-save handling. Typed
host declarations, module reload, full editor/platform coverage, representative
cross-engine benchmarks and complete Village_Rim migration remain open.
All work is local and uncommitted; logs, captures and CSV stay under ignored build/.
