#!/usr/bin/env python3
"""Apply the reviewed static-analysis production hardening patch once.

This helper is executed by a temporary branch-only workflow because the GitHub
contents API does not support unified patch application. It removes itself and
its workflow before committing the generated source changes.
"""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github/workflows/apply-static-analysis-production-hardening.yml"
SELF = Path(__file__).resolve()


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def write(relative: str, value: str) -> None:
    (ROOT / relative).write_text(value, encoding="utf-8")


def replace_once(relative: str, old: str, new: str) -> None:
    value = read(relative)
    count = value.count(old)
    if count != 1:
        raise RuntimeError(
            f"{relative}: expected exactly one replacement target, found {count}"
        )
    write(relative, value.replace(old, new, 1))


def append_before(relative: str, marker: str, addition: str) -> None:
    value = read(relative)
    count = value.count(marker)
    if count != 1:
        raise RuntimeError(
            f"{relative}: expected exactly one insertion marker, found {count}"
        )
    write(relative, value.replace(marker, addition + marker, 1))


def patch_header() -> None:
    replace_once(
        "include/observatory/analyze.hpp",
        "struct AnalyzePackageOptions {\n    std::filesystem::path database;\n",
        "struct AnalyzePackageOptions {\n"
        "    std::filesystem::path database;\n"
        "    std::optional<std::int64_t> package_id;\n",
    )
    replace_once(
        "include/observatory/analyze.hpp",
        "[[nodiscard]] AnalyzeError resolve_exact_package(\n"
        "    const std::filesystem::path& database,\n"
        "    std::string_view server_identifier,\n"
        "    std::string_view server_version,\n"
        "    std::string_view package_identifier,\n"
        "    ResolvedPackage& resolved,\n"
        "    std::string& error);\n\n",
        "[[nodiscard]] AnalyzeError resolve_exact_package(\n"
        "    const std::filesystem::path& database,\n"
        "    std::string_view server_identifier,\n"
        "    std::string_view server_version,\n"
        "    std::string_view package_identifier,\n"
        "    ResolvedPackage& resolved,\n"
        "    std::string& error);\n\n"
        "[[nodiscard]] AnalyzeError resolve_package_by_id(\n"
        "    const std::filesystem::path& database,\n"
        "    std::int64_t package_id,\n"
        "    ResolvedPackage& resolved,\n"
        "    std::string& error);\n\n",
    )


def patch_main() -> None:
    replace_once(
        "src/main.cpp",
        '        << "  --database PATH               registry SQLite database\\n"\n'
        '        << "  --server IDENTIFIER           exact server identifier\\n"\n',
        '        << "  --database PATH               registry SQLite database\\n"\n'
        '        << "  --package-id ID               exact package record id\\n"\n'
        '        << "  --server IDENTIFIER           exact server identifier\\n"\n',
    )
    replace_once(
        "src/main.cpp",
        '        if (argument == "--database") options.database = std::string(value);\n'
        '        else if (argument == "--server")\n',
        '        if (argument == "--database") options.database = std::string(value);\n'
        '        else if (argument == "--package-id") {\n'
        '            std::int64_t package_id{};\n'
        '            if (!parse_finding_id(value, package_id)) {\n'
        '                std::cerr << "invalid package id\\n";\n'
        '                return 1;\n'
        '            }\n'
        '            options.package_id = package_id;\n'
        '        } else if (argument == "--server")\n',
    )
    replace_once(
        "src/main.cpp",
        '    if (options.database.empty() || options.server_identifier.empty() ||\n'
        '        options.server_version.empty() || options.package_identifier.empty()) {\n'
        '        std::cerr << "analyze package requires --database --server --version --package\\n";\n'
        '        return 1;\n'
        '    }\n',
        '    const bool have_server = !options.server_identifier.empty();\n'
        '    const bool have_version = !options.server_version.empty();\n'
        '    const bool have_package = !options.package_identifier.empty();\n'
        '    const bool have_triplet = have_server && have_version && have_package;\n'
        '    const bool have_any_triplet = have_server || have_version || have_package;\n'
        '    if (options.database.empty() ||\n'
        '        (options.package_id.has_value() && have_any_triplet) ||\n'
        '        (!options.package_id.has_value() && !have_triplet)) {\n'
        '        std::cerr\n'
        '            << "analyze package requires --database and exactly one selector: "\n'
        '            << "--package-id or --server/--version/--package\\n";\n'
        '        return 1;\n'
        '    }\n',
    )


