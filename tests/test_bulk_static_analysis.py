#!/usr/bin/env python3
"""Offline tests for the bounded bulk static-analysis scheduler."""

from __future__ import annotations

import json
import os
from pathlib import Path
import sqlite3
import stat
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "bulk_static_analysis.py"

SCHEMA = """
CREATE TABLE server_versions(
  id INTEGER PRIMARY KEY,
  server_identifier TEXT NOT NULL,
  server_version TEXT NOT NULL
);
CREATE TABLE packages(
  id INTEGER PRIMARY KEY,
  server_version_id INTEGER NOT NULL,
  registry_type TEXT NOT NULL,
  identifier TEXT,
  version TEXT
);
CREATE TABLE analysis_runs(
  id INTEGER PRIMARY KEY,
  server_version_id INTEGER NOT NULL,
  package_id INTEGER NOT NULL,
  analysis_type TEXT NOT NULL,
  status TEXT NOT NULL,
  analyzer_name TEXT NOT NULL,
  analyzer_version TEXT NOT NULL,
  ruleset_version TEXT NOT NULL,
  artifact_sha256 TEXT
);
"""


def write_rules(root: Path) -> Path:
    rules = root / "rules.json"
    rules.write_text(json.dumps({"ruleset_version": "1.0.0"}), encoding="utf-8")
    return rules


def scheduler_command(
    database: Path,
    fake: Path,
    rules: Path,
    evidence: Path,
    *,
    batch_size: int,
) -> list[str]:
    return [
        sys.executable,
        str(SCRIPT),
        "--database",
        str(database),
        "--observatory-binary",
        str(fake),
        "--rules",
        str(rules),
        "--evidence-root",
        str(evidence),
        "--batch-size",
        str(batch_size),
        "--retry-failed-after-seconds",
        "3600",
        "--format",
        "json",
    ]


