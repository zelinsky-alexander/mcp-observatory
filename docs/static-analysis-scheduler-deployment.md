# Static-analysis scheduler deployment

The systemd files under `examples/systemd/` are reference configurations. The
production installation entry point is:

```bash
sudo bash scripts/install_static_analysis_scheduler.sh \
  --accept-docker-root-equivalent
```

The installer is idempotent and targets the deployed AWS layout by default:

- release/current symlink: `/opt/mcp-observatory/current`;
- catalog database: `/var/lib/mcp-observatory/catalog/local-registry.sqlite`;
- evidence directory: `/var/lib/mcp-observatory/evidence`;
- catalog writer account: `mcp-refresh`;
- shared read group: `mcp-catalog`.

It performs the following operations:

1. validates the release binary, scheduler, rules, service identities, database,
   Docker installation, and catalog write permissions;
2. creates the evidence directory as `mcp-refresh:mcp-catalog` with mode `0750`;
3. writes `/etc/mcp-observatory/static-analysis.env`;
4. installs the production service and timer under `/etc/systemd/system`;
5. installs a drop-in for `mcp-observatory-refresh.service` so successful catalog
   publication triggers analysis immediately;
6. verifies the generated units with `systemd-analyze verify`;
7. reloads systemd and enables the hourly timer;
8. requests the first analysis run asynchronously unless `--no-start` is used.

The generated service processes at most 1,000 package records and at most 50
minutes per invocation. The timer starts it hourly. The refresh drop-in starts it
after each successful daily catalog refresh, so new package/version records are
handled while the hourly runs continue draining the historical backlog.

## Security acknowledgement

The current analyzer uses the host Docker daemon. Membership in the Docker group
is effectively root-equivalent, so the installer requires the explicit
`--accept-docker-root-equivalent` option. The generated unit grants Docker access
only to the `mcp-refresh` analysis service. It does not modify the `mcp-portal`
account or grant the public portal catalog-write or Docker access.

## Verify installation

```bash
systemctl status \
  mcp-observatory-static-analysis.timer \
  mcp-observatory-static-analysis.service

systemctl list-timers mcp-observatory-static-analysis.timer

journalctl -u mcp-observatory-static-analysis.service -n 100 --no-pager
```

Confirm the refresh trigger:

```bash
systemctl cat mcp-observatory-refresh.service
```

The output should include:

```ini
[Unit]
OnSuccess=mcp-observatory-static-analysis.service
```

## Configuration overrides

Set environment variables on the installer command to change deployment paths or
bounds. For example:

```bash
sudo env \
  MCPO_STATIC_ANALYSIS_BATCH_SIZE=500 \
  MCPO_STATIC_ANALYSIS_MAXIMUM_RUN_SECONDS=2400 \
  bash scripts/install_static_analysis_scheduler.sh \
    --accept-docker-root-equivalent
```

The supported variable list is available through:

```bash
bash scripts/install_static_analysis_scheduler.sh --help
```