def patch_analyzer() -> None:
    replace_once(
        "src/analyze.cpp",
        "    resolved = std::move(matches.front());\n"
        "    return AnalyzeError::none;\n"
        "}\n\n"
        "int analyze_worker_main(\n",
        "    resolved = std::move(matches.front());\n"
        "    return AnalyzeError::none;\n"
        "}\n\n"
        "AnalyzeError resolve_package_by_id(\n"
        "    const std::filesystem::path& database_path,\n"
        "    std::int64_t package_id,\n"
        "    ResolvedPackage& resolved,\n"
        "    std::string& error) {\n"
        "    if (package_id <= 0) {\n"
        "        error = \"package id must be positive\";\n"
        "        return AnalyzeError::invalid_arguments;\n"
        "    }\n"
        "    Database database;\n"
        "    if (!database.open(\n"
        "            database_path, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, error))\n"
        "        return AnalyzeError::database;\n"
        "    Statement query;\n"
        "    if (!query.prepare(\n"
        "            database,\n"
        "            \"SELECT p.id,p.server_version_id,sv.server_identifier,\"\n"
        "            \"sv.server_version,p.registry_type,p.identifier,p.version,\"\n"
        "            \"p.transport FROM packages p JOIN server_versions sv \"\n"
        "            \"ON sv.id=p.server_version_id WHERE p.id=?1;\",\n"
        "            error) ||\n"
        "        !query.bind_int64(1, package_id, error))\n"
        "        return AnalyzeError::database;\n"
        "    const int selected = query.step();\n"
        "    if (selected == SQLITE_DONE) {\n"
        "        error = \"exact package record not found for package id\";\n"
        "        return AnalyzeError::package_not_found;\n"
        "    }\n"
        "    if (selected != SQLITE_ROW) {\n"
        "        error = sqlite_message(database.get(), \"resolve package id\");\n"
        "        return AnalyzeError::database;\n"
        "    }\n"
        "    ResolvedPackage row;\n"
        "    row.package_id = query.integer(0);\n"
        "    row.server_version_id = query.integer(1);\n"
        "    row.server_identifier = query.text(2);\n"
        "    row.server_version = query.text(3);\n"
        "    row.registry_type = query.text(4);\n"
        "    row.package_identifier = query.text(5);\n"
        "    if (!query.is_null(6)) row.package_version = query.text(6);\n"
        "    row.transport = query.text(7);\n"
        "    if (row.registry_type != \"npm\" && row.registry_type != \"pypi\") {\n"
        "        error = \"unsupported registry type for package analysis v1: \" +\n"
        "            row.registry_type;\n"
        "        return AnalyzeError::unsupported_registry;\n"
        "    }\n"
        "    if (!row.package_version || row.package_version->empty()) {\n"
        "        error = \"exact package version is required; refusing to select latest\";\n"
        "        return AnalyzeError::missing_version;\n"
        "    }\n"
        "    resolved = std::move(row);\n"
        "    return AnalyzeError::none;\n"
        "}\n\n"
        "int analyze_worker_main(\n",
    )
    replace_once(
        "src/analyze.cpp",
        "    if (options.database.empty() || options.server_identifier.empty() ||\n"
        "        options.server_version.empty() || options.package_identifier.empty())\n"
        "        return failure_result(\n"
        "            AnalyzeError::invalid_arguments,\n"
        "            \"analyze package requires --database --server --version --package\");\n\n"
        "    ResolvedPackage package;\n"
        "    std::string error;\n"
        "    const AnalyzeError resolved = resolve_exact_package(\n"
        "        options.database,\n"
        "        options.server_identifier,\n"
        "        options.server_version,\n"
        "        options.package_identifier,\n"
        "        package,\n"
        "        error);\n",
        "    const bool have_triplet =\n"
        "        !options.server_identifier.empty() &&\n"
        "        !options.server_version.empty() &&\n"
        "        !options.package_identifier.empty();\n"
        "    const bool have_any_triplet =\n"
        "        !options.server_identifier.empty() ||\n"
        "        !options.server_version.empty() ||\n"
        "        !options.package_identifier.empty();\n"
        "    if (options.database.empty() ||\n"
        "        (options.package_id.has_value() && have_any_triplet) ||\n"
        "        (!options.package_id.has_value() && !have_triplet))\n"
        "        return failure_result(\n"
        "            AnalyzeError::invalid_arguments,\n"
        "            \"analyze package requires --database and exactly one selector: \"\n"
        "            \"--package-id or --server/--version/--package\");\n\n"
        "    ResolvedPackage package;\n"
        "    std::string error;\n"
        "    const AnalyzeError resolved = options.package_id.has_value()\n"
        "        ? resolve_package_by_id(\n"
        "              options.database, *options.package_id, package, error)\n"
        "        : resolve_exact_package(\n"
        "              options.database,\n"
        "              options.server_identifier,\n"
        "              options.server_version,\n"
        "              options.package_identifier,\n"
        "              package,\n"
        "              error);\n",
    )


