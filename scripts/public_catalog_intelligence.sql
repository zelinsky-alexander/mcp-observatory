-- Public catalog intelligence queries for MCP Observatory.
--
-- This file contains read-only, bounded queries over the existing schema.
-- It intentionally does not expose evidence paths, refresh runtime paths,
-- worker state, or arbitrary user-supplied SQL.

-- Latest and previous completed snapshots.
WITH ordered AS (
    SELECT id, completed_at, started_at, pages, records_received,
           unique_server_versions, snapshot_sha256,
           ROW_NUMBER() OVER (
               ORDER BY completed_at COLLATE BINARY DESC, id DESC
           ) AS ordinal
    FROM snapshots
)
SELECT id, completed_at, started_at, pages, records_received,
       unique_server_versions, substr(snapshot_sha256, 1, 16) AS sha256_prefix,
       ordinal
FROM ordered
WHERE ordinal <= 2
ORDER BY ordinal;

-- Additions in the latest snapshot relative to the previous snapshot.
WITH ordered AS (
    SELECT id,
           ROW_NUMBER() OVER (
               ORDER BY completed_at COLLATE BINARY DESC, id DESC
           ) AS ordinal
    FROM snapshots
), pair AS (
    SELECT MAX(CASE WHEN ordinal = 1 THEN id END) AS latest_id,
           MAX(CASE WHEN ordinal = 2 THEN id END) AS previous_id
    FROM ordered
)
SELECT sv.id, sv.server_identifier, sv.server_version,
       sv.registry_status, sv.published_at, sv.updated_at,
       substr(sv.canonical_sha256, 1, 16) AS canonical_sha256_prefix
FROM pair
JOIN snapshot_server_versions current_link
  ON current_link.snapshot_id = pair.latest_id
JOIN server_versions sv ON sv.id = current_link.server_version_id
LEFT JOIN snapshot_server_versions previous_link
  ON previous_link.snapshot_id = pair.previous_id
 AND previous_link.server_version_id = current_link.server_version_id
WHERE pair.previous_id IS NOT NULL
  AND previous_link.server_version_id IS NULL
ORDER BY sv.server_identifier COLLATE BINARY,
         sv.server_version COLLATE BINARY
LIMIT 500;

-- Removals from the latest snapshot relative to the previous snapshot.
WITH ordered AS (
    SELECT id,
           ROW_NUMBER() OVER (
               ORDER BY completed_at COLLATE BINARY DESC, id DESC
           ) AS ordinal
    FROM snapshots
), pair AS (
    SELECT MAX(CASE WHEN ordinal = 1 THEN id END) AS latest_id,
           MAX(CASE WHEN ordinal = 2 THEN id END) AS previous_id
    FROM ordered
)
SELECT sv.id, sv.server_identifier, sv.server_version,
       sv.registry_status, sv.published_at, sv.updated_at,
       substr(sv.canonical_sha256, 1, 16) AS canonical_sha256_prefix
FROM pair
JOIN snapshot_server_versions previous_link
  ON previous_link.snapshot_id = pair.previous_id
JOIN server_versions sv ON sv.id = previous_link.server_version_id
LEFT JOIN snapshot_server_versions current_link
  ON current_link.snapshot_id = pair.latest_id
 AND current_link.server_version_id = previous_link.server_version_id
WHERE pair.latest_id IS NOT NULL
  AND current_link.server_version_id IS NULL
ORDER BY sv.server_identifier COLLATE BINARY,
         sv.server_version COLLATE BINARY
LIMIT 500;

-- Static-analysis coverage by exact package record.
SELECT
    COUNT(*) AS package_records,
    SUM(EXISTS(
        SELECT 1 FROM analysis_runs ar
        WHERE ar.package_id = p.id AND ar.status = 'completed'
    )) AS analyzed_package_records,
    SUM(EXISTS(
        SELECT 1 FROM analysis_runs ar
        WHERE ar.package_id = p.id AND ar.status = 'failed'
    )) AS failed_package_records,
    SUM(NOT EXISTS(
        SELECT 1 FROM analysis_runs ar
        WHERE ar.package_id = p.id
    )) AS never_analyzed_package_records
FROM packages p;
