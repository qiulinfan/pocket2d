# Phase 2 checkpoint

2026-10-15. This slice implements native Tilemap authoring/gameplay and bounded
agent script execution. The authoritative completed validation matrix and next
work are in [status.md](status.md); the full modernization goal remains active.

Implemented:

- Two-layer Tilemap resources, frame-zero cell queries, 32×32 CPU chunks,
  viewport culling, deferred cell edits and row-run Box2D walls. Scene preflight
  validates data, origin/extents and render scale before replacing the old world.
- A 64×64 farm fixture exercises real movement, planting/growth/harvest, wall
  removal and passage. Agent observations can query bounded committed cells.
- TilemapDocument, Inspector and global Map Asset Draft controls support
  preview, save, undo/redo and explicit memory discard. Drafts remain accessible
  after selection/scene changes; fresh runtime instances do not receive stale
  clean cached data. Multiple live references receive authored previews.
- Lua allocator/count-hook budgets and an independent SDL3 worker supervisor
  bound requests, validate correlated responses, terminate failed workers and
  recover via fresh-process reset. Lua heap limits are not process RAM limits;
  this trusted local host is not a filesystem/network sandbox.
- Direct component destruction during locked physics callbacks is rejected
  before touching fixture metadata. Deferred removal/destruction remains valid.
  Map activation is committed consistently to rendering and all collision bodies,
  even if EndContact writes the desired activation state during that commit.

Source/tests/docs are local and uncommitted; no push or Unity migration occurred.
Logs, PNGs, benchmarks and external-agent traces stay in ignored `build/`.
Both fixtures include their reproducible self-authored BMP geometry rather than
depending on ignored local art or editor metadata.

Usage: [Tilemap API](../lua-api/tilemap.md), [farm playtest](tilemap-farm-playtest.md),
[agent protocol](agent-protocol.md), [script budgets](script-budgets.md),
[performance comparison](tilemap-performance.md), [build commands](build-and-test.md).
