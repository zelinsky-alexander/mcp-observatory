# Runtime discovery v1

`tools/runtime_discovery.py` performs discovery-only runtime observation for one
exact npm stdio package already present in the Observatory catalog.

## Security phases

1. The host fetches npm metadata, resolves, downloads, and verifies the exact artifact.
2. A disposable container populates an npm cache. This is the only phase with
   registry network access.
3. A second disposable container runs `npm install --offline --ignore-scripts`
   with no network.
4. A third disposable, read-only, non-root container starts the installed package
   under `mcp-native-guard inspect` with no network.
5. The host validates and persists the deterministic inventory. No tool is invoked.

Child output is drained with a fixed memory bound. A timeout terminates the local
process group and force-removes the exact Docker container recorded through its
private cidfile.

The Docker socket, database, and evidence root are never mounted into the runtime
container. The current MVP requires a Linux `mcp-native-guard` binary compatible
with the selected Debian-based Node image; a static or otherwise self-contained
release build is recommended.

## Observe

```bash
python3 tools/runtime_discovery.py observe \
  --database db/local-registry.sqlite \
  --server io.github.aimsise/color-engine-mcp \
  --version 1.0.1 \
  --package color-engine-mcp \
  --guard-binary ../mcp-native-guard/build/release/mcp-native-guard \
  --evidence-root evidence \
  --runtime-image node:22-bookworm-slim
```

The command creates `runtime_observation_runs` and
`runtime_observation_tools` when needed. Evidence is stored below:

```text
evidence/runtime/sha256/<prefix>/<artifact-sha256>/<launch-profile-sha256>/inventory.json
```

Successful stdout is one JSON object containing `status`,
`runtime_observation_run_id`, `artifact_sha256`, `launch_profile_sha256`,
`guard_sha256`, and `tool_count`. The recorded guard identity is the executable's
SHA-256 digest rather than a caller-supplied version label.

## Compare

```bash
python3 tools/runtime_discovery.py compare \
  --database db/local-registry.sqlite \
  --older-run-id 10 \
  --newer-run-id 14
```

The JSON result contains `added`, `removed`, `modified`, and `unchanged` tool
names. A difference is interface drift and is not automatically a security
finding.

## Deliberate limits

- npm only;
- exact version only;
- stdio only;
- an unambiguous package `bin` entry is required;
- lifecycle scripts are never executed;
- tools are never invoked;
- packages that cannot install from the populated offline cache fail closed;
- no claim of safety or maliciousness is produced.
