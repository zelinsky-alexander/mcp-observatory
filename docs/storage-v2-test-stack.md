# Storage v2 test-stack checklist

Use `storage-v2-foundation` in all three repositories.

The detailed production-side procedure is in the portal repository at `docs/storage-v2-production-sidecar-test.md`.

MVP order:

1. Build/test side-branch Observatory, Native Guard, and portal.
2. Prepare `/var/lib/mcp-observatory-v2` from the live catalog using SQLite backup; do not modify the live database.
3. Backfill compact v2 summaries in the history clone and build/VACUUM a compact hot clone.
4. Start the read-only portal sidecar on `127.0.0.1:8081` and compare route latency/content with production `8080`.
5. Run the isolated compact Registry refresh; synchronize immutable Registry identities hot -> history; synchronize the static schedule without executing packages.
6. Run one then a small bounded static-analysis batch against history. Publish only compact summaries/manifests to hot.
7. Verify content-addressed evidence bundles and confirm `artifact.tgz` and duplicated `analysis-rules.json` are excluded.
8. Run one exact npm `stdio` runtime observation with the side-branch Native Guard binary and publish the canonical runtime observation to hot.
9. Do not alter Nginx/Cloudflare/current symlinks/timers until all acceptance gates pass.

No new third-party dependency is introduced by Storage v2. The new tooling uses Python standard library, SQLite, existing project binaries, and the already-required Docker runtime for package/runtime isolation.
