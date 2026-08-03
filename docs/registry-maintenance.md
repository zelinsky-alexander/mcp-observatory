# Reliable registry maintenance

The maintenance workflow keeps published SQLite catalogs readable while a
daily incremental collection is in progress. It does not change registry
evidence: every collected bundle remains immutable, and a failed attempt is
never relabelled as successful.

## Write and publication boundary

Every native command that opens a catalog for writing acquires an exclusive
advisory lock at `<database>.writer.lock`. The refresh, backup, and restore
scripts use the same lock name. The lock serializes cooperating writers; an
unrelated program that ignores it can still modify SQLite and is outside this
operational boundary.

`scripts/daily_registry_refresh.sh` performs these steps while holding the
published catalog lock:

1. Validate the live database with `integrity_check`, `foreign_key_check`, and
   require an existing latest snapshot.
2. Copy the live database into a same-directory staging database through the
   SQLite backup API.
3. Collect and import the incremental bundle into staging only.
4. Validate `_SUCCESS`, the bundle, staged SQLite integrity, foreign keys,
   latest snapshot digest, page count, and record count.
5. Create and verify a pre-publication backup and its SHA-256 metadata.
6. Retain the newest refresh backups, removing older backup and metadata pairs.
7. Flush staging and atomically replace the published database with `rename`;
   then flush the containing directory.
8. Atomically update `last-success.json`.

The daily script retains two refresh backups by default. Set
`MCPO_BACKUP_RETENTION_COUNT` to a positive integer to change the limit. The
equivalent maintenance command option is `--backup-retention-count`. Pruning
runs only after the new backup and its verification metadata have been
successfully created, and only applies to timestamped refresh backups for the
same database. Manual and pre-restore backups are not affected.

Normal failures remove staging. A forced power loss or `SIGKILL` can leave a
dot-prefixed staging file, but cannot partly overwrite the published file.
SQLite uses the rollback journal for this catalog; do not switch the published
database to WAL without revisiting sidecar publication.

## Status record

The default record is
`runtime/registry-refresh/last-success.json`. It has `schema_version: 1`, the
attempt's UTC and local timestamps, `state`, `last_success`, and `failure`.
Successful state includes the published snapshot digest, bundle and backup
paths, and collection/catalog counts. Failed state includes a category,
message, exit code, and failure timestamps while retaining the preceding
`last_success` object when one exists.

Monitor `state`, the age of `last_success.published_at_utc`, and timer/service
failures. Do not treat a recent failed-attempt timestamp as a successful
refresh.

## Verified backup and restore

Create a transactionally consistent backup:

```bash
scripts/backup_registry_catalog.sh \
  /var/lib/mcp-observatory/local-registry.sqlite \
  /var/lib/mcp-observatory/backups/manual.sqlite
```

The command validates the copy and writes `manual.sqlite.json` containing its
SHA-256, size, snapshot digest, counts, and creation time. Keep both files.

Restore only a matching, valid pair:

```bash
systemctl stop mcp-observatory-refresh.timer

scripts/restore_registry_catalog.sh \
  /var/lib/mcp-observatory/local-registry.sqlite \
  /var/lib/mcp-observatory/backups/manual.sqlite
```

Restore verifies the metadata digest, source integrity, staged copy, and
expected snapshot before atomic replacement. When the current database is
valid, it must create a verified, uniquely named `pre-restore` backup beside
it; any backup or metadata failure aborts before replacement. If current
database validation fails, restore may proceed without that backup. It emits a
warning on standard error and reports `pre_restore_backup.state` as `skipped`
with the validation failure in its JSON result. A successful recovery backup
is reported with state `created` and both paths. Restart the timer only after
read-only queries and `last-success.json` have been reviewed.

## Failure recovery

Collection failure: the live database is unchanged. Preserve the partial
bundle and progress log for diagnosis. Correct connectivity, timeout, disk, or
registry-response problems, then run the daily script again. Never add
`_SUCCESS` manually.

Import failure: collection may have produced a valid immutable bundle, but the
staged transaction failed and the live catalog is unchanged. Inspect the
progress log and validate the preserved bundle with `mcp-observatory bundle
validate`. Correct the local schema, capacity, or permission cause and rerun
the staged refresh. Directly importing into the live database bypasses staged
publication and is not the production recovery path.

Validation failure: do not publish or modify the evidence bundle. Retain the
bundle and logs, investigate the reported bundle digest/count or SQLite
integrity mismatch, and rerun only after the cause is understood. If the live
database itself fails pre-refresh validation, restore a verified backup.

Interrupted publication: an interruption before `rename` leaves the previous
database intact; after `rename`, it leaves the complete validated replacement.
First stop the timer and ensure no service process is active. Validate the live
database and compare its latest digest with the bundle and status record. If
the database is valid but status is stale, retain the logs and allow the next
refresh to advance from that database. If it is invalid or its provenance is
uncertain, use verified restore. Dot-prefixed `*.staging` files may be removed
only after confirming no writer holds the adjacent lock.

## AWS/Linux systemd example

Examples are provided in `examples/systemd`. Install the repository read-only
at `/opt/mcp-observatory`, create an unprivileged `mcp-observatory` account,
and place the initial validated database under
`/var/lib/mcp-observatory`. Review paths and hardening against the target AMI:

```bash
sudo install -m 0644 examples/systemd/mcp-observatory-refresh.service \
  /etc/systemd/system/
sudo install -m 0644 examples/systemd/mcp-observatory-refresh.timer \
  /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemd-analyze security mcp-observatory-refresh.service
sudo systemctl enable --now mcp-observatory-refresh.timer
```

The service has no capabilities, writes only its state directory, blocks
link-local addresses (including EC2 instance metadata), uses a dedicated
account, and kills the whole process group on timeout. It still allows public
IPv4/IPv6 and DNS needed for the Official Registry. If the host requires an
HTTP proxy, private CA, different resolver, or no IPv6, adjust the unit
explicitly and re-run `systemd-analyze security`.