def patch_scheduler() -> None:
    replace_once(
        "tools/bulk_static_analysis.py",
        '            if old in {"failed", "running"} and state == "eligible":\n',
        '            if old in {\n'
        '                "failed", "running", "unsupported", "unresolvable"\n'
        '            } and state == "eligible":\n',
    )
    replace_once(
        "tools/bulk_static_analysis.py",
        "def candidates(\n"
        "    db: sqlite3.Connection,\n"
        "    key: str,\n"
        "    batch_size: int,\n"
        "    maximum_attempts: int,\n"
        "    retry_seconds: int,\n"
        ") -> list[sqlite3.Row]:\n"
        "    return db.execute(\n"
        "        \"\"\"SELECT s.package_id,sv.server_identifier,sv.server_version,\n"
        "                  p.identifier package_identifier\n"
        "           FROM static_analysis_schedule_state s\n"
        "           JOIN packages p ON p.id=s.package_id\n"
        "           JOIN server_versions sv ON sv.id=p.server_version_id\n",
        "def candidates(\n"
        "    db: sqlite3.Connection,\n"
        "    key: str,\n"
        "    batch_size: int,\n"
        "    maximum_attempts: int,\n"
        "    retry_seconds: int,\n"
        ") -> list[sqlite3.Row]:\n"
        "    return db.execute(\n"
        "        \"\"\"SELECT s.package_id\n"
        "           FROM static_analysis_schedule_state s\n",
    )
    replace_once(
        "tools/bulk_static_analysis.py",
        "def run_child(argv: list[str], timeout: int, output_limit: int) -> subprocess.CompletedProcess[bytes]:\n"
        "    process = subprocess.Popen(\n"
        "        argv,\n"
        "        stdin=subprocess.DEVNULL,\n"
        "        stdout=subprocess.PIPE,\n"
        "        stderr=subprocess.PIPE,\n"
        "        close_fds=True,\n"
        "        start_new_session=True,\n"
        "        env={\"PATH\": os.environ.get(\"PATH\", \"/usr/bin:/bin\"), \"LANG\": \"C.UTF-8\"},\n"
        "    )\n",
        "def run_child(argv: list[str], timeout: int, output_limit: int) -> subprocess.CompletedProcess[bytes]:\n"
        "    child_environment = {\n"
        "        \"PATH\": os.environ.get(\"PATH\", \"/usr/bin:/bin\"),\n"
        "        \"LANG\": \"C.UTF-8\",\n"
        "    }\n"
        "    if os.environ.get(\"TMPDIR\"):\n"
        "        child_environment[\"TMPDIR\"] = os.environ[\"TMPDIR\"]\n"
        "    process = subprocess.Popen(\n"
        "        argv,\n"
        "        stdin=subprocess.DEVNULL,\n"
        "        stdout=subprocess.PIPE,\n"
        "        stderr=subprocess.PIPE,\n"
        "        close_fds=True,\n"
        "        start_new_session=True,\n"
        "        env=child_environment,\n"
        "    )\n",
    )
    replace_once(
        "tools/bulk_static_analysis.py",
        "def analyze_claimed(\n",
        "def classify_child_failure(returncode: int, stderr: str) -> tuple[str, str]:\n"
        "    lowered = stderr.lower()\n"
        "    if \"requested url returned error: 404\" in lowered:\n"
        "        return \"unresolvable\", \"registry_not_found\"\n"
        "    if \"pypi release metadata identity mismatch\" in lowered:\n"
        "        return \"unresolvable\", \"registry_identity_mismatch\"\n"
        "    if (\n"
        "        \"no supported non-yanked tar-gzip source distribution\"\n"
        "        in lowered\n"
        "    ):\n"
        "        return \"unsupported\", \"unsupported_pypi_distribution\"\n"
        "    if (\n"
        "        \"ambiguous package selection\" in lowered\n"
        "        or \"exact package record not found\" in lowered\n"
        "    ):\n"
        "        return \"unresolvable\", \"artifact_unresolvable\"\n"
        "    if returncode == 5:\n"
        "        if any(token in lowered for token in (\n"
        "            \"unsupported\", \"wheel\", \"zip sdist\", \"yanked\"\n"
        "        )):\n"
        "            return \"unsupported\", \"artifact_unsupported\"\n"
        "        return \"unresolvable\", \"artifact_unresolvable\"\n"
        "    return \"failed\", \"analysis_failed\"\n\n\n"
        "def reclassify_terminal_failures(db: sqlite3.Connection, key: str) -> None:\n"
        "    rules = (\n"
        "        (\"unresolvable\", \"registry_not_found\",\n"
        "         \"requested url returned error: 404\"),\n"
        "        (\"unresolvable\", \"registry_identity_mismatch\",\n"
        "         \"pypi release metadata identity mismatch\"),\n"
        "        (\"unsupported\", \"unsupported_pypi_distribution\",\n"
        "         \"no supported non-yanked tar-gzip source distribution\"),\n"
        "        (\"unresolvable\", \"artifact_unresolvable\",\n"
        "         \"ambiguous package selection\"),\n"
        "    )\n"
        "    for state, code, message_fragment in rules:\n"
        "        db.execute(\n"
        "            \"\"\"UPDATE static_analysis_schedule_state\n"
        "               SET state=?,reason_code=?,updated_at=CURRENT_TIMESTAMP\n"
        "               WHERE profile_key=? AND state='failed'\n"
        "                 AND instr(lower(COALESCE(reason_message,'')),?)>0\"\"\",\n"
        "            (state, code, key, message_fragment),\n"
        "        )\n\n\n"
        "def analyze_claimed(\n",
    )
    replace_once(
        "tools/bulk_static_analysis.py",
        "    argv = [\n"
        "        str(Path(args.observatory_binary).resolve()),\n"
        "        \"analyze\",\n"
        "        \"package\",\n"
        "        \"--database\",\n"
        "        str(database),\n"
        "        \"--server\",\n"
        "        str(item[\"server_identifier\"]),\n"
        "        \"--version\",\n"
        "        str(item[\"server_version\"]),\n"
        "        \"--package\",\n"
        "        str(item[\"package_identifier\"]),\n",
        "    argv = [\n"
        "        str(Path(args.observatory_binary).resolve()),\n"
        "        \"analyze\",\n"
        "        \"package\",\n"
        "        \"--database\",\n"
        "        str(database),\n"
        "        \"--package-id\",\n"
        "        str(package_id),\n",
    )
    replace_once(
        "tools/bulk_static_analysis.py",
        "        else:\n"
        "            lowered = stderr.lower()\n"
        "            if child.returncode == 5:\n"
        "                state = (\n"
        "                    \"unsupported\"\n"
        "                    if any(token in lowered for token in (\n"
        "                        \"unsupported\", \"wheel\", \"zip sdist\", \"yanked\"\n"
        "                    ))\n"
        "                    else \"unresolvable\"\n"
        "                )\n"
        "                code = (\n"
        "                    \"artifact_unsupported\"\n"
        "                    if state == \"unsupported\"\n"
        "                    else \"artifact_unresolvable\"\n"
        "                )\n"
        "            message = stderr or f\"analysis child exited with status {child.returncode}\"\n",
        "        else:\n"
        "            state, code = classify_child_failure(child.returncode, stderr)\n"
        "            message = stderr or f\"analysis child exited with status {child.returncode}\"\n",
    )
    replace_once(
        "tools/bulk_static_analysis.py",
        "                register_profile(db, profile, key)\n"
        "                synchronize(db, key, profile, args.stale_running_after_seconds)\n",
        "                register_profile(db, profile, key)\n"
        "                reclassify_terminal_failures(db, key)\n"
        "                synchronize(db, key, profile, args.stale_running_after_seconds)\n",
    )


