# Registry collection bundle version 1

## Purpose and trust boundary

`registry collect` performs bounded HTTPS retrieval of public metadata from a
configured MCP Registry API. It does not authenticate, install packages,
execute packages, invoke MCP tools, probe registry entries, publish data, or
use AWS. Registry JSON remains untrusted input throughout parsing and
validation.

The collector writes a self-contained local evidence bundle. A later publisher
may consume the stable files, but collection and publishing are separate trust
boundaries. The current collector uses a bounded JSON representation per
response so it can preserve unknown fields; page byte size and nesting depth
are capped before or during parsing.

## URL policy

The effective base URL is selected in this order:

1. `--registry-base-url`
2. `MCPO_REGISTRY_BASE_URL`
3. compiled default `https://registry.modelcontextprotocol.io`

The URL is split into scheme, host, effective port, and optional base path.
HTTPS is required except for the exact local hosts `localhost`, `127.0.0.1`,
and `[::1]`. Credentials, fragments, invalid ports, base queries, and schemes
other than HTTP and HTTPS are rejected. The normalized origin is
scheme + lowercase host + effective port. Default ports and a trailing base
slash are removed from the stored normalized base URL.

The list endpoint is `<base-path>/v0.1/servers`; the opaque `metadata.nextCursor`
is percent-encoded into the next request's `cursor` query. Response content
never supplies a pagination host. curl automatic redirects are disabled. Each
redirect is resolved and checked against the original origin before the next
request, and the redirect count is bounded. The configured base path is
preserved.

## Layout and completion

```text
bundle/
├── manifest.json
├── checkpoint.json
├── raw/
│   ├── page-000001.json
│   └── pages.jsonl
├── canonical/
│   └── servers.jsonl
├── diagnostics/
│   └── errors.jsonl
└── _SUCCESS
```

Collection occurs in a uniquely named sibling directory. Existing
destinations are never replaced. Data files are flushed and hashed, the
manifest is written, the bundle is self-validated, and `_SUCCESS` is written
last. A no-replace rename promotes the directory. Failed directories retain a
bounded diagnostic and successfully committed raw pages when safe, but never
appear at the requested successful destination.

## Runtime reliability policy

The default runtime policy is:

```text
request_timeout_seconds=60
stall_timeout_seconds=300
run_timeout_seconds=0
maximum_attempts_per_page=8
retry_initial_seconds=2
retry_maximum_seconds=120
```

`run_timeout_seconds=0` means unlimited total runtime.
`request_timeout_seconds` bounds one HTTP attempt.
`stall_timeout_seconds` detects lack of durable page completion.
Heartbeats prove process liveness only.
Durable progress means a page and compact checkpoint were atomically
committed.

When a positive total runtime is configured, request timeouts and retry waits
are capped by the remaining total time. Unlimited mode creates no total
deadline and reports `run_timeout=unlimited`.

Request timeouts and temporary transport failures are retryable. HTTP 408,
425, 429, 500, 502, 503, and 504 are retryable. Other response validation,
schema, limit, cursor, identity, URL-policy, and persistence failures are not.
Backoff doubles from the configured initial delay and saturates at the
configured maximum without integer overflow. `Retry-After` supports integer
seconds only in this version, only for HTTP 429 and 503, and is capped at the
retry maximum. HTTP-date values and invalid values use normal backoff.

Request and retry heartbeats check both deadlines. A stall retains the partial
bundle with category `collection_stalled`; a positive total deadline uses the
distinct category `total_run_deadline_exhausted`.

With `registry collect --verbose`, the collector writes continuous progress to
standard error and flushes every complete line immediately. Request-wait
heartbeats, retry-backoff heartbeats, and durable completion logs are distinct.
Completion is logged only after checkpoint commit. Phase timing covers
pagination, canonicalization, manifest generation, final validation, atomic
promotion, and total collection time. Output does not include response bodies,
complete cursors, secrets, or environment values.

```bash
mcp-observatory registry collect --output ./official-run \
  --verbose 2>registry-progress.log
```

`raw/pages.jsonl` records the page number, normalized request and effective
URLs, UTC retrieval time, status, content type, byte count, SHA-256, input and
output cursors, redirect count, record count, and raw relative path. It never
records headers, cookies, credentials, or an environment dump. Raw bodies are
unmodified. Compression can later change physical artifact names while the
JSON and JSONL logical formats remain versioned and unchanged.

## Compact checkpoint version 2

`checkpoint.json` stores only the current durable resume head:

```text
checkpoint_version
registry
registry_base_url
completed_pages
completed_records
next_cursor
last_completed_page
last_page_path
last_page_size
last_page_sha256
pages_metadata_path
pages_metadata_size
pages_metadata_sha256
updated_at
status
```

Raw pages and `raw/pages.jsonl` remain the authoritative page history. The
checkpoint does not contain a growing artifact array, so its size remains
approximately constant.

