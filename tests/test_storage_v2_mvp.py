#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
from pathlib import Path
import sqlite3
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


foundation = load("storage_v2_foundation_for_mvp", ROOT / "tools" / "storage_v2_foundation.py")
mvp = load("storage_v2_mvp", ROOT / "tools" / "storage_v2_mvp.py")

SCHEMA = """
PRAGMA foreign_keys=ON;
CREATE TABLE snapshots(id INTEGER PRIMARY KEY,snapshot_sha256 TEXT,completed_at TEXT);
CREATE TABLE server_versions(id INTEGER PRIMARY KEY,server_identifier TEXT,server_version TEXT);
CREATE TABLE packages(id INTEGER PRIMARY KEY,server_version_id INTEGER,registry_type TEXT,identifier TEXT,version TEXT,transport TEXT);
CREATE TABLE analysis_runs(
  id INTEGER PRIMARY KEY,server_version_id INTEGER,package_id INTEGER,
  analysis_type TEXT,status TEXT,analyzer_name TEXT,analyzer_version TEXT,
  ruleset_version TEXT,started_at TEXT,completed_at TEXT,artifact_sha256 TEXT,
  published_integrity TEXT,integrity_verified INTEGER,base_image_ref TEXT,
  base_image_digest TEXT,network_mode TEXT,container_read_only INTEGER,
  container_user TEXT,summary_json TEXT,error_stage TEXT,error_message TEXT
);
CREATE TABLE analysis_artifacts(
  id INTEGER PRIMARY KEY,analysis_run_id INTEGER UNIQUE,registry_type TEXT,
  package_identifier TEXT,package_version TEXT,source_url TEXT,
  local_relative_path TEXT,byte_size INTEGER,sha256 TEXT,published_integrity TEXT,
  integrity_verified INTEGER,downloaded_at TEXT
);
CREATE TABLE analysis_files(
  id INTEGER PRIMARY KEY,analysis_run_id INTEGER,archive_path TEXT,file_type TEXT,
  byte_size INTEGER,sha256 TEXT,executable INTEGER,native_binary INTEGER,
  generated INTEGER,minified INTEGER,UNIQUE(analysis_run_id,archive_path)
);
CREATE TABLE analysis_dependencies(
  id INTEGER PRIMARY KEY,analysis_run_id INTEGER,dependency_type TEXT,
  dependency_name TEXT,declared_version TEXT,resolved_version TEXT,
  direct INTEGER,development INTEGER
);
CREATE TABLE analysis_findings(
  id INTEGER PRIMARY KEY,analysis_run_id INTEGER,rule_id TEXT,category TEXT,
  severity TEXT,confidence TEXT,disposition TEXT,subject_path TEXT,
  line_number INTEGER,symbol TEXT,title TEXT,evidence TEXT,explanation TEXT
);
CREATE TABLE analysis_evidence(
  id INTEGER PRIMARY KEY,analysis_run_id INTEGER,evidence_type TEXT,
  relative_path TEXT,sha256 TEXT,byte_size INTEGER,media_type TEXT,
  UNIQUE(analysis_run_id,relative_path)
);
CREATE TABLE analysis_finding_reviews(
  id INTEGER PRIMARY KEY,finding_id INTEGER,previous_disposition TEXT,
  disposition TEXT,reviewer TEXT,reviewed_at TEXT
);
CREATE TABLE static_analysis_schedule_profiles(
  profile_key TEXT PRIMARY KEY,analysis_type TEXT,analyzer_name TEXT,
  analyzer_version TEXT,ruleset_version TEXT,rules_sha256 TEXT
);
CREATE TABLE static_analysis_schedule_current(singleton INTEGER PRIMARY KEY,profile_key TEXT);
CREATE TABLE static_analysis_schedule_state(
  profile_key TEXT,package_id INTEGER,state TEXT,reason_code TEXT,
  reason_message TEXT,attempt_count INTEGER,analysis_run_id INTEGER,
  artifact_sha256 TEXT,reused_existing INTEGER,discovered_at TEXT,
  last_attempt_at TEXT,updated_at TEXT,PRIMARY KEY(profile_key,package_id)
);
"""


