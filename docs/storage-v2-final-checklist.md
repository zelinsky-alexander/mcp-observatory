# Storage v2 pre-PR checklist

- Side-by-side paths only; live catalog/evidence paths are source-only.
- Hot catalog excludes bulk v1 detail rows.
- History/control DB retains full detail.
- Coverage reconciliation counts reused canonical analysis runs once for finding totals.
- Registry refresh operates on isolated v2 hot catalog and syncs identities to history.
- Static batch writes history + separate evidence, then publishes summaries.
- Runtime observation reuses bounded Native Guard inspect path and publishes summaries.
- No automatic legacy cleanup or production cutover is included.
