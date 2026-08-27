# Declared remote runtime hardening review

The feature branch integrates declared remote runtime observation with the existing local exact-artifact runtime scheduler while keeping the two evidence classes independent.

Implemented production hardening:

- connect sockets only to prevalidated globally routable DNS results; HTTPS retains the declared hostname for SNI and certificate verification;
- bound complete `tools/list` pagination to 32 pages, 2048 tools, 1 MiB per response, 4 MiB cumulative response bytes and 4096-byte cursors;
- treat observation-limit exhaustion as inconclusive rather than persisting a partial completed inventory;
- recover stale remote schedule and observation `running` rows as interrupted/inconclusive;
- propagate only a minimal child environment plus explicitly configured `TMPDIR`;
- share one orchestration wall-clock budget across local discovery, remote discovery and publication/verification;
- isolate remote scheduler operational failure so successful local observations still publish and verify;
- retain separate local and remote profiles, observation tables, schedule tables and portal metrics.

No third-party dependency was added.