class StorageV2MvpTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.history = self.root / "history.sqlite"
        self.hot = self.root / "hot.sqlite"
        db = sqlite3.connect(self.history)
        db.row_factory = sqlite3.Row
        db.executescript(SCHEMA)
        db.execute("INSERT INTO snapshots VALUES(1,'snap','2026-08-07T00:00:00Z')")
        db.execute("INSERT INTO server_versions VALUES(1,'io.example/test','1.0.0')")
        db.execute("INSERT INTO packages VALUES(1,1,'npm','example','1.0.0','stdio')")
        db.execute(
            """INSERT INTO analysis_runs(
                 id,server_version_id,package_id,analysis_type,status,analyzer_name,
                 analyzer_version,ruleset_version,started_at,completed_at,artifact_sha256)
               VALUES(1,1,1,'npm_package_static_v1','completed','static','1.1.0',
                      'rules-1','2026-08-07T00:00:00Z','2026-08-07T00:01:00Z','aaa')"""
        )
        db.execute("INSERT INTO analysis_files VALUES(NULL,1,'index.js','javascript',100,'f',0,0,0,0)")
        db.execute(
            """INSERT INTO analysis_findings(
                 analysis_run_id,rule_id,category,severity,confidence,disposition,
                 subject_path,line_number,symbol,title,evidence,explanation)
               VALUES(1,'risk-api:fetch','risk-api','high','high','unreviewed',
                      'index.js',1,'fetch','Fetch use','fetch(url)','Network API')"""
        )
        db.execute(
            "INSERT INTO static_analysis_schedule_profiles VALUES('profile','npm_package_static_v1','static','1.1.0','rules-1','r')"
        )
        db.execute("INSERT INTO static_analysis_schedule_current VALUES(1,'profile')")
        db.execute(
            """INSERT INTO static_analysis_schedule_state VALUES(
                 'profile',1,'completed',NULL,NULL,1,1,'aaa',0,CURRENT_TIMESTAMP,
                 CURRENT_TIMESTAMP,CURRENT_TIMESTAMP)"""
        )
        foundation.install(db)
        foundation.backfill(db, 100)
        foundation.refresh_coverage(db)
        db.commit()
        db.close()

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_hot_catalog_removes_detail_but_keeps_summaries(self) -> None:
        sizes = mvp.compact_hot_catalog(self.history, self.hot)
        self.assertGreater(sizes["before_bytes"], 0)
        db = sqlite3.connect(self.hot)
        self.assertEqual(db.execute("SELECT COUNT(*) FROM analysis_files").fetchone()[0], 0)
        self.assertEqual(db.execute("SELECT COUNT(*) FROM analysis_findings").fetchone()[0], 0)
        self.assertEqual(db.execute("SELECT COUNT(*) FROM analysis_v2_run_summaries").fetchone()[0], 1)
        self.assertEqual(
            db.execute("SELECT unreviewed_high_count FROM analysis_v2_run_summaries").fetchone()[0], 1
        )
        db.close()

    def test_publish_adds_new_summary_without_detail_rows(self) -> None:
        mvp.compact_hot_catalog(self.history, self.hot)
        db = sqlite3.connect(self.history)
        db.execute(
            """INSERT INTO analysis_runs(
                 id,server_version_id,package_id,analysis_type,status,analyzer_name,
                 analyzer_version,ruleset_version,started_at,completed_at,artifact_sha256)
               VALUES(2,1,1,'npm_package_static_v1','completed','static','1.1.0',
                      'rules-1','2026-08-07T01:00:00Z','2026-08-07T01:01:00Z','bbb')"""
        )
        db.execute("INSERT INTO analysis_files VALUES(NULL,2,'main.js','javascript',200,'g',0,0,0,0)")
        db.execute(
            """INSERT INTO analysis_findings(
                 analysis_run_id,rule_id,category,severity,confidence,disposition,
                 subject_path,line_number,symbol,title,evidence,explanation)
               VALUES(2,'risk-api:fetch','risk-api','high','high','unreviewed',
                      'main.js',2,'fetch','Fetch use','fetch(url)','Network API')"""
        )
        db.commit()
        db.close()
        mvp.publish_summaries(self.history, self.hot)
        hot = sqlite3.connect(self.hot)
        self.assertEqual(hot.execute("SELECT COUNT(*) FROM analysis_runs").fetchone()[0], 2)
        self.assertEqual(hot.execute("SELECT COUNT(*) FROM analysis_v2_run_summaries").fetchone()[0], 2)
        self.assertEqual(hot.execute("SELECT COUNT(*) FROM analysis_files").fetchone()[0], 0)
        self.assertEqual(hot.execute("SELECT COUNT(*) FROM analysis_findings").fetchone()[0], 0)
        hot.close()

    def test_evidence_bundle_is_deterministic_and_excludes_artifact_and_rules(self) -> None:
        evidence = self.root / "evidence"
        evidence.mkdir()
        (evidence / "artifact.tgz").write_bytes(b"package")
        (evidence / "analysis-rules.json").write_text("{}", encoding="utf-8")
        (evidence / "findings.jsonl").write_text('{"rule":"x"}\n', encoding="utf-8")
        (evidence / "files.jsonl").write_text('{"path":"x"}\n', encoding="utf-8")
        one = self.root / "one.tar.gz"
        two = self.root / "two.tar.gz"
        first = mvp.deterministic_bundle(evidence, one)
        second = mvp.deterministic_bundle(evidence, two)
        self.assertEqual(first["bundle_sha256"], second["bundle_sha256"])
        with tarfile.open(one, "r:gz") as archive:
            names = archive.getnames()
        self.assertEqual(names, ["files.jsonl", "findings.jsonl"])
        self.assertNotIn("artifact.tgz", names)
        self.assertNotIn("analysis-rules.json", names)


if __name__ == "__main__":
    unittest.main()
