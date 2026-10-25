# Phase 12: Lua binding upgrade evidence

The isolated LuaBridge3 experiment is implemented and verified. Production remains
Lua 5.4.9 / LuaBridge 2.7; the full modernization goal remains active. No commits
or pushes were made.

- Four actual production TUs pass baseline compilation and fail candidate compilation
  with 23 diagnostics; all eight header-resolution checks prove version isolation.
- The standalone semantics and budget/error probes pass four CTest cases in each of
  Debug, Release and ASan/UBSan. These are separate from the unchanged production
  65-case Phase 11 matrix.
- Matched Release operation, fresh-object compile and executable-size measurements
  are complete. Gains and costs are workload-specific, not an engine ranking.
- [Detailed results and reproduction](../binding-experiment-results.md) describe the
  actual adaptations, diagnostics, lifecycle/budget evidence and limitations.
- [ADR 007](../adr/adr-007-checked-lua-binding.md) selects full checked-binding integration
  at the fixed VM version as the next implementation. Its production acceptance
  is still gated by complete runtime/typed/render/reload verification.

Tracked changes are standalone tooling and development documents. Candidate source,
raw compiler logs, timings and binaries stay in ignored build/. The full gameplay,
platform packaging and comparative SOTA gates remain open.