def patch_installer_and_units() -> None:
    replace_once(
        "scripts/install_static_analysis_scheduler.sh",
        'evidence_root="${MCPO_EVIDENCE_ROOT:-$state_root/evidence}"\n',
        'evidence_root="${MCPO_EVIDENCE_ROOT:-$state_root/evidence}"\n'
        'tmp_root="${MCPO_STATIC_ANALYSIS_TMP_ROOT:-$state_root/tmp}"\n',
    )
    replace_once(
        "scripts/install_static_analysis_scheduler.sh",
        "  MCPO_INSTALL_ROOT, MCPO_STATE_ROOT, MCPO_DATABASE, MCPO_EVIDENCE_ROOT,\n",
        "  MCPO_INSTALL_ROOT, MCPO_STATE_ROOT, MCPO_DATABASE, MCPO_EVIDENCE_ROOT,\n"
        "  MCPO_STATIC_ANALYSIS_TMP_ROOT,\n",
    )
    replace_once(
        "scripts/install_static_analysis_scheduler.sh",
        '        "MCPO_EVIDENCE_ROOT:$evidence_root" \\\n',
        '        "MCPO_EVIDENCE_ROOT:$evidence_root" \\\n'
        '        "MCPO_STATIC_ANALYSIS_TMP_ROOT:$tmp_root" \\\n',
    )
    replace_once(
        "scripts/install_static_analysis_scheduler.sh",
        "MCPO_EVIDENCE_ROOT=$evidence_root\n"
        "MCPO_STATIC_ANALYSIS_RULES=$install_root/rules/artifact-static-analysis-v1.json\n",
        "MCPO_EVIDENCE_ROOT=$evidence_root\n"
        "TMPDIR=$tmp_root\n"
        "MCPO_STATIC_ANALYSIS_RULES=$install_root/rules/artifact-static-analysis-v1.json\n",
    )
    replace_once(
        "scripts/install_static_analysis_scheduler.sh",
        "prepare_state() {\n"
        "    install -d -o \"$service_user\" -g \"$catalog_group\" -m 0750 \"$evidence_root\"\n"
        "}\n",
        "prepare_state() {\n"
        "    install -d -o \"$service_user\" -g \"$catalog_group\" -m 0750 \"$evidence_root\"\n"
        "    install -d -o \"$service_user\" -g \"$catalog_group\" -m 0750 \"$tmp_root\"\n"
        "}\n",
    )
    replace_once(
        "scripts/install_static_analysis_scheduler.sh",
        '    log "evidence directory: $evidence_root"\n',
        '    log "evidence directory: $evidence_root"\n'
        '    log "Docker-visible temporary directory: $tmp_root"\n',
    )
    replace_once(
        "examples/systemd/mcp-observatory-static-analysis.service",
        "Environment=MCPO_EVIDENCE_ROOT=/var/lib/mcp-observatory/evidence\n",
        "Environment=MCPO_EVIDENCE_ROOT=/var/lib/mcp-observatory/evidence\n"
        "Environment=TMPDIR=/var/lib/mcp-observatory/tmp\n",
    )