class BulkStaticAnalysisTests(unittest.TestCase):
    def test_classifies_processes_and_is_idempotent(self) -> None:
        with tempfile.TemporaryDirectory(prefix="mcpo-bulk-static-") as temporary:
            root = Path(temporary)
            database = root / "catalog.sqlite"
            calls = root / "calls.log"
            evidence = root / "evidence"
            connection = sqlite3.connect(database)
            connection.executescript(SCHEMA)
            connection.executemany(
                "INSERT INTO server_versions VALUES(?,?,?)",
                [
                    (1, "srv-ok", "1"),
                    (2, "srv-fail", "1"),
                    (3, "srv-unsupported", "1"),
                    (4, "srv-missing", "1"),
                    (5, "srv-existing", "1"),
                ],
            )
            connection.executemany(
                "INSERT INTO packages VALUES(?,?,?,?,?)",
                [
                    (10, 1, "npm", "pkg-ok", "1"),
                    (20, 2, "pypi", "pkg-fail", "1"),
                    (30, 3, "docker", "image", "1"),
                    (40, 4, "npm", "pkg-missing", None),
                    (50, 5, "npm", "pkg-existing", "1"),
                ],
            )
            connection.execute(
                """INSERT INTO analysis_runs VALUES(
                     900,5,50,'npm_package_static_v1','completed',
                     'mcp-observatory-static','1.1.0','1.0.0',?)""",
                ("b" * 64,),
            )
            connection.commit()
            connection.close()

            rules = write_rules(root)
            fake = root / "fake-observatory.py"
            fake.write_text(
                """#!/usr/bin/env python3
import json
from pathlib import Path
import sqlite3
import sys
package_id = int(sys.argv[sys.argv.index('--package-id') + 1])
if '--package' in sys.argv:
    raise SystemExit('scheduler must select by package id')
database = sys.argv[sys.argv.index('--database') + 1]
db = sqlite3.connect(database)
row = db.execute(
    'SELECT identifier,server_version_id FROM packages WHERE id=?',
    (package_id,),
).fetchone()
package = row[0]
Path(%r).open('a', encoding='utf-8').write(package + '\\n')
if package == 'pkg-fail':
    print('temporary download failure', file=sys.stderr)
    raise SystemExit(3)
db.execute(
    "INSERT INTO analysis_runs VALUES(901,?,?,"
    "'npm_package_static_v1','completed','mcp-observatory-static',"
    "'1.1.0','1.0.0',?)",
    (row[1], package_id, %r),
)
db.commit()
db.close()
print(json.dumps({
    'status': 'completed',
    'analysis_run_id': 901,
    'artifact_sha256': %r,
    'reused_existing': False,
}))
"""
                % (str(calls), "a" * 64, "a" * 64),
                encoding="utf-8",
            )
            fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
            command = scheduler_command(
                database, fake, rules, evidence, batch_size=10
            )

            first = subprocess.run(command, text=True, capture_output=True, check=False)
            self.assertEqual(first.returncode, 0, first.stderr)
            payload = json.loads(first.stdout)
            self.assertEqual(payload["eligible_package_records"], 3)
            self.assertEqual(payload["successfully_analyzed"], 2)
            self.assertEqual(payload["failed_attempts"], 1)
            self.assertEqual(payload["unsupported_or_unresolvable"], 2)
            self.assertEqual(payload["never_attempted"], 0)
            self.assertEqual(payload["unique_artifacts_analyzed"], 2)
            self.assertEqual(payload["processed_in_batch"], 2)
            self.assertEqual(payload["remaining_queue_records"], 1)
            self.assertEqual(payload["stop_reason"], "queue_empty")
            self.assertGreaterEqual(payload["run_elapsed_seconds"], 0)
            self.assertIsNotNone(payload["estimated_remaining_seconds"])
            self.assertEqual(
                calls.read_text(encoding="utf-8").splitlines(),
                ["pkg-fail", "pkg-ok"],
            )

            second = subprocess.run(command, text=True, capture_output=True, check=False)
            self.assertEqual(second.returncode, 0, second.stderr)
            second_payload = json.loads(second.stdout)
            self.assertEqual(second_payload["processed_in_batch"], 0)
            self.assertEqual(second_payload["stop_reason"], "queue_empty")
            self.assertEqual(
                calls.read_text(encoding="utf-8").splitlines(),
                ["pkg-fail", "pkg-ok"],
            )

            connection = sqlite3.connect(database)
            states = dict(
                connection.execute(
                    "SELECT package_id,state FROM static_analysis_schedule_state"
                )
            )
            connection.close()
            self.assertEqual(
                states,
                {
                    10: "completed",
                    20: "failed",
                    30: "unsupported",
                    40: "unresolvable",
                    50: "completed",
                },
            )

    def test_new_catalog_record_precedes_existing_backlog(self) -> None:
        with tempfile.TemporaryDirectory(prefix="mcpo-bulk-priority-") as temporary:
            root = Path(temporary)
            database = root / "catalog.sqlite"
            calls = root / "calls.log"
            connection = sqlite3.connect(database)
            connection.executescript(SCHEMA)
            connection.executemany(
                "INSERT INTO server_versions VALUES(?,?,?)",
                [(1, "srv-old-a", "1"), (2, "srv-old-b", "1")],
            )
            connection.executemany(
                "INSERT INTO packages VALUES(?,?,?,?,?)",
                [
                    (10, 1, "npm", "pkg-old-a", "1"),
                    (20, 2, "npm", "pkg-old-b", "1"),
                ],
            )
            connection.commit()
            connection.close()

            rules = write_rules(root)
            fake = root / "fake-observatory.py"
            fake.write_text(
                """#!/usr/bin/env python3
import hashlib
import json
from pathlib import Path
import sqlite3
import sys
package_id = int(sys.argv[sys.argv.index('--package-id') + 1])
if '--package' in sys.argv:
    raise SystemExit('scheduler must select by package id')
database = sys.argv[sys.argv.index('--database') + 1]
db = sqlite3.connect(database)
row = db.execute(
    'SELECT identifier,server_version_id FROM packages WHERE id=?',
    (package_id,),
).fetchone()
package = row[0]
Path(%r).open('a', encoding='utf-8').write(package + '\\n')
run_id = db.execute('SELECT COALESCE(MAX(id), 900) + 1 FROM analysis_runs').fetchone()[0]
digest = hashlib.sha256(package.encode()).hexdigest()
db.execute(
    "INSERT INTO analysis_runs VALUES(?,?,?,"
    "'npm_package_static_v1','completed','mcp-observatory-static',"
    "'1.1.0','1.0.0',?)",
    (run_id, row[1], package_id, digest),
)
db.commit()
db.close()
print(json.dumps({
    'status': 'completed', 'analysis_run_id': run_id,
    'artifact_sha256': digest, 'reused_existing': False,
}))
"""
                % str(calls),
                encoding="utf-8",
            )
            fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
            command = scheduler_command(
                database, fake, rules, root / "evidence", batch_size=1
            )

            first = subprocess.run(command, text=True, capture_output=True, check=False)
            self.assertEqual(first.returncode, 0, first.stderr)
            self.assertEqual(
                calls.read_text(encoding="utf-8").splitlines(), ["pkg-old-b"]
            )

            connection = sqlite3.connect(database)
            connection.execute(
                "UPDATE static_analysis_schedule_state SET discovered_at='2026-01-01 00:00:00'"
            )
            connection.execute(
                "INSERT INTO server_versions VALUES(3,'srv-new','2')"
            )
            connection.execute(
                "INSERT INTO packages VALUES(30,3,'npm','pkg-new','2')"
            )
            connection.commit()
            connection.close()

            second = subprocess.run(command, text=True, capture_output=True, check=False)
            self.assertEqual(second.returncode, 0, second.stderr)
            self.assertEqual(
                calls.read_text(encoding="utf-8").splitlines(),
                ["pkg-old-b", "pkg-new"],
            )


    def test_terminal_failures_are_classified_and_preserved(self) -> None:
        with tempfile.TemporaryDirectory(prefix="mcpo-bulk-terminal-") as temporary:
            root = Path(temporary)
            database = root / "catalog.sqlite"
            connection = sqlite3.connect(database)
            connection.executescript(SCHEMA)
            connection.executemany(
                "INSERT INTO server_versions VALUES(?,?,?)",
                [(1, "not-found", "1"), (2, "identity", "1"), (3, "wheel", "1")],
            )
            connection.executemany(
                "INSERT INTO packages VALUES(?,?,?,?,?)",
                [
                    (10, 1, "npm", "not-found", "1"),
                    (20, 2, "pypi", "identity", "1"),
                    (30, 3, "pypi", "wheel-only", "1"),
                ],
            )
            connection.commit()
            connection.close()

            fake = root / "fake-observatory.py"
            fake.write_text(
                """#!/usr/bin/env python3
import sys
package_id = int(sys.argv[sys.argv.index('--package-id') + 1])
messages = {
    10: 'curl: (22) The requested URL returned error: 404',
    20: 'PyPI release metadata identity mismatch',
    30: 'PyPI release has no supported non-yanked tar-gzip source distribution',
}
print(messages[package_id], file=sys.stderr)
raise SystemExit(3 if package_id == 10 else 7)
""",
                encoding="utf-8",
            )
            fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
            command = scheduler_command(
                database,
                fake,
                write_rules(root),
                root / "evidence",
                batch_size=10,
            )
            first = subprocess.run(command, text=True, capture_output=True, check=False)
            self.assertEqual(first.returncode, 0, first.stderr)
            payload = json.loads(first.stdout)
            self.assertEqual(payload["processed_in_batch"], 3)
            self.assertEqual(payload["failed_attempts"], 0)
            self.assertEqual(payload["unsupported_or_unresolvable"], 3)
            self.assertEqual(payload["remaining_queue_records"], 0)

            second = subprocess.run(command, text=True, capture_output=True, check=False)
            self.assertEqual(second.returncode, 0, second.stderr)
            self.assertEqual(json.loads(second.stdout)["processed_in_batch"], 0)

            connection = sqlite3.connect(database)
            outcomes = dict(
                connection.execute(
                    "SELECT package_id,state || ':' || reason_code "
                    "FROM static_analysis_schedule_state"
                )
            )
            connection.close()
            self.assertEqual(
                outcomes,
                {
                    10: "unresolvable:registry_not_found",
                    20: "unresolvable:registry_identity_mismatch",
                    30: "unsupported:unsupported_pypi_distribution",
                },
            )

    def test_child_receives_configured_tmpdir(self) -> None:
        with tempfile.TemporaryDirectory(prefix="mcpo-bulk-tmpdir-") as temporary:
            root = Path(temporary)
            database = root / "catalog.sqlite"
            connection = sqlite3.connect(database)
            connection.executescript(SCHEMA)
            connection.execute("INSERT INTO server_versions VALUES(1,'srv','1')")
            connection.execute("INSERT INTO packages VALUES(10,1,'npm','pkg','1')")
            connection.commit()
            connection.close()

            observed = root / "observed.txt"
            fake = root / "fake-observatory.py"
            fake.write_text(
                """#!/usr/bin/env python3
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import sys
package_id = int(sys.argv[sys.argv.index('--package-id') + 1])
Path(%r).write_text(os.environ.get('TMPDIR', ''), encoding='utf-8')
database = sys.argv[sys.argv.index('--database') + 1]
db = sqlite3.connect(database)
server_version_id = db.execute(
    'SELECT server_version_id FROM packages WHERE id=?', (package_id,)
).fetchone()[0]
digest = hashlib.sha256(str(package_id).encode()).hexdigest()
db.execute(
    "INSERT INTO analysis_runs VALUES(901,?,?,"
    "'npm_package_static_v1','completed','mcp-observatory-static',"
    "'1.1.0','1.0.0',?)",
    (server_version_id, package_id, digest),
)
db.commit()
db.close()
print(json.dumps({
    'status': 'completed', 'analysis_run_id': 901,
    'artifact_sha256': digest, 'reused_existing': False,
}))
"""
                % str(observed),
                encoding="utf-8",
            )
            fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
            tmpdir = root / "docker-visible-tmp"
            tmpdir.mkdir()
            environment = os.environ.copy()
            environment["TMPDIR"] = str(tmpdir)
            result = subprocess.run(
                scheduler_command(
                    database,
                    fake,
                    write_rules(root),
                    root / "evidence",
                    batch_size=1,
                ),
                text=True,
                capture_output=True,
                check=False,
                env=environment,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(observed.read_text(encoding="utf-8"), str(tmpdir))


if __name__ == "__main__":
    unittest.main()
