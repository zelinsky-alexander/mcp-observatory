# Post-Storage-v2 Portal Support Plan

Branch: `fix/post-v2-portal-support`

This branch starts from current `main` after Storage v2 migration. It exists only for authoritative data-model/publication changes required by the portal bug-fix work.

## Principle

`mcp-observatory` owns authoritative registry history, Storage v2 hot/history publication, static-analysis state, runtime observations, and longitudinal comparison. The portal should not invent new authoritative security state.

## Current portal issues potentially requiring Observatory support

- Review queue: keep aggregate counts in the hot summary; bounded finding detail can be served from history. Add a new hot review projection only if bounded history reads prove too slow or semantically unstable.
- All-observed server browser: current history already contains longitudinal server identities. Prefer bounded history reads first; add a compact server-history projection only if needed for latency.
- Snapshot detail: authoritative snapshot membership/change data must come from Observatory tables. Add compact per-snapshot summaries only if current tables cannot answer bounded pages efficiently.
- Coverage drill-down: predicates must be derived from the same static scheduler state/profile semantics as `analysis_v2_coverage_summary`. If portal queries would duplicate scheduler logic, publish compact detail/index projections here instead.
- Dashboard complete-analysis drill-down: prefer `analysis_v2_run_summaries` in hot storage; history only for deeper detail.

## Runtime discovery

Issue #10 is a separate functional milestone and should not be mixed into the portal bug-fix merge. Its first phase remains discovery-only: automatically schedule eligible exact npm `stdio` artifacts, run `mcp-native-guard inspect` inside the restricted runtime pipeline, persist compatible observations, and expose runtime coverage/tool-definition drift. No arbitrary `tools/call` execution in that phase.

## Guardrails

- Preserve Storage v2 compact-hot / full-history separation.
- Do not reintroduce millions of v1 detail rows into the hot DB.
- Keep publication atomic and validation fail-closed.
- Do not change runtime/security trust boundaries merely to simplify portal queries.
- Any new projection needs deterministic rebuild/publication logic and parity tests against authoritative history/state.

## Decision sequence

1. Implement bounded portal-side history reads where already supported.
2. Measure/query-test against production-shaped Storage v2 fixtures.
3. Add Observatory projections only for demonstrated performance or semantic-consistency gaps.
4. Keep runtime discovery work in its own follow-up implementation series.