def patch_bulk_tests() -> None:
    replace_once(
        "tests/test_bulk_static_analysis.py",
        "import json\nfrom pathlib import Path\n",
        "import json\nimport os\nfrom pathlib import Path\n",
    )
    replace_once(
        "tests/test_bulk_static_analysis.py",
        "package = sys.argv[sys.argv.index('--package') + 1]\n"
        "Path(%r).open('a', encoding='utf-8').write(package + '\\\\n')\n"
        "if package == 'pkg-fail':\n"
        "    print('temporary download failure', file=sys.stderr)\n"
        "    raise SystemExit(3)\n"
        "database = sys.argv[sys.argv.index('--database') + 1]\n"
        "db = sqlite3.connect(database)\n"
        "row = db.execute(\n"
        "    'SELECT server_version_id,id FROM packages WHERE identifier=?', (package,)\n"
        ").fetchone()\n",
        "package_id = int(sys.argv[sys.argv.index('--package-id') + 1])\n"
        "if '--package' in sys.argv:\n"
        "    raise SystemExit('scheduler must select by package id')\n"
        "database = sys.argv[sys.argv.index('--database') + 1]\n"
        "db = sqlite3.connect(database)\n"
        "row = db.execute(\n"
        "    'SELECT identifier,server_version_id FROM packages WHERE id=?',\n"
        "    (package_id,),\n"
        ").fetchone()\n"
        "package = row[0]\n"
        "Path(%r).open('a', encoding='utf-8').write(package + '\\\\n')\n"
        "if package == 'pkg-fail':\n"
        "    print('temporary download failure', file=sys.stderr)\n"
        "    raise SystemExit(3)\n",
    )
    replace_once(
        "tests/test_bulk_static_analysis.py",
        "    (row[0], row[1], %r),\n",
        "    (row[1], package_id, %r),\n",
    )
    replace_once(
        "tests/test_bulk_static_analysis.py",
        "package = sys.argv[sys.argv.index('--package') + 1]\n"
        "Path(%r).open('a', encoding='utf-8').write(package + '\\\\n')\n"
        "database = sys.argv[sys.argv.index('--database') + 1]\n"
        "db = sqlite3.connect(database)\n"
        "row = db.execute(\n"
        "    'SELECT server_version_id,id FROM packages WHERE identifier=?', (package,)\n"
        ").fetchone()\n",
        "package_id = int(sys.argv[sys.argv.index('--package-id') + 1])\n"
        "if '--package' in sys.argv:\n"
        "    raise SystemExit('scheduler must select by package id')\n"
        "database = sys.argv[sys.argv.index('--database') + 1]\n"
        "db = sqlite3.connect(database)\n"
        "row = db.execute(\n"
        "    'SELECT identifier,server_version_id FROM packages WHERE id=?',\n"
        "    (package_id,),\n"
        ").fetchone()\n"
        "package = row[0]\n"
        "Path(%r).open('a', encoding='utf-8').write(package + '\\\\n')\n",
    )
    replace_once(
        "tests/test_bulk_static_analysis.py",
        "    (run_id, row[0], row[1], digest),\n",
        "    (run_id, row[1], package_id, digest),\n",
    )
    addition = r'''
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

'''
    append_before(
        "tests/test_bulk_static_analysis.py",
        "\nif __name__ == \"__main__\":\n",
        addition,
    )


