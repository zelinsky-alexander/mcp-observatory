# Declared remote runtime observation v1

## Goal

Extend the existing automatic runtime schedule to MCP servers that publish an explicit remote HTTP(S) URL in the Registry catalog.

For each supported `remotes` row, the runtime service:

1. uses the exact declared URL;
2. performs a bounded MCP `initialize` handshake;
3. sends `notifications/initialized`;
4. requests `tools/list`;
5. canonicalizes the full tool definitions;
6. stores a profile-bound remote observation in the authoritative history database;
7. compares it with the previous completed observation for the same declared URL and probe profile;
8. publishes coverage and drift summaries to the hot portal database.

No tool is invoked.

## Scheduling

`run_storage_v2_runtime_batch.sh` still starts the existing local artifact runtime scheduler first. The Storage v2 wrapper then runs a second bounded remote queue before publishing the runtime read model.

Default remote bounds:

- batch size: 5 endpoints;
- remote batch wall clock: 300 seconds;
- request phase timeout: 15 seconds;
- child timeout: 45 seconds;
- maximum attempts: inherited from the existing runtime schedule;
- completed endpoint reprobe: once per 24 hours.

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
- the declared transport is Streamable-HTTP-like;
- the hostname resolves to globally routable addresses.

The probe:

- does not derive alternate paths;
- does not scan ports or addresses;
- does not follow redirects;
- does not accept embedded URL credentials;
- sends no authentication material;
- rejects loopback/private/link-local/reserved destinations;
- uses normal TLS certificate validation;
- limits response bodies to 1 MiB;
- performs discovery only.

HTTP 401/403 is stored as `blocked` rather than a security failure. Connection/time-limit failures are `inconclusive`. Protocol-invalid responses from a reached endpoint are `failed`.

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
```

The first production deployment should begin with a small remote batch and inspect the resulting state distribution before increasing throughput.

## Portal

The portal remains read-only. `/coverage` gains a `Declared remote runtime coverage` panel showing declared, eligible, completed, blocked/authentication, inconclusive, protocol-failed, comparable and drifted counts. There is no public route that starts a remote probe.

## Limitations

- v1 supports Streamable HTTP semantics; legacy SSE endpoint establishment is not implemented.
- OAuth/authenticated endpoints are deliberately recorded as blocked; no credentialed probing is attempted.
- Network probing is performed by the Observatory transport runner. Native Guard remains the local `stdio` sensor until it gains a native TLS/HTTP transport.
- DNS is validated before each connection, but v1 does not pin the socket to the prevalidated address. Deployment should therefore keep the worker/network boundary restrictive; address pinning is a recommended hardening follow-up.
