# Storage v2 foundation

This branch introduces an additive, non-destructive foundation for reducing the hot catalog cost of static-analysis results.

## Scope

The existing v1 analysis tables remain authoritative. `tools/storage_v2_foundation.py` adds compact v2 read-model tables and SQLite triggers so existing analysis writers dual-write new rows without requiring an analyzer rewrite in the first milestone.

The foundation contains:

- normalized rule definitions keyed by `(ruleset_version, rule_id)`;
- one compact summary row per analysis run;
- per-run/per-rule occurrence counts;
- a compact coverage-summary table intended for portal reads;
- an evidence-manifest boundary that stores locators and digests rather than detailed file inventory in the hot read model;
- bounded, idempotent backfill of existing v1 runs;
- a writer lock compatible with the existing adjacent catalog writer-lock convention.

No existing v1 table or evidence file is deleted or rewritten.

## Why this exists

Production measurements on 2026-08-07 showed approximately 42,796 analysis runs, 4.19 million `analysis_files` rows, 2.15 million `analysis_findings` rows, and 393k `analysis_evidence` rows. The 44 finding rules had invariant severity/title/explanation metadata within each rule, while repeated occurrence rows dominated the database. The public portal should therefore read compact summaries instead of globally aggregating the raw research tables.

## Install on a copy only

During development, run this against a copied test catalog, not the production catalog:

```bash
python3 tools/storage_v2_foundation.py \
  --database /path/to/copied-catalog.sqlite
```

The install is additive. It creates v2 tables and triggers but does not backfill old runs unless requested.

## Bounded backfill

```bash
python3 tools/storage_v2_foundation.py \
  --database /path/to/copied-catalog.sqlite \
  --backfill-batch-size 100
```

Repeat until `backfilled_runs=0`. The backfill selects only v1 runs that do not yet have a v2 run summary.

## Coverage materialization

If the static-analysis scheduler tables are present, refresh the compact coverage row with:

```bash
python3 tools/storage_v2_foundation.py \
  --database /path/to/copied-catalog.sqlite \
  --refresh-coverage
```

Portal cutover is deliberately outside this first change. The next step is to make the scheduler refresh this row after each bounded batch and then switch `/`, `/coverage`, and server-list reads to v2 summaries.

## Evidence boundary

`analysis_v2_evidence_manifests` is intentionally metadata-only. It records a storage kind, locator, bundle/inventory digests, and whether exact artifact bytes are retained. This establishes the boundary needed for later migration from ten loose files per artifact to a compact detailed-evidence bundle and selective external artifact retention.

The first milestone does **not** delete `artifact.tgz`, consolidate existing evidence directories, or move historical rows out of the v1 catalog. Those are later migration steps after equivalence and performance validation.

## Dependencies and licensing

This implementation is new project code using only the Python standard library and SQLite already used by the project. It introduces no third-party package or new license obligation.