def patch_analyze_cli_test() -> None:
    marker = "        # 3 exact PyPI sdist acquisition, integrity verification, and reuse\n"
    addition = r'''        # Exact package-id selection bypasses an ambiguous identity triplet.
        connection = sqlite3.connect(database)
        selected_package_id = connection.execute(
            "SELECT id FROM packages WHERE identifier='same-name' AND version='1.0.0'"
        ).fetchone()[0]
        connection.close()
        exact_package_json = json.dumps(
            {"name": "same-name", "version": "1.0.0", "main": "index.js"}
        ).encode()
        exact_tarball = make_tarball(
            root,
            {
                "package/package.json": exact_package_json,
                "package/index.js": b"export const value = 1;\n",
            },
        )
        exact_metadata = {
            "name": "same-name",
            "version": "1.0.0",
            "dist": {
                "tarball": f"{base}/same-name/-/same-name-1.0.0.tgz",
                "integrity": sha512_integrity(exact_tarball),
            },
        }
        NpmHandler.catalog["/same-name/1.0.0"] = (
            json.dumps(exact_metadata).encode(),
            "application/json",
        )
        NpmHandler.catalog["/same-name/-/same-name-1.0.0.tgz"] = (
            exact_tarball,
            "application/octet-stream",
        )
        by_id = run(
            binary,
            "analyze",
            "package",
            "--database",
            str(database),
            "--package-id",
            str(selected_package_id),
            "--evidence-root",
            str(evidence),
            "--rules",
            str(rules),
            "--npm-registry-url",
            base,
            "--allow-in-process-worker",
            "--format",
            "json",
        )
        require(by_id.returncode == 0, "package-id analysis should complete", by_id)
        by_id_payload = json.loads(by_id.stdout)
        require(
            by_id_payload["package_identifier"] == "same-name"
            and by_id_payload["package_version"] == "1.0.0",
            "package-id selected the wrong record",
            by_id,
        )
        connection = sqlite3.connect(database)
        require(
            connection.execute(
                "SELECT package_id FROM analysis_runs WHERE id=?",
                (by_id_payload["analysis_run_id"],),
            ).fetchone()
            == (selected_package_id,),
            "package-id analysis run is attached to the wrong package record",
        )
        connection.close()
        conflicting_selector = run(
            binary,
            "analyze",
            "package",
            "--database",
            str(database),
            "--package-id",
            str(selected_package_id),
            "--server",
            "io.example/ambiguous",
            "--version",
            "1.0.0",
            "--package",
            "same-name",
        )
        require(
            conflicting_selector.returncode == 1,
            "package-id and identity triplet must be mutually exclusive",
            conflicting_selector,
        )

'''
    append_before("tests/test_analyze_cli.py", marker, addition)


