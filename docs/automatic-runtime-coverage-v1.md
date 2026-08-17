# Automatic runtime coverage v1

This milestone automatically drains discovery-only runtime work for exact npm
`stdio` package records. It reuses the existing restricted runtime pipeline and
`mcp-native-guard inspect`; it does not invoke MCP tools and does not classify a
server as safe or malicious.

## Flow

```text
successful Registry refresh / hourly timer
  -> synchronize eligible package records
  -> claim one bounded item
  -> exact npm artifact acquisition and integrity verification
  -> networked cache preparation
  -> offline install with lifecycle scripts disabled
  -> read-only, non-root, network-disabled runtime container
  -> mcp-native-guard inspect
  -> canonical inventory persistence
  -> previous compatible version comparison
  -> publish runtime read model to Storage v2 hot catalog
  -> portal coverage and drift views
```

## Eligibility

Version 1 records every package as exactly one scheduler state for the selected
runtime profile.

Eligible:

- npm ecosystem;
- `stdio` transport;
- non-empty package identifier;
- exact package version.

Further runtime constraints are resolved during the bounded attempt. A package
without an unambiguous executable is recorded as `unsupported`; artifact,
installation, protocol, timeout, and other failures remain explicit and never become
successful observations.

## Scheduler profile

The scheduler profile binds:

- scheduler version;
- exact SHA-256 of the Native Guard executable;
- runtime image reference;
- SHA-256 of `profiles/observatory-discovery-v1.json` from `mcp-native-guard`;
- SHA-256 of `tools/runtime_discovery.py`.

Changing any of these creates a new profile and therefore new coverage work. The
runtime observation itself retains artifact SHA-256, launch-profile SHA-256, Guard
SHA-256, sandbox image, and canonical inventory SHA-256.

The current milestone records the runtime image reference rather than resolving an
OCI digest. Binding the immutable image digest is a follow-up hardening step and
should be completed before stronger reproducibility claims.

## State model

```text
eligible -> running -> completed
                    -> failed
                    -> unsupported
                    -> unresolvable
```

The scheduler tracks attempt count, bounded reason code/message, observation run,
artifact and launch-profile digests, previous compatible run, and drift counts.
Stale `running` rows become explicit `failed/interrupted` states.

Retries are bounded by attempt count and retry delay. One non-blocking scheduler
lock prevents concurrent bulk schedulers against the same authoritative database.
The existing catalog writer lock protects each state transition and result
verification.

## Longitudinal comparison

After a successful observation, the scheduler selects the most recent completed
observation for a different server version that:

- was produced under the same scheduler profile;
- has the same logical Registry server identifier;
- has the same package identifier.

Canonical tool definitions are compared by exact tool name:

- `added`;
- `removed`;
- `modified` when the complete canonical tool object changed;
- `unchanged`.

The scheduler stores only comparison counts in its read model. Canonical definitions
remain in `runtime_observation_tools`. Drift is an observation of interface change,
not a security finding.

## Storage v2

`tools/bulk_runtime_discovery_v2.py` runs the scheduler against the authoritative
history database, publishes the normal Storage v2 summaries, then mirrors the
runtime observation/scheduler read model to the hot catalog with
`tools/storage_v2_runtime_publish.py`.

The hot portal database contains no new execution capability. Public routes remain
read-only.

## Run one bounded batch

```bash
MCPO_NATIVE_GUARD_ROOT=/opt/mcp-native-guard/current \
MCPO_RUNTIME_BATCH_SIZE=10 \
bash scripts/run_storage_v2_runtime_batch.sh
```

Important defaults:

```text
history: /var/lib/mcp-observatory-v2/history/assurance-history.sqlite
hot:     /var/lib/mcp-observatory-v2/catalog/local-registry.sqlite
evidence:/var/lib/mcp-observatory-v2/runtime-evidence
Guard:   /opt/mcp-native-guard/current/build/release/mcp-native-guard
profile: /opt/mcp-native-guard/current/profiles/observatory-discovery-v1.json
image:   node:22-bookworm-slim
```

## Scheduling

Examples are provided for:

- `mcp-observatory-v2-runtime-discovery.service`;
- an hourly `mcp-observatory-v2-runtime-discovery.timer`;
- an `OnSuccess` drop-in for `mcp-observatory-v2-refresh.service`.

The service requires Docker access. Docker daemon access is host-root-equivalent;
this remains an accepted research-MVP risk until the later disposable-worker
milestone moves hostile execution off the public/catalog host.

## Assurance statement

A completed runtime observation supports this bounded statement:

> For this exact artifact, launch profile, Guard executable, discovery profile, and
> sandbox image, the server initialized and exposed the recorded canonical MCP tool
> interface.

It does not prove absence of malicious behavior and does not exercise tool effects.
