# Research boundaries

The initial scaffold is deliberately non-invasive.

## Current guarantees

- Network access is limited to the explicitly invoked public Registry
  collection command and its validated same-origin redirects.
- No address-range scanning or registry-entry probing.
- No package installation.
- No external process execution except the fixed curl HTTP client and OpenSSL
  SHA-256 command used by registry bundle collection.
- No authentication attempts.
- No MCP tool invocation.
- No automatic consumption of registry entries as executable commands.

## Future measurement requirements

Any future active measurement must use reviewed targets, explicit provenance, strict concurrency and timeout limits, clear scanner identification, an opt-out mechanism, and data minimization. Authentication bypass, brute force, side-effecting MCP calls, and publication of a searchable vulnerable-endpoint index remain out of scope.

Installing or launching an MCP package is execution of untrusted third-party code. Future package inspection must run in disposable isolation with no secrets, no host mounts, bounded CPU/memory/PIDs, and restricted network access.

Legal review is required before broad third-party network measurement. Public availability does not create a universal right to probe a service across jurisdictions.

The registry milestone collects public registry metadata only. It does not
install or execute an MCP package and does not contact the endpoints described
by registry records. Local or AWS publishing remains a separate future
component consuming a validated bundle.