def patch_docs() -> None:
    replace_once(
        "docs/static-analysis-scheduler-deployment.md",
        "- evidence directory: `/var/lib/mcp-observatory/evidence`;\n",
        "- evidence directory: `/var/lib/mcp-observatory/evidence`;\n"
        "- Docker-visible temporary directory: `/var/lib/mcp-observatory/tmp`;\n",
    )
    append_before(
        "docs/static-analysis-scheduler-deployment.md",
        "## Security acknowledgement\n",
        "The installer sets `TMPDIR` to the state directory rather than `/tmp`. "
        "The analyzer stages bind-mounted inputs there, so the host Docker daemon "
        "can see the same paths while the service keeps `PrivateTmp=yes`.\n\n",
    )
    replace_once(
        "docs/bulk-static-analysis.md",
        "package identifiers, versions, and server identifiers always come from the\n"
        "  catalog, never from an HTTP request or shell expansion;\n",
        "package record IDs always come from the catalog and are passed through the\n"
        "  exact `--package-id` analyzer selector, never from an HTTP request or shell\n"
        "  expansion;\n",
    )
    append_before(
        "docs/bulk-static-analysis.md",
        "## Coverage interpretation\n",
        "Registry HTTP 404 responses and registry identity mismatches are terminal "
        "`unresolvable` outcomes. PyPI releases without a supported non-yanked "
        "tar-gzip source distribution are terminal `unsupported` outcomes. The "
        "scheduler preserves these states across later synchronization runs instead "
        "of consuming retries.\n\n",
    )


def main() -> None:
    patch_header()
    patch_main()
    patch_analyzer()
    patch_scheduler()
    patch_installer_and_units()
    patch_bulk_tests()
    patch_analyze_cli_test()
    patch_docs()

    # Remove the branch-only mutation machinery from the generated commit.
    WORKFLOW.unlink()
    SELF.unlink()
    scripts_dir = SELF.parent
    try:
        scripts_dir.rmdir()
    except OSError:
        pass


if __name__ == "__main__":
    main()
