# Research boundaries

The initial scaffold is deliberately non-invasive.

## Current guarantees

- No network connections.
- No DNS resolution.
- No package installation.
- No external process execution.
- No authentication attempts.
- No MCP tool invocation.
- No automatic consumption of registry entries as executable commands.

## Future measurement requirements

Any future active measurement must use reviewed targets, explicit provenance, strict concurrency and timeout limits, clear scanner identification, an opt-out mechanism, and data minimization. Authentication bypass, brute force, side-effecting MCP calls, and publication of a searchable vulnerable-endpoint index remain out of scope.

Installing or launching an MCP package is execution of untrusted third-party code. Future package inspection must run in disposable isolation with no secrets, no host mounts, bounded CPU/memory/PIDs, and restricted network access.

Legal review is required before broad third-party network measurement. Public availability does not create a universal right to probe a service across jurisdictions.
