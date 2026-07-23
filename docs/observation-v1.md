# Observation format version 1

An observation binds one deterministic MCP inventory to the context in which it was collected. It is the initial file-format boundary between an approved sensor, such as `mcp-native-guard inspect`, and MCP Observatory.

## Required members

- `observation_version`: unsigned integer; version 1 is currently accepted.
- `observed_at`: UTC timestamp in fixed `YYYY-MM-DDTHH:MM:SSZ` form.
- `target_id`: stable research identifier chosen by the target manager.
- `source_type`: provenance class such as `controlled_corpus`, `operator_submission`, or `official_registry`.
- `sensor.name`: producer identity.
- `sensor.version`: producer release or build identity.
- `configuration_profile`: identifier for the observation environment and policy-relevant configuration.
- `inventory`: a complete deterministic version-1 inventory accepted by the inventory reader.

## Identity scope

An observation identifies what the sensor saw under a declared context. It does not prove the identity of a server binary, package publisher, operator, or remote endpoint. Different configuration profiles may legitimately expose different inventories for the same package or endpoint.

## Version-1 restrictions

Security-relevant identity strings must be non-empty, bounded, and unescaped. Duplicate required members fail closed. Unknown members are parsed only within configured nesting limits and are not written to the normalized history record.

The timestamp parser validates the fixed UTC shape and basic numeric ranges. It does not yet perform full calendar validation or establish that the timestamp came from a trusted clock.

## History representation

`ingest-observation` validates the full source document and embedded inventory, then emits one compact JSON object followed by one newline. Unknown source members and insignificant formatting are removed. The embedded deterministic inventory remains the semantic payload.

The current append-only JSONL store is intentionally limited:

- one process should write a given history file at a time;
- no duplicate or ordering check is performed;
- no cryptographic digest or signature is calculated;
- no crash-recovery transaction spans multiple records;
- no retention or compaction policy is implemented.

These limitations keep the first ingestion milestone auditable while the observation contract is still evolving.
