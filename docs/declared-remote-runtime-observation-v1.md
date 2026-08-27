# Declared remote runtime observation v1

## Goal

Extend the existing automatic runtime schedule to MCP servers that publish an explicit remote HTTP(S) URL in the Registry catalog, while preserving the existing local exact-artifact `stdio` observation path as an independent assurance layer.

For each supported `remotes` row, the runtime service:

1. uses the exact declared URL;
2. resolves the hostname and connects only to a prevalidated globally routable address;
3. performs a bounded MCP `initialize` handshake;
4. sends `notifications/initialized`;
5. requests every bounded `tools/list` page until pagination completes;
6. canonicalizes the complete tool definitions;
7. stores a profile-bound remote observation in the authoritative history database;
8. compares it with the previous completed observation for the same declared URL and probe profile;
9. publishes local and remote coverage/drift summaries to the hot portal database.

No tool is invoked.

## Scheduling and shared service budget

`run_storage_v2_runtime_batch.sh` remains the single runtime timer/service entry point. The Storage v2 wrapper starts the existing local artifact runtime scheduler first and then runs a separate declared-remote queue before publishing both read models.

The phases share one bounded service budget rather than independently consuming the full systemd timeout. Defaults are:

- overall runtime orchestration budget: 3150 seconds;
- publication/verification reserve: 120 seconds;
- configured remote allowance: up to 300 seconds;
- local scheduler receives at most the remaining local allowance after the remote and publication reserves are removed;
- remote scheduler receives at most its configured allowance and is further capped by the actual remaining shared budget.

A remote scheduler operational failure is isolated from the local phase: successful local observations are still published and verified. Publication or verification failures remain fatal because they affect the coherent read model.

Default remote bounds:

- batch size: 5 endpoints;
- remote batch wall clock: 300 seconds, additionally capped by the shared deadline;
- request phase timeout: 15 seconds;
- child timeout: 45 seconds;
- maximum attempts: inherited from the existing runtime schedule;
- completed endpoint reprobe: once per 24 hours;
- stale `running` recovery: inherited from `MCPO_RUNTIME_STALE_RUNNING_AFTER_SECONDS`.

Interrupted remote schedule/observation rows are recovered to `inconclusive` with an explicit interruption reason, so they cannot remain permanently stranded in `running`.

The remote scheduler is keyed by:

```text
scheduler version
+ remote runner SHA-256
+ remote probe profile SHA-256
```

A completed remote is periodically made eligible again so live-only drift can be observed even when the Registry version and distributed package remain unchanged.

## Eligibility and safety boundary

A remote is eligible only when:

- the Registry contains a non-empty exact URL;
- the URL scheme is HTTP or HTTPS;
- the declared transport is Streamable-HTTP-like.

Before each connection the hostname is resolved. Every returned address must be globally routable. The socket then connects only to one of those exact prevalidated addresses; no second DNS lookup is performed by the connection layer. For HTTPS the declared hostname is retained for TLS SNI and certificate verification.

The probe:

- does not derive alternate paths;
- does not scan ports or addresses;
- does not follow redirects;
- does not accept embedded URL credentials;
- sends no authentication material;
- rejects loopback/private/link-local/reserved destinations;
- uses normal TLS certificate validation;
- limits each response body to 1 MiB;
- limits cumulative response bytes to 4 MiB;
- limits `tools/list` to 32 pages and 2048 tools;
- limits pagination cursors to 4096 bytes;
- performs discovery only.

HTTP 401/403 is stored as `blocked` rather than a security failure. Destination-policy rejection is also `blocked`. Connection/time-limit failures and observation-limit exhaustion are `inconclusive`. Protocol-invalid responses from a reached endpoint are `failed`.

## Bounded complete tool inventory

`tools/list` pagination is followed using `nextCursor` until no cursor remains. Every page stays inside the response-size, cumulative-size, page-count, tool-count and cursor-size limits recorded by the remote probe profile. Repeated cursors, invalid cursor types and duplicate tool names are protocol-invalid.

If any observation bound is exceeded, the observation is `inconclusive`; a partial inventory is never persisted as `completed`. Therefore a completed remote observation represents the complete canonical interface within the declared profile bounds.

## Data model

Authoritative history tables:

```text
runtime_remote_observation_runs
runtime_remote_observation_tools
runtime_remote_schedule_profiles
runtime_remote_schedule_current
runtime_remote_schedule_state
```

The observation keeps the declared URL, remote/catalog identities, transport, probe profile, protocol version, bounded server identity, canonical inventory, inventory digest, HTTP status and a digest of the session ID when one is issued. Raw session IDs are not retained.

The schedule state keeps the latest observation plus added/removed/modified/unchanged tool counts relative to the previous compatible live observation.

Local exact-artifact observations remain in the existing local runtime tables and profiles. Remote observations are never presented as equivalent to local package execution evidence.

## Deployment

This branch must be deployed together with the matching Native Guard branch because the runtime script requires:

```text
/opt/mcp-native-guard/current/profiles/observatory-remote-discovery-v1.json
```

The feature uses no new Python package or system dependency.

After deployment, the existing runtime timer/service continues to be the entry point. Optional bounds can be adjusted with:

```bash
MCPO_REMOTE_RUNTIME_BATCH_SIZE=5
MCPO_REMOTE_RUNTIME_MAXIMUM_RUN_SECONDS=300
MCPO_REMOTE_RUNTIME_PHASE_TIMEOUT_SECONDS=15
MCPO_REMOTE_RUNTIME_CHILD_TIMEOUT_SECONDS=45
MCPO_RUNTIME_OVERALL_MAXIMUM_RUN_SECONDS=3150
MCPO_RUNTIME_PUBLICATION_RESERVE_SECONDS=120
```

The first production deployment should begin with a small remote batch and inspect the resulting state distribution before increasing throughput.

## Portal

The portal remains read-only. `/coverage` gains a `Declared remote runtime coverage` panel showing declared, eligible, completed, blocked/authentication, inconclusive, protocol-failed, comparable and drifted counts. There is no public route that starts a remote probe.

## Limitations

- v1 supports Streamable HTTP semantics; legacy SSE endpoint establishment is not implemented.
- OAuth/authenticated endpoints are deliberately recorded as blocked; no credentialed probing is attempted.
- Network probing is performed by the Observatory transport runner. Native Guard remains the local `stdio` sensor until it gains a native TLS/HTTP transport.
- HTTP endpoints can be observed, but unlike HTTPS they do not establish transport authenticity or integrity; this distinction must remain explicit in interpretation.
