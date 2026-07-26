# Local Registry Explorer schema version 1

## Boundary and dependency

The explorer indexes official registry metadata from an immutable completed
bundle. It performs no network enrichment. It does not determine whether an
MCP server is safe, vulnerable or malicious. Security findings and
longitudinal comparison are planned for later milestones.

The implementation uses the official SQLite C API through the maintained CMake
`SQLite::SQLite3` target. The dependency is SQLite, public domain, for embedded
local indexing and querying. SQLite is actively maintained. Its major concerns
are the normal security considerations for database files and a native
library; it has no restrictive licence. SQLite is supplied by the operating
system and is not vendored.

## Import workflow

`registry index --bundle PATH --database PATH` requires `_SUCCESS`,
`manifest.json`, and `canonical/servers.jsonl`. It calls the same authoritative
full validator as `bundle validate` before opening the database. Validation
streams canonical JSONL with an 8 MiB validation ceiling per record and checks
the bundle artifacts, snapshot SHA-256, canonical normalization and record
hashes, strict identity ordering, raw-page provenance, and manifest counts.

After validation, indexing:

1. opens SQLite and enables `PRAGMA foreign_keys = ON`;
2. begins one `BEGIN IMMEDIATE` transaction;
3. creates or verifies explicit schema version 1;
4. returns an idempotent result if `snapshot_sha256` already exists;
5. inserts the authoritative manifest snapshot row;
6. streams and bounds each canonical line;
7. inserts or reuses an immutable canonical server-version row;
8. inserts ordered official repository, package, argument, environment, and
   remote metadata;
9. links every exact server version to the snapshot;
10. rechecks canonical and relationship counts; and
11. commits.

RAII rollback covers every failure after `BEGIN IMMEDIATE`. Schema creation for
a new database is in the same transaction, so a failed first import does not
leave a visible schema or snapshot. The source bundle is opened read-only.

The default SQLite rollback journal and durability configuration are retained.
WAL is not enabled because this v0.1 workflow has one bounded local writer and
does not need WAL sidecar lifecycle or write-concurrency behavior.

## Schema

`schema_info` contains one singleton row with `schema_version=1`, the creating
program version, and `search_mode` (`fts5` or `like`). Unsupported versions and
missing required tables are rejected; general migrations are not implemented.

`snapshots` preserves the snapshot digest, start/completion timestamps,
registry base URL, collector name/version/commit, bundle version, absolute
source bundle path, page and record counts, unique version count, and import
timestamp.

`server_versions` preserves identifier, exact version string, description,
official Registry status and timestamps, canonical digest, and complete
canonical JSON. Its immutable uniqueness key is
`(server_identifier, server_version, canonical_sha256)`. Import explicitly
rejects a different digest for an identifier/version already in the catalog.

`snapshot_server_versions` is the many-to-many snapshot relationship.
`repositories`, `packages`, `package_arguments`, `package_environment`, and
`remotes` contain only imported official metadata. Ordered declarations carry
zero-based positions and uniqueness constraints. Environment rows store only
name, required flag, and description; values and risk heuristics are absent.
An explicitly empty official `repository` object is preserved as a
relationship with nullable source/URL fields. Package argument declarations
without a literal `value` are preserved in order with SQL `NULL` for
`argument_value`; no value is inferred from a name, hint, or default. The
complete declaration remains in canonical JSON.

HTTP(S) URL scheme, lowercase host, and explicit port are extracted only when
the authority can be parsed without credentials. Repository owner/name are
derived only for simple two-component GitHub or GitLab URLs; otherwise they
remain SQL `NULL`.

Foreign keys use delete restrictions or cascades that prevent orphan rows.
Indexes cover snapshot links, exact identities, status, registry/transport,
package identifier, and repository/remote host filters.

## Search

At schema creation, the explorer explicitly attempts to create an FTS5 virtual
table. With FTS5, prepared `MATCH` queries cover identifier, description,
package identifiers, repository URL, remote URLs, and remote hosts and may use
BM25 relevance. If FTS5 is unavailable, schema creation succeeds in `like`
mode and search uses escaped bound `LIKE` patterns plus normal indexes. No user
value is concatenated into SQL.

Both modes apply identical snapshot and metadata filters, limits, offsets, and
output shapes. Ordering is exact identifier match, identifier prefix,
available FTS relevance, identifier bytes, then version bytes. Search reports
the selected mode in documentation; the schema also records it.

## Limits and output

Index defaults are 500,000 records, 8 MiB per canonical line, and 4 GiB for the
main database. Search defaults to 20 results with a maximum of 500. List
defaults to 50 with a maximum of 1,000. Numeric overflow, zero limits,
contradictory filters, oversized records, and database growth beyond the
configured maximum are errors.

Text is deterministic human-readable output. Summary and show JSON each emit
one JSON value. Search and list JSONL emit one complete object per line.
Progress and phase timings go only to standard error.

## Limitations

Version ordering is bytewise and is not semantic-version ordering. FTS and
fallback modes can differ in relevance ordering because fallback mode has no
text relevance score. Schema migrations, risk findings, snapshot diffs,
changes, web serving, enrichment, package/repository lookups, network checks,
runtime probing, and deployment are outside v0.1.