For each page the collector validates the response and limits before writing
the raw page to a same-directory temporary file. It flushes and closes that
file, atomically renames it, atomically replaces `raw/pages.jsonl`, then
atomically replaces `checkpoint.json`. Directory entries are synced on the
supported Linux implementation. Only then is the page reported complete and
the stall timer reset. A checkpoint persistence failure is fatal. A raw page
that exists beyond the checkpoint head is uncommitted and is ignored on
resume.

## Canonical identity and hashes

One identity is the exact pair:

```text
server.name + U+000A + server.version
```

Both values come from the Registry `server` object. Canonical objects use
lexicographically sorted keys and compact JSON. They include record version 1,
logical registry identity `official-mcp`, server name, exact version,
description/repository/packages/remotes when present, the complete original
wrapper (therefore preserving unknown fields), a stable `observed_at`, and
`canonical_sha256`.

`observed_at` is the Registry-managed `updatedAt`, otherwise `publishedAt`.
When neither is a valid fixed UTC timestamp, version 1 uses
`1970-01-01T00:00:00Z` to mean that the registry supplied no stable observation
time. It deliberately does not use collection time: identical registry data
must produce identical canonical bytes.

The record hash covers the canonical object before adding `observed_at` and
`canonical_sha256`. Thus it excludes request/effective URLs, retrieval time,
run ID, bundle path, host, and physical registry base URL. The stable logical
registry identity remains included. The snapshot hash is SHA-256 over the
complete sorted `canonical/servers.jsonl`; it is the complete logical snapshot
hash. Artifact hashes cover each declared raw, JSONL, canonical, and diagnostic
data artifact. The manifest and `_SUCCESS` cannot hash themselves and are
validated structurally instead.

Duplicate identities collapse only when their complete canonical record bytes
are equal. Conflicting duplicates fail the run. `first_seen_at` is absent
because historical ownership belongs to a later storage/index milestone.

## Validation and privacy

`bundle validate` requires `_SUCCESS`, parses the manifest, normalizes its base
URL, verifies every declared path, size, and SHA-256, checks raw metadata
against raw files, recomputes record hashes and the snapshot hash, validates
fixed UTC timestamps, counts records/pages, and requires strict identity
ordering.

Bundles contain public registry metadata and provenance, including repository,
package, and remote metadata supplied by publishers. They may still contain
personal data placed in public metadata. Operators should apply retention,
access, and publication review appropriate to their jurisdiction. No local
credentials or unrelated environment data are collected.

## Legacy checkpoint reconstruction

Legacy interrupted collections may contain only `raw/page-*.json`. The
checkpoint reconstructor enumerates every matching filename, parses its
positive decimal page number, rejects two spellings of the same number, and
requires an exact sequence beginning at page 1. Each bounded body must be a
valid Registry response with a `servers` array and valid metadata. Every page
before another page must provide a non-empty `metadata.nextCursor`; terminal
pages may provide null, empty, or no next cursor. Cursor reuse is rejected.

Some legacy responses contain a `metadata.cursor` field recording their input.
When present, it must exactly equal the preceding page's `nextCursor`.
Official Registry response bodies generally do not echo request cursors. When
that field is absent, the only recoverable input for page N+1 is reconstructed
as page N's `nextCursor`; reconstruction cannot independently prove the
historical request URL. This limitation is explicit in the checkpoint
provenance.

`raw/pages.jsonl` is rebuilt with `reconstructed:true`, normalized request and
effective URLs derived from the configured base URL, and the fixed unknown-time
value `1970-01-01T00:00:00Z`. Actual sizes and SHA-256 values are recomputed.
After the rebuilt page index is flushed, `checkpoint.json` is written to a
unique sibling temporary file and atomically promoted without replacement.
Reconstruction writes compact checkpoint version 2. Current version-1
checkpoints remain readable; unsupported future versions are rejected.
`_SUCCESS` is never created.

`registry collect --resume PARTIAL --output NEW_BUNDLE` validates or creates
the checkpoint, verifies provenance, counts, paths, last-page and page-metadata
sizes and hashes, metadata termination, and cursor continuity, copies only
committed evidence into a new temporary bundle, and continues at
`completed_pages + 1`. Attempt numbering restarts at one. Active stall tracking
starts when resumed network collection begins, not from a timestamp written by
an earlier process. It never promotes the resume directory into a successful
bundle.

## Current limitations

- Linux process facilities and fixed `/usr/bin/curl` and `/usr/bin/openssl`
  executable identities are required. Directory durability sync is implemented
  for the current Linux/WSL workflow; portability to filesystems with different
  rename or directory-sync semantics requires separate review.
- curl and OpenSSL are runtime prerequisites rather than linked libraries.
- JSON numbers retain their valid source spelling; object keys and strings are
  normalized. Registry snapshots are deterministic for identical response
  values.
- The bundle is unsigned and uncompressed.
- Reconstructed legacy provenance cannot recover original retrieval times,
  response headers, redirects, or independently prove unrecorded request
  cursors.
- No historical index, publisher, scheduler, AWS integration, SQLite, package
  execution, semantic analysis, or runtime MCP observation is included.
