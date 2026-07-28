#include "observatory/explorer.hpp"

#include "observatory/registry.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <ctime>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcpo {
namespace {

#ifndef MCPO_VERSION
#define MCPO_VERSION "unknown"
#endif

constexpr std::size_t maximum_search_limit = 500U;
constexpr std::size_t maximum_list_limit = 1'000U;

bool fits_sqlite_integer(std::size_t value) {
    return value <= static_cast<std::size_t>(
        std::numeric_limits<sqlite3_int64>::max());
}

ExplorerResult failure(ExplorerError error, std::string message) {
    return {error, std::move(message), std::nullopt};
}

std::string elapsed_text(std::chrono::steady_clock::duration duration) {
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    std::ostringstream output;
    output << std::fixed << std::setprecision(3)
           << static_cast<double>(milliseconds) / 1000.0 << 's';
    return output.str();
}

std::string utc_now() {
    const std::time_t now = std::time(nullptr);
    std::tm value{};
    gmtime_r(&now, &value);
    char buffer[21]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &value);
    return buffer;
}

void progress(bool enabled, std::string_view message) {
    if (!enabled) return;
    std::cerr << "[registry-index] " << message << '\n';
    std::cerr.flush();
}

std::string sqlite_message(sqlite3* database, std::string_view operation) {
    return std::string(operation) + ": " +
        (database == nullptr ? "SQLite handle unavailable" : sqlite3_errmsg(database));
}

class Database {
public:
    Database() = default;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    ~Database() {
        if (handle_ != nullptr) sqlite3_close(handle_);
    }

    bool open(
        const std::filesystem::path& path,
        int flags,
        std::string& error) {
        const int result =
            sqlite3_open_v2(path.c_str(), &handle_, flags, nullptr);
        if (result != SQLITE_OK) {
            error = sqlite_message(handle_, "open database " + path.string());
            return false;
        }
        sqlite3_extended_result_codes(handle_, 1);
        sqlite3_busy_timeout(handle_, 5'000);
        return execute("PRAGMA foreign_keys = ON;", error);
    }

    bool execute(std::string_view sql, std::string& error) {
        char* detail = nullptr;
        const int result = sqlite3_exec(
            handle_, std::string(sql).c_str(), nullptr, nullptr, &detail);
        if (result == SQLITE_OK) return true;
        error = "SQLite execute";
        if (detail != nullptr) {
            error += ": ";
            error += detail;
            sqlite3_free(detail);
        } else {
            error += ": ";
            error += sqlite3_errmsg(handle_);
        }
        return false;
    }

    [[nodiscard]] sqlite3* get() const noexcept { return handle_; }

private:
    sqlite3* handle_{};
};

class Statement {
public:
    Statement() = default;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    ~Statement() {
        if (statement_ != nullptr) sqlite3_finalize(statement_);
    }

    bool prepare(Database& database, std::string_view sql, std::string& error) {
        if (sqlite3_prepare_v2(
                database.get(), sql.data(), static_cast<int>(sql.size()),
                &statement_, nullptr) != SQLITE_OK) {
            error = sqlite_message(database.get(), "prepare statement");
            return false;
        }
        return true;
    }

    bool bind_text(int position, std::string_view value, std::string& error) {
        if (sqlite3_bind_text(
                statement_, position, value.data(),
                static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
            error = "bind text parameter failed";
            return false;
        }
        return true;
    }

    bool bind_optional(
        int position,
        const std::optional<std::string>& value,
        std::string& error) {
        if (value) return bind_text(position, *value, error);
        if (sqlite3_bind_null(statement_, position) != SQLITE_OK) {
            error = "bind null parameter failed";
            return false;
        }
        return true;
    }

    bool bind_int64(int position, sqlite3_int64 value, std::string& error) {
        if (sqlite3_bind_int64(statement_, position, value) != SQLITE_OK) {
            error = "bind integer parameter failed";
            return false;
        }
        return true;
    }

    bool bind_null(int position, std::string& error) {
        if (sqlite3_bind_null(statement_, position) != SQLITE_OK) {
            error = "bind null parameter failed";
            return false;
        }
        return true;
    }

    bool step_done(Database& database, std::string& error) {
        const int result = sqlite3_step(statement_);
        if (result == SQLITE_DONE) return true;
        error = sqlite_message(database.get(), "execute prepared statement");
        return false;
    }

    int step() { return sqlite3_step(statement_); }
    sqlite3_int64 integer(int column) const {
        return sqlite3_column_int64(statement_, column);
    }
    std::optional<std::string> optional_text(int column) const {
        if (sqlite3_column_type(statement_, column) == SQLITE_NULL)
            return std::nullopt;
        const auto* text = sqlite3_column_text(statement_, column);
        const int bytes = sqlite3_column_bytes(statement_, column);
        return std::string(
            reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes));
    }
    std::string text(int column) const {
        return optional_text(column).value_or(std::string{});
    }
    void reset() {
        sqlite3_reset(statement_);
        sqlite3_clear_bindings(statement_);
    }

private:
    sqlite3_stmt* statement_{};
};

class Transaction {
public:
    explicit Transaction(Database& database) : database_(database) {}
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction() {
        if (active_) {
            std::string ignored;
            database_.execute("ROLLBACK;", ignored);
        }
    }

    bool begin(std::string& error) {
        if (!database_.execute("BEGIN IMMEDIATE;", error)) return false;
        active_ = true;
        return true;
    }
    bool commit(std::string& error) {
        if (!database_.execute("COMMIT;", error)) return false;
        active_ = false;
        return true;
    }

private:
    Database& database_;
    bool active_{};
};

bool has_table(Database& database, std::string_view name, bool& present, std::string& error) {
    Statement query;
    if (!query.prepare(
            database,
            "SELECT 1 FROM sqlite_schema WHERE type='table' AND name=?1;",
            error) ||
        !query.bind_text(1, name, error))
        return false;
    const int result = query.step();
    if (result != SQLITE_ROW && result != SQLITE_DONE) {
        error = sqlite_message(database.get(), "inspect database schema");
        return false;
    }
    present = result == SQLITE_ROW;
    return true;
}

bool initialize_schema(
    Database& database,
    std::string& search_mode,
    bool& created,
    std::string& error,
    bool allow_migration = true) {
    bool has_schema{};
    if (!has_table(database, "schema_info", has_schema, error)) return false;
    if (has_schema) {
        Statement schema;
        if (!schema.prepare(
                database,
                "SELECT schema_version, search_mode FROM schema_info;",
                error))
            return false;
        if (schema.step() != SQLITE_ROW) {
            error = "schema_info does not contain a schema row";
            return false;
        }
        const sqlite3_int64 version = schema.integer(0);
        search_mode = schema.text(1);
        if (schema.step() != SQLITE_DONE) {
            error = "schema_info must contain exactly one row";
            return false;
        }
        if (version != 1 && version != registry_explorer_schema_version) {
            error = "unsupported registry explorer schema version " +
                std::to_string(version);
            return false;
        }
        if (search_mode != "fts5" && search_mode != "like") {
            error = "invalid registry explorer search mode";
            return false;
        }
        static constexpr std::string_view required_tables[] = {
            "snapshots", "server_versions", "snapshot_server_versions",
            "repositories", "packages", "package_arguments",
            "package_environment", "remotes"};
        for (const std::string_view table : required_tables) {
            bool present{};
            if (!has_table(database, table, present, error)) return false;
            if (!present) {
                error = "incompatible registry explorer schema: missing table " +
                    std::string(table);
                return false;
            }
        }
        if (search_mode == "fts5") {
            bool present{};
            if (!has_table(database, "server_search", present, error)) return false;
            if (!present) {
                error = "incompatible registry explorer schema: missing FTS5 table";
                return false;
            }
        }
        if (version == 1) {
            if (!allow_migration) {
                created = false;
                return true;
            }
            static constexpr std::string_view analysis_sql = R"SQL(
CREATE TABLE analysis_runs(
    id INTEGER PRIMARY KEY,
    server_version_id INTEGER NOT NULL REFERENCES server_versions(id) ON DELETE RESTRICT,
    package_id INTEGER NOT NULL REFERENCES packages(id) ON DELETE RESTRICT,
    analysis_type TEXT NOT NULL CHECK(analysis_type IN ('npm_package_static_v1')),
    status TEXT NOT NULL CHECK(status IN ('running','completed','failed')),
    analyzer_name TEXT NOT NULL,
    analyzer_version TEXT NOT NULL,
    ruleset_version TEXT NOT NULL,
    started_at TEXT NOT NULL,
    completed_at TEXT,
    artifact_sha256 TEXT,
    published_integrity TEXT,
    integrity_verified INTEGER CHECK(integrity_verified IN (0, 1)),
    base_image_ref TEXT,
    base_image_digest TEXT,
    network_mode TEXT,
    container_read_only INTEGER CHECK(container_read_only IN (0, 1)),
    container_user TEXT,
    summary_json TEXT,
    error_stage TEXT,
    error_message TEXT
);
CREATE TABLE analysis_artifacts(
    id INTEGER PRIMARY KEY,
    analysis_run_id INTEGER NOT NULL UNIQUE REFERENCES analysis_runs(id) ON DELETE CASCADE,
    registry_type TEXT NOT NULL,
    package_identifier TEXT NOT NULL,
    package_version TEXT NOT NULL,
    source_url TEXT NOT NULL,
    local_relative_path TEXT NOT NULL,
    byte_size INTEGER NOT NULL CHECK(byte_size >= 0),
    sha256 TEXT NOT NULL,
    published_integrity TEXT NOT NULL,
    integrity_verified INTEGER NOT NULL CHECK(integrity_verified IN (0, 1)),
    downloaded_at TEXT NOT NULL
);
CREATE TABLE analysis_findings(
    id INTEGER PRIMARY KEY,
    analysis_run_id INTEGER NOT NULL REFERENCES analysis_runs(id) ON DELETE CASCADE,
    rule_id TEXT NOT NULL,
    category TEXT NOT NULL,
    severity TEXT NOT NULL CHECK(severity IN ('info','low','medium','high','critical')),
    confidence TEXT NOT NULL CHECK(confidence IN ('low','medium','high')),
    disposition TEXT NOT NULL CHECK(disposition IN (
        'unreviewed','expected','reviewed-benign','mitigated',
        'suspicious','confirmed-risk','false-positive')),
    subject_path TEXT NOT NULL,
    line_number INTEGER CHECK(line_number IS NULL OR line_number > 0),
    symbol TEXT,
    title TEXT NOT NULL,
    evidence TEXT,
    explanation TEXT NOT NULL
);
CREATE TABLE analysis_files(
    id INTEGER PRIMARY KEY,
    analysis_run_id INTEGER NOT NULL REFERENCES analysis_runs(id) ON DELETE CASCADE,
    archive_path TEXT NOT NULL,
    file_type TEXT NOT NULL,
    byte_size INTEGER NOT NULL CHECK(byte_size >= 0),
    sha256 TEXT NOT NULL,
    executable INTEGER NOT NULL CHECK(executable IN (0, 1)),
    native_binary INTEGER NOT NULL CHECK(native_binary IN (0, 1)),
    generated INTEGER NOT NULL CHECK(generated IN (0, 1)),
    minified INTEGER NOT NULL CHECK(minified IN (0, 1)),
    UNIQUE(analysis_run_id, archive_path)
);
CREATE TABLE analysis_dependencies(
    id INTEGER PRIMARY KEY,
    analysis_run_id INTEGER NOT NULL REFERENCES analysis_runs(id) ON DELETE CASCADE,
    dependency_type TEXT NOT NULL CHECK(dependency_type IN ('runtime','development','peer','optional')),
    dependency_name TEXT NOT NULL,
    declared_version TEXT NOT NULL,
    resolved_version TEXT,
    direct INTEGER NOT NULL CHECK(direct IN (0, 1)),
    development INTEGER NOT NULL CHECK(development IN (0, 1))
);
CREATE TABLE analysis_evidence(
    id INTEGER PRIMARY KEY,
    analysis_run_id INTEGER NOT NULL REFERENCES analysis_runs(id) ON DELETE CASCADE,
    evidence_type TEXT NOT NULL,
    relative_path TEXT NOT NULL,
    sha256 TEXT NOT NULL,
    byte_size INTEGER NOT NULL CHECK(byte_size >= 0),
    media_type TEXT NOT NULL,
    UNIQUE(analysis_run_id, relative_path)
);
CREATE INDEX analysis_runs_package ON analysis_runs(package_id, status);
CREATE INDEX analysis_runs_artifact ON analysis_runs(
    artifact_sha256, analyzer_version, ruleset_version, status);
CREATE INDEX analysis_findings_run ON analysis_findings(analysis_run_id);
CREATE INDEX analysis_findings_rule ON analysis_findings(rule_id);
CREATE INDEX analysis_files_run ON analysis_files(analysis_run_id);
CREATE INDEX analysis_dependencies_run ON analysis_dependencies(analysis_run_id);
CREATE INDEX analysis_evidence_run ON analysis_evidence(analysis_run_id);
)SQL";
            if (!database.execute(analysis_sql, error)) return false;
            Statement bump;
            if (!bump.prepare(
                    database,
                    "UPDATE schema_info SET schema_version=?1 WHERE singleton=1;",
                    error) ||
                !bump.bind_int64(1, registry_explorer_schema_version, error) ||
                !bump.step_done(database, error))
                return false;
        } else {
            static constexpr std::string_view analysis_tables[] = {
                "analysis_runs", "analysis_artifacts", "analysis_findings",
                "analysis_files", "analysis_dependencies", "analysis_evidence"};
            for (const std::string_view table : analysis_tables) {
                bool present{};
                if (!has_table(database, table, present, error)) return false;
                if (!present) {
                    error = "incompatible registry explorer schema: missing table " +
                        std::string(table);
                    return false;
                }
            }
        }
        created = false;
        return true;
    }

    bool any_table{};
    Statement tables;
    if (!tables.prepare(
            database,
            "SELECT 1 FROM sqlite_schema WHERE type='table' "
            "AND name NOT LIKE 'sqlite_%' LIMIT 1;",
            error))
        return false;
    any_table = tables.step() == SQLITE_ROW;
    if (any_table) {
        error = "database has tables but no compatible schema_info";
        return false;
    }

    static constexpr std::string_view schema_sql = R"SQL(
CREATE TABLE snapshots(
    id INTEGER PRIMARY KEY,
    snapshot_sha256 TEXT NOT NULL UNIQUE,
    completed_at TEXT NOT NULL,
    started_at TEXT,
    registry_base_url TEXT NOT NULL,
    collector_name TEXT,
    collector_version TEXT,
    collector_git_commit TEXT,
    bundle_version INTEGER NOT NULL,
    source_bundle_path TEXT NOT NULL,
    pages INTEGER NOT NULL,
    records_received INTEGER NOT NULL,
    unique_server_versions INTEGER NOT NULL,
    imported_at TEXT NOT NULL
);
CREATE TABLE server_versions(
    id INTEGER PRIMARY KEY,
    server_identifier TEXT NOT NULL,
    server_version TEXT NOT NULL,
    description TEXT,
    registry_status TEXT,
    published_at TEXT,
    updated_at TEXT,
    canonical_sha256 TEXT NOT NULL,
    canonical_json TEXT NOT NULL,
    UNIQUE(server_identifier, server_version, canonical_sha256)
);
CREATE TABLE snapshot_server_versions(
    snapshot_id INTEGER NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
    server_version_id INTEGER NOT NULL REFERENCES server_versions(id) ON DELETE RESTRICT,
    PRIMARY KEY(snapshot_id, server_version_id)
);
CREATE TABLE repositories(
    server_version_id INTEGER PRIMARY KEY REFERENCES server_versions(id) ON DELETE CASCADE,
    source TEXT,
    url TEXT,
    scheme TEXT,
    host TEXT,
    owner TEXT,
    repository_name TEXT
);
CREATE TABLE packages(
    id INTEGER PRIMARY KEY,
    server_version_id INTEGER NOT NULL REFERENCES server_versions(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    registry_type TEXT NOT NULL,
    identifier TEXT NOT NULL,
    version TEXT,
    transport TEXT NOT NULL,
    UNIQUE(server_version_id, position)
);
CREATE TABLE package_arguments(
    package_id INTEGER NOT NULL REFERENCES packages(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    argument_value TEXT,
    PRIMARY KEY(package_id, position)
);
CREATE TABLE package_environment(
    package_id INTEGER NOT NULL REFERENCES packages(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    name TEXT NOT NULL,
    required INTEGER NOT NULL CHECK(required IN (0, 1)),
    description TEXT,
    PRIMARY KEY(package_id, position)
);
CREATE TABLE remotes(
    id INTEGER PRIMARY KEY,
    server_version_id INTEGER NOT NULL REFERENCES server_versions(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    url TEXT NOT NULL,
    scheme TEXT,
    host TEXT,
    port INTEGER,
    transport TEXT NOT NULL,
    UNIQUE(server_version_id, position)
);
CREATE INDEX server_versions_identity
    ON server_versions(server_identifier, server_version);
CREATE INDEX server_versions_status
    ON server_versions(registry_status);
CREATE INDEX snapshot_links_server
    ON snapshot_server_versions(server_version_id, snapshot_id);
CREATE INDEX packages_server ON packages(server_version_id);
CREATE INDEX packages_registry ON packages(registry_type);
CREATE INDEX packages_transport ON packages(transport);
CREATE INDEX packages_identifier ON packages(identifier);
CREATE INDEX repositories_host ON repositories(host);
CREATE INDEX remotes_server ON remotes(server_version_id);
CREATE INDEX remotes_host ON remotes(host);
CREATE INDEX remotes_transport ON remotes(transport);
CREATE TABLE analysis_runs(
    id INTEGER PRIMARY KEY,
    server_version_id INTEGER NOT NULL REFERENCES server_versions(id) ON DELETE RESTRICT,
    package_id INTEGER NOT NULL REFERENCES packages(id) ON DELETE RESTRICT,
    analysis_type TEXT NOT NULL CHECK(analysis_type IN ('npm_package_static_v1')),
    status TEXT NOT NULL CHECK(status IN ('running','completed','failed')),
    analyzer_name TEXT NOT NULL,
    analyzer_version TEXT NOT NULL,
    ruleset_version TEXT NOT NULL,
    started_at TEXT NOT NULL,
    completed_at TEXT,
    artifact_sha256 TEXT,
    published_integrity TEXT,
    integrity_verified INTEGER CHECK(integrity_verified IN (0, 1)),
    base_image_ref TEXT,
    base_image_digest TEXT,
    network_mode TEXT,
    container_read_only INTEGER CHECK(container_read_only IN (0, 1)),
    container_user TEXT,
    summary_json TEXT,
    error_stage TEXT,
    error_message TEXT
);
CREATE TABLE analysis_artifacts(
    id INTEGER PRIMARY KEY,
    analysis_run_id INTEGER NOT NULL UNIQUE REFERENCES analysis_runs(id) ON DELETE CASCADE,
    registry_type TEXT NOT NULL,
    package_identifier TEXT NOT NULL,
    package_version TEXT NOT NULL,
    source_url TEXT NOT NULL,
    local_relative_path TEXT NOT NULL,
    byte_size INTEGER NOT NULL CHECK(byte_size >= 0),
    sha256 TEXT NOT NULL,
    published_integrity TEXT NOT NULL,
    integrity_verified INTEGER NOT NULL CHECK(integrity_verified IN (0, 1)),
    downloaded_at TEXT NOT NULL
);
CREATE TABLE analysis_findings(
    id INTEGER PRIMARY KEY,
    analysis_run_id INTEGER NOT NULL REFERENCES analysis_runs(id) ON DELETE CASCADE,
    rule_id TEXT NOT NULL,
    category TEXT NOT NULL,
    severity TEXT NOT NULL CHECK(severity IN ('info','low','medium','high','critical')),
    confidence TEXT NOT NULL CHECK(confidence IN ('low','medium','high')),
    disposition TEXT NOT NULL CHECK(disposition IN (
        'unreviewed','expected','reviewed-benign','mitigated',
        'suspicious','confirmed-risk','false-positive')),
    subject_path TEXT NOT NULL,
    line_number INTEGER CHECK(line_number IS NULL OR line_number > 0),
    symbol TEXT,
    title TEXT NOT NULL,
    evidence TEXT,
    explanation TEXT NOT NULL
);
CREATE TABLE analysis_files(
    id INTEGER PRIMARY KEY,
    analysis_run_id INTEGER NOT NULL REFERENCES analysis_runs(id) ON DELETE CASCADE,
    archive_path TEXT NOT NULL,
    file_type TEXT NOT NULL,
    byte_size INTEGER NOT NULL CHECK(byte_size >= 0),
    sha256 TEXT NOT NULL,
    executable INTEGER NOT NULL CHECK(executable IN (0, 1)),
    native_binary INTEGER NOT NULL CHECK(native_binary IN (0, 1)),
    generated INTEGER NOT NULL CHECK(generated IN (0, 1)),
    minified INTEGER NOT NULL CHECK(minified IN (0, 1)),
    UNIQUE(analysis_run_id, archive_path)
);
CREATE TABLE analysis_dependencies(
    id INTEGER PRIMARY KEY,
    analysis_run_id INTEGER NOT NULL REFERENCES analysis_runs(id) ON DELETE CASCADE,
    dependency_type TEXT NOT NULL CHECK(dependency_type IN ('runtime','development','peer','optional')),
    dependency_name TEXT NOT NULL,
    declared_version TEXT NOT NULL,
    resolved_version TEXT,
    direct INTEGER NOT NULL CHECK(direct IN (0, 1)),
    development INTEGER NOT NULL CHECK(development IN (0, 1))
);
CREATE TABLE analysis_evidence(
    id INTEGER PRIMARY KEY,
    analysis_run_id INTEGER NOT NULL REFERENCES analysis_runs(id) ON DELETE CASCADE,
    evidence_type TEXT NOT NULL,
    relative_path TEXT NOT NULL,
    sha256 TEXT NOT NULL,
    byte_size INTEGER NOT NULL CHECK(byte_size >= 0),
    media_type TEXT NOT NULL,
    UNIQUE(analysis_run_id, relative_path)
);
CREATE INDEX analysis_runs_package ON analysis_runs(package_id, status);
CREATE INDEX analysis_runs_artifact ON analysis_runs(
    artifact_sha256, analyzer_version, ruleset_version, status);
CREATE INDEX analysis_findings_run ON analysis_findings(analysis_run_id);
CREATE INDEX analysis_findings_rule ON analysis_findings(rule_id);
CREATE INDEX analysis_files_run ON analysis_files(analysis_run_id);
CREATE INDEX analysis_dependencies_run ON analysis_dependencies(analysis_run_id);
CREATE INDEX analysis_evidence_run ON analysis_evidence(analysis_run_id);
)SQL";
    if (!database.execute(schema_sql, error)) return false;

    search_mode = "like";
    std::string fts_error;
    if (database.execute(
            "CREATE VIRTUAL TABLE server_search USING fts5("
            "server_version_id UNINDEXED, server_identifier, description, "
            "package_identifiers, repository_url, remote_urls, remote_hosts, "
            "tokenize='unicode61');",
            fts_error)) {
        search_mode = "fts5";
    }

    if (!database.execute(
            "CREATE TABLE schema_info("
            "singleton INTEGER PRIMARY KEY CHECK(singleton=1), "
            "schema_version INTEGER NOT NULL, "
            "created_by_version TEXT NOT NULL, "
            "search_mode TEXT NOT NULL CHECK(search_mode IN ('fts5','like'))"
            ");",
            error))
        return false;
    Statement insert;
    if (!insert.prepare(
            database,
            "INSERT INTO schema_info("
            "singleton,schema_version,created_by_version,search_mode"
            ") VALUES(1,?1,?2,?3);",
            error) ||
        !insert.bind_int64(1, registry_explorer_schema_version, error) ||
        !insert.bind_text(2, MCPO_VERSION, error) ||
        !insert.bind_text(3, search_mode, error) ||
        !insert.step_done(database, error))
        return false;
    created = true;
    return true;
}

bool verify_foreign_keys(Database& database, std::string& error) {
    Statement pragma;
    if (!pragma.prepare(database, "PRAGMA foreign_keys;", error)) return false;
    if (pragma.step() != SQLITE_ROW || pragma.integer(0) != 1) {
        error = "SQLite foreign key enforcement is not enabled";
        return false;
    }
    return true;
}

bool database_size(
    Database& database,
    std::uint64_t& bytes,
    std::string& error) {
    Statement query;
    if (!query.prepare(
            database, "SELECT page_count * page_size "
            "FROM pragma_page_count(), pragma_page_size();", error))
        return false;
    if (query.step() != SQLITE_ROW || query.integer(0) < 0) {
        error = "cannot determine SQLite database size";
        return false;
    }
    bytes = static_cast<std::uint64_t>(query.integer(0));
    return true;
}

struct UrlParts {
    std::optional<std::string> scheme;
    std::optional<std::string> host;
    std::optional<unsigned> port;
    std::optional<std::string> owner;
    std::optional<std::string> repository;
};

std::string lower_ascii(std::string_view value) {
    std::string result(value);
    for (char& character : result) {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    }
    return result;
}

UrlParts parse_url_parts(std::string_view url, bool derive_repository) {
    UrlParts result;
    const std::size_t separator = url.find("://");
    if (separator == std::string_view::npos || separator == 0U) return result;
    result.scheme = lower_ascii(url.substr(0U, separator));
    const std::size_t authority_begin = separator + 3U;
    const std::size_t authority_end = url.find_first_of("/?#", authority_begin);
    std::string_view authority = url.substr(
        authority_begin,
        authority_end == std::string_view::npos ?
            url.size() - authority_begin : authority_end - authority_begin);
    if (authority.empty() || authority.find('@') != std::string_view::npos)
        return result;
    std::string_view host = authority;
    if (authority.front() == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos) return result;
        host = authority.substr(0U, close + 1U);
        if (close + 1U < authority.size()) {
            if (authority[close + 1U] != ':') return result;
            const std::string_view port_text = authority.substr(close + 2U);
            unsigned port{};
            const auto parsed =
                std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != port_text.data() + port_text.size() || port > 65'535U)
                return result;
            result.port = port;
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            host = authority.substr(0U, colon);
            const std::string_view port_text = authority.substr(colon + 1U);
            unsigned port{};
            const auto parsed =
                std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != port_text.data() + port_text.size() || port > 65'535U)
                return result;
            result.port = port;
        }
    }
    if (host.empty()) return result;
    result.host = lower_ascii(host);
    if (!derive_repository ||
        (*result.host != "github.com" && *result.host != "gitlab.com") ||
        authority_end == std::string_view::npos)
        return result;
    std::string_view path = url.substr(authority_end + 1U);
    const std::size_t path_end = path.find_first_of("?#");
    if (path_end != std::string_view::npos) path = path.substr(0U, path_end);
    const std::size_t slash = path.find('/');
    if (slash == std::string_view::npos || slash == 0U || slash + 1U >= path.size())
        return result;
    const std::string_view owner = path.substr(0U, slash);
    std::string_view repository = path.substr(slash + 1U);
    if (repository.find('/') != std::string_view::npos) return result;
    if (repository.ends_with(".git")) repository.remove_suffix(4U);
    if (repository.empty()) return result;
    result.owner = std::string(owner);
    result.repository = std::string(repository);
    return result;
}

struct ImportStatements {
    Statement exact_identity;
    Statement identity_exists;
    Statement insert_server;
    Statement link;
    Statement repository;
    Statement package;
    Statement argument;
    Statement environment;
    Statement remote;
    Statement fts;

    bool prepare(Database& database, bool fts_enabled, std::string& error) {
        return exact_identity.prepare(
                   database,
                   "SELECT id FROM server_versions "
                   "WHERE server_identifier=?1 AND server_version=?2 "
                   "AND canonical_sha256=?3;",
                   error) &&
            identity_exists.prepare(
                   database,
                   "SELECT 1 FROM server_versions "
                   "WHERE server_identifier=?1 AND server_version=?2 LIMIT 1;",
                   error) &&
            insert_server.prepare(
                database,
                "INSERT INTO server_versions("
                "server_identifier,server_version,description,registry_status,"
                "published_at,updated_at,canonical_sha256,canonical_json"
                ") VALUES(?1,?2,?3,?4,?5,?6,?7,?8);",
                error) &&
            link.prepare(
                database,
                "INSERT INTO snapshot_server_versions(snapshot_id,server_version_id) "
                "VALUES(?1,?2);",
                error) &&
            repository.prepare(
                database,
                "INSERT INTO repositories("
                "server_version_id,source,url,scheme,host,owner,repository_name"
                ") VALUES(?1,?2,?3,?4,?5,?6,?7);",
                error) &&
            package.prepare(
                database,
                "INSERT INTO packages("
                "server_version_id,position,registry_type,identifier,version,transport"
                ") VALUES(?1,?2,?3,?4,?5,?6);",
                error) &&
            argument.prepare(
                database,
                "INSERT INTO package_arguments(package_id,position,argument_value) "
                "VALUES(?1,?2,?3);",
                error) &&
            environment.prepare(
                database,
                "INSERT INTO package_environment("
                "package_id,position,name,required,description"
                ") VALUES(?1,?2,?3,?4,?5);",
                error) &&
            remote.prepare(
                database,
                "INSERT INTO remotes("
                "server_version_id,position,url,scheme,host,port,transport"
                ") VALUES(?1,?2,?3,?4,?5,?6,?7);",
                error) &&
            (!fts_enabled || fts.prepare(
                database,
                "INSERT INTO server_search("
                "server_version_id,server_identifier,description,package_identifiers,"
                "repository_url,remote_urls,remote_hosts"
                ") VALUES(?1,?2,?3,?4,?5,?6,?7);",
                error));
    }
};

bool insert_children(
    Database& database,
    ImportStatements& statements,
    sqlite3_int64 server_id,
    const RegistryCanonicalRecord& record,
    bool fts_enabled,
    std::string& error) {
    std::string package_identifiers;
    std::string remote_urls;
    std::string remote_hosts;
    if (record.repository) {
        const UrlParts parts = record.repository->url ?
            parse_url_parts(*record.repository->url, true) : UrlParts{};
        if (!statements.repository.bind_int64(1, server_id, error) ||
            !statements.repository.bind_optional(
                2, record.repository->source, error) ||
            !statements.repository.bind_optional(
                3, record.repository->url, error) ||
            !statements.repository.bind_optional(4, parts.scheme, error) ||
            !statements.repository.bind_optional(5, parts.host, error) ||
            !statements.repository.bind_optional(6, parts.owner, error) ||
            !statements.repository.bind_optional(7, parts.repository, error) ||
            !statements.repository.step_done(database, error))
            return false;
        statements.repository.reset();
    }
    for (std::size_t position = 0U; position < record.packages.size(); ++position) {
        const RegistryPackageRecord& package = record.packages[position];
        if (!package_identifiers.empty()) package_identifiers.push_back(' ');
        package_identifiers += package.identifier;
        if (!statements.package.bind_int64(1, server_id, error) ||
            !statements.package.bind_int64(
                2, static_cast<sqlite3_int64>(position), error) ||
            !statements.package.bind_text(3, package.registry_type, error) ||
            !statements.package.bind_text(4, package.identifier, error) ||
            !statements.package.bind_optional(5, package.version, error) ||
            !statements.package.bind_text(6, package.transport, error) ||
            !statements.package.step_done(database, error))
            return false;
        statements.package.reset();
        const sqlite3_int64 package_id = sqlite3_last_insert_rowid(database.get());
        for (std::size_t argument_position = 0U;
             argument_position < package.arguments.size(); ++argument_position) {
            if (!statements.argument.bind_int64(1, package_id, error) ||
                !statements.argument.bind_int64(
                    2, static_cast<sqlite3_int64>(argument_position), error) ||
                !statements.argument.bind_optional(
                    3, package.arguments[argument_position].value, error) ||
                !statements.argument.step_done(database, error))
                return false;
            statements.argument.reset();
        }
        for (std::size_t environment_position = 0U;
             environment_position < package.environment.size();
             ++environment_position) {
            const RegistryEnvironmentRecord& declaration =
                package.environment[environment_position];
            if (!statements.environment.bind_int64(1, package_id, error) ||
                !statements.environment.bind_int64(
                    2, static_cast<sqlite3_int64>(environment_position), error) ||
                !statements.environment.bind_text(3, declaration.name, error) ||
                !statements.environment.bind_int64(
                    4, declaration.required ? 1 : 0, error) ||
                !statements.environment.bind_optional(
                    5, declaration.description, error) ||
                !statements.environment.step_done(database, error))
                return false;
            statements.environment.reset();
        }
    }
    for (std::size_t position = 0U; position < record.remotes.size(); ++position) {
        const RegistryRemoteRecord& remote = record.remotes[position];
        const UrlParts parts = parse_url_parts(remote.url, false);
        if (!remote_urls.empty()) remote_urls.push_back(' ');
        remote_urls += remote.url;
        if (parts.host) {
            if (!remote_hosts.empty()) remote_hosts.push_back(' ');
            remote_hosts += *parts.host;
        }
        const std::optional<std::string> port = parts.port ?
            std::optional<std::string>(std::to_string(*parts.port)) : std::nullopt;
        if (!statements.remote.bind_int64(1, server_id, error) ||
            !statements.remote.bind_int64(
                2, static_cast<sqlite3_int64>(position), error) ||
            !statements.remote.bind_text(3, remote.url, error) ||
            !statements.remote.bind_optional(4, parts.scheme, error) ||
            !statements.remote.bind_optional(5, parts.host, error) ||
            !(port ?
                statements.remote.bind_int64(
                    6, static_cast<sqlite3_int64>(*parts.port), error) :
                statements.remote.bind_null(6, error)) ||
            !statements.remote.bind_text(7, remote.transport, error) ||
            !statements.remote.step_done(database, error))
            return false;
        statements.remote.reset();
    }
    if (fts_enabled) {
        const std::optional<std::string> repository_url =
            record.repository ? record.repository->url : std::nullopt;
        if (!statements.fts.bind_int64(1, server_id, error) ||
            !statements.fts.bind_text(2, record.server_identifier, error) ||
            !statements.fts.bind_optional(3, record.description, error) ||
            !statements.fts.bind_text(4, package_identifiers, error) ||
            !statements.fts.bind_optional(5, repository_url, error) ||
            !statements.fts.bind_text(6, remote_urls, error) ||
            !statements.fts.bind_text(7, remote_hosts, error) ||
            !statements.fts.step_done(database, error))
            return false;
        statements.fts.reset();
    }
    return true;
}

}  // namespace

ExplorerResult index_registry_bundle(const RegistryIndexOptions& options) {
    const auto total_started = std::chrono::steady_clock::now();
    if (options.bundle.empty() || options.database.empty() ||
        options.maximum_records == 0U || options.maximum_line_bytes == 0U ||
        options.maximum_database_bytes == 0U ||
        !fits_sqlite_integer(options.maximum_records))
        return failure(ExplorerError::invalid_arguments, "invalid registry index limits or path");

    progress(options.verbose, "validation_start");
    const auto validation_started = std::chrono::steady_clock::now();
    std::string detail;
    if (!validate_bundle(options.bundle, detail)) {
        const ExplorerError category =
            detail.find("canonical") != std::string::npos ?
                ExplorerError::malformed_canonical :
                ExplorerError::bundle_validation;
        return failure(category, detail);
    }
    RegistryBundleManifest manifest;
    if (!read_registry_bundle_manifest(options.bundle, manifest, detail))
        return failure(ExplorerError::bundle_validation, detail);
    if (manifest.bundle_version != 1U && manifest.bundle_version != 2U)
        return failure(
            ExplorerError::bundle_validation,
            "unsupported registry bundle version " +
                std::to_string(manifest.bundle_version));
    if (!fits_sqlite_integer(manifest.bundle_version) ||
        !fits_sqlite_integer(manifest.pages) ||
        !fits_sqlite_integer(manifest.records_received) ||
        !fits_sqlite_integer(manifest.unique_server_versions))
        return failure(
            ExplorerError::bundle_validation,
            "manifest numeric metadata exceeds SQLite integer range");
    if (manifest.unique_server_versions > options.maximum_records)
        return failure(
            ExplorerError::limit_exceeded,
            "manifest unique_server_versions exceeds --maximum-records");
    const auto validation_finished = std::chrono::steady_clock::now();
    progress(options.verbose, "validation_complete");
    progress(
        options.verbose,
        "phase bundle_validation=" +
            elapsed_text(validation_finished - validation_started));

    std::error_code path_error;
    if (std::filesystem::exists(options.database, path_error)) {
        const auto existing_size =
            std::filesystem::file_size(options.database, path_error);
        if (path_error)
            return failure(
                ExplorerError::database,
                "cannot inspect database path: " + options.database.string());
        if (existing_size > options.maximum_database_bytes)
            return failure(
                ExplorerError::database_size_exceeded,
                "database already exceeds --maximum-database-bytes");
    }

    Database database;
    if (!database.open(
            options.database,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
            detail))
        return failure(ExplorerError::database, detail);
    if (!verify_foreign_keys(database, detail))
        return failure(ExplorerError::database, detail);

    Transaction transaction(database);
    if (!transaction.begin(detail))
        return failure(ExplorerError::database, detail);
    const auto schema_started = std::chrono::steady_clock::now();
    std::string search_mode;
    bool schema_created{};
    if (!initialize_schema(
            database, search_mode, schema_created, detail)) {
        const ExplorerError category =
            detail.find("schema") != std::string::npos ?
                ExplorerError::incompatible_schema : ExplorerError::database;
        return failure(category, detail);
    }
    const auto schema_finished = std::chrono::steady_clock::now();
    progress(
        options.verbose,
        "phase schema_initialization=" +
            elapsed_text(schema_finished - schema_started));

    Statement existing;
    if (!existing.prepare(
            database,
            "SELECT id FROM snapshots WHERE snapshot_sha256=?1;",
            detail) ||
        !existing.bind_text(1, manifest.snapshot_sha256, detail))
        return failure(ExplorerError::database, detail);
    const int existing_result = existing.step();
    if (existing_result == SQLITE_ROW) {
        const sqlite3_int64 snapshot_id = existing.integer(0);
        if (!transaction.commit(detail))
            return failure(ExplorerError::database, detail);
        std::ostringstream output;
        output << "registry snapshot already indexed\n"
               << "snapshot_sha256=" << manifest.snapshot_sha256 << '\n'
               << "snapshot_id=" << snapshot_id << '\n'
               << "database=" << options.database.string() << '\n';
        RegistryIndexStats stats;
        stats.snapshot_already_indexed = true;
        return {ExplorerError::none, output.str(), stats};
    }
    if (existing_result != SQLITE_DONE)
        return failure(
            ExplorerError::database,
            sqlite_message(database.get(), "query existing snapshot"));

    Statement insert_snapshot;
    if (!insert_snapshot.prepare(
            database,
            "INSERT INTO snapshots("
            "snapshot_sha256,completed_at,started_at,registry_base_url,"
            "collector_name,collector_version,collector_git_commit,bundle_version,"
            "source_bundle_path,pages,records_received,unique_server_versions,imported_at"
            ") VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13);",
            detail))
        return failure(ExplorerError::database, detail);
    const std::filesystem::path absolute_bundle =
        std::filesystem::weakly_canonical(options.bundle, path_error);
    if (path_error)
        return failure(
            ExplorerError::bundle_validation,
            "cannot resolve source bundle path");
    if (!insert_snapshot.bind_text(1, manifest.snapshot_sha256, detail) ||
        !insert_snapshot.bind_text(2, manifest.completed_at, detail) ||
        !insert_snapshot.bind_text(3, manifest.started_at, detail) ||
        !insert_snapshot.bind_text(4, manifest.registry_base_url, detail) ||
        !insert_snapshot.bind_optional(5, manifest.collector_name, detail) ||
        !insert_snapshot.bind_optional(6, manifest.collector_version, detail) ||
        !insert_snapshot.bind_optional(7, manifest.collector_git_commit, detail) ||
        !insert_snapshot.bind_int64(
            8, static_cast<sqlite3_int64>(manifest.bundle_version), detail) ||
        !insert_snapshot.bind_text(9, absolute_bundle.string(), detail) ||
        !insert_snapshot.bind_int64(
            10, static_cast<sqlite3_int64>(manifest.pages), detail) ||
        !insert_snapshot.bind_int64(
            11, static_cast<sqlite3_int64>(manifest.records_received), detail) ||
        !insert_snapshot.bind_int64(
            12, static_cast<sqlite3_int64>(manifest.unique_server_versions), detail) ||
        !insert_snapshot.bind_text(13, utc_now(), detail) ||
        !insert_snapshot.step_done(database, detail))
        return failure(ExplorerError::database, detail);
    const sqlite3_int64 snapshot_id = sqlite3_last_insert_rowid(database.get());

    ImportStatements statements;
    const bool fts_enabled = search_mode == "fts5";
    if (!statements.prepare(database, fts_enabled, detail))
        return failure(ExplorerError::database, detail);

    std::ifstream input(
        options.bundle / "canonical/servers.jsonl",
        std::ios::binary);
    if (!input)
        return failure(
            ExplorerError::malformed_canonical,
            "cannot open canonical/servers.jsonl");
    const auto import_started = std::chrono::steady_clock::now();
    std::size_t imported{};
    std::size_t unique_names{};
    std::size_t repositories{};
    std::size_t packages{};
    std::size_t remotes{};
    std::size_t inserted_server_versions{};
    std::size_t reused_server_versions{};
    std::size_t changed_identity_records{};
    std::size_t snapshot_links_created{};
    std::string previous_name;
    std::string line;
    while (std::getline(input, line)) {
        ++imported;
        if (imported > options.maximum_records)
            return failure(
                ExplorerError::limit_exceeded,
                "canonical record count exceeds --maximum-records at line " +
                    std::to_string(imported));
        if (line.empty())
            return failure(
                ExplorerError::malformed_canonical,
                "canonical/servers.jsonl line " + std::to_string(imported) +
                    " is empty");
        if (line.size() > options.maximum_line_bytes)
            return failure(
                ExplorerError::malformed_canonical,
                "canonical/servers.jsonl line " + std::to_string(imported) +
                    " exceeds --maximum-line-bytes");
        RegistryCanonicalRecord record;
        if (!parse_registry_canonical_record(line, record, detail))
            return failure(
                ExplorerError::malformed_canonical,
                "canonical/servers.jsonl line " + std::to_string(imported) +
                    ": " + detail);
        if (record.server_identifier != previous_name) {
            ++unique_names;
            previous_name = record.server_identifier;
        }
        repositories += record.repository ? 1U : 0U;
        packages += record.packages.size();
        remotes += record.remotes.size();

        statements.exact_identity.reset();
        if (!statements.exact_identity.bind_text(
                1, record.server_identifier, detail) ||
            !statements.exact_identity.bind_text(
                2, record.server_version, detail) ||
            !statements.exact_identity.bind_text(
                3, record.canonical_sha256, detail))
            return failure(ExplorerError::database, detail);
        const int identity_result = statements.exact_identity.step();
        sqlite3_int64 server_id{};
        bool is_new = identity_result == SQLITE_DONE;
        if (identity_result == SQLITE_ROW) {
            server_id = statements.exact_identity.integer(0);
            ++reused_server_versions;
        } else if (identity_result != SQLITE_DONE) {
            return failure(
                ExplorerError::database,
                sqlite_message(database.get(), "query exact canonical identity"));
        }

        if (is_new) {
            statements.identity_exists.reset();
            if (!statements.identity_exists.bind_text(
                    1, record.server_identifier, detail) ||
                !statements.identity_exists.bind_text(
                    2, record.server_version, detail))
                return failure(ExplorerError::database, detail);
            const int exists_result = statements.identity_exists.step();
            if (exists_result == SQLITE_ROW) {
                ++changed_identity_records;
            } else if (exists_result != SQLITE_DONE) {
                return failure(
                    ExplorerError::database,
                    sqlite_message(database.get(), "query canonical identity"));
            }
            ++inserted_server_versions;
            statements.insert_server.reset();
            if (!statements.insert_server.bind_text(
                    1, record.server_identifier, detail) ||
                !statements.insert_server.bind_text(
                    2, record.server_version, detail) ||
                !statements.insert_server.bind_optional(
                    3, record.description, detail) ||
                !statements.insert_server.bind_optional(
                    4, record.registry_status, detail) ||
                !statements.insert_server.bind_optional(
                    5, record.published_at, detail) ||
                !statements.insert_server.bind_optional(
                    6, record.updated_at, detail) ||
                !statements.insert_server.bind_text(
                    7, record.canonical_sha256, detail) ||
                !statements.insert_server.bind_text(8, line, detail) ||
                !statements.insert_server.step_done(database, detail))
                return failure(ExplorerError::database, detail);
            server_id = sqlite3_last_insert_rowid(database.get());
            if (!insert_children(
                    database, statements, server_id, record, fts_enabled, detail))
                return failure(
                    ExplorerError::database,
                    "import children for " + record.server_identifier + "@" +
                        record.server_version + ": " + detail);
        }
        statements.link.reset();
        if (!statements.link.bind_int64(1, snapshot_id, detail) ||
            !statements.link.bind_int64(2, server_id, detail) ||
            !statements.link.step_done(database, detail))
            return failure(
                ExplorerError::malformed_canonical,
                "duplicate exact identity at canonical line " +
                    std::to_string(imported) + ": " + detail);
        ++snapshot_links_created;

        if (options.verbose && imported % 10'000U == 0U)
            progress(
                true,
                "import records=" + std::to_string(imported) + "/" +
                    std::to_string(manifest.unique_server_versions));
        if (imported % 1'000U == 0U) {
            std::uint64_t current_size{};
            if (!database_size(database, current_size, detail))
                return failure(ExplorerError::database, detail);
            if (current_size > options.maximum_database_bytes)
                return failure(
                    ExplorerError::database_size_exceeded,
                    "database exceeds --maximum-database-bytes during import");
        }
    }
    if (input.bad())
        return failure(
            ExplorerError::malformed_canonical,
            "read failure in canonical/servers.jsonl after line " +
                std::to_string(imported));
    if (imported != manifest.unique_server_versions)
        return failure(
            ExplorerError::malformed_canonical,
            "manifest count mismatch: imported " + std::to_string(imported) +
                " canonical records, expected " +
                std::to_string(manifest.unique_server_versions));

    const auto import_finished = std::chrono::steady_clock::now();
    progress(
        options.verbose,
        "phase record_import=" + elapsed_text(import_finished - import_started));
    const auto finalization_started = std::chrono::steady_clock::now();
    Statement linked;
    if (!linked.prepare(
            database,
            "SELECT COUNT(*) FROM snapshot_server_versions WHERE snapshot_id=?1;",
            detail) ||
        !linked.bind_int64(1, snapshot_id, detail) ||
        linked.step() != SQLITE_ROW ||
        linked.integer(0) != static_cast<sqlite3_int64>(manifest.unique_server_versions))
        return failure(
            ExplorerError::database,
            "snapshot relationship count does not match manifest");

    std::uint64_t final_size{};
    if (!database_size(database, final_size, detail))
        return failure(ExplorerError::database, detail);
    if (final_size > options.maximum_database_bytes)
        return failure(
            ExplorerError::database_size_exceeded,
            "database exceeds --maximum-database-bytes before commit");
    const auto finalization_finished = std::chrono::steady_clock::now();
    progress(
        options.verbose,
        "phase index_finalization=" +
            elapsed_text(finalization_finished - finalization_started));
    progress(options.verbose, "transaction_commit");
    const auto commit_started = std::chrono::steady_clock::now();
    if (!transaction.commit(detail))
        return failure(ExplorerError::database, detail);
    const auto commit_finished = std::chrono::steady_clock::now();
    progress(
        options.verbose,
        "phase transaction_commit=" +
            elapsed_text(commit_finished - commit_started));
    progress(
        options.verbose,
        "phase total_indexing=" +
            elapsed_text(commit_finished - total_started));

    std::ostringstream output;
    output << "registry index complete\n"
           << "snapshot_id=" << snapshot_id << '\n'
           << "snapshot_sha256=" << manifest.snapshot_sha256 << '\n'
           << "records=" << imported << '\n'
           << "inserted_server_versions=" << inserted_server_versions << '\n'
           << "reused_server_versions=" << reused_server_versions << '\n'
           << "changed_identity_records=" << changed_identity_records << '\n'
           << "snapshot_links_created=" << snapshot_links_created << '\n'
           << "unique_server_names=" << unique_names << '\n'
           << "repositories=" << repositories << '\n'
           << "packages=" << packages << '\n'
           << "remotes=" << remotes << '\n'
           << "database=" << options.database.string() << '\n';
    return {
        ExplorerError::none,
        output.str(),
        RegistryIndexStats{
            inserted_server_versions,
            reused_server_versions,
            changed_identity_records,
            snapshot_links_created,
            false}};
}

namespace {

struct SelectedSnapshot {
    sqlite3_int64 id{};
    std::string digest;
    std::string completed_at;
};

ExplorerResult open_catalog(
    const std::filesystem::path& path,
    Database& database,
    std::string& search_mode) {
    std::string error;
    if (!database.open(path, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, error))
        return failure(ExplorerError::database, error);
    bool created{};
    if (!initialize_schema(database, search_mode, created, error, false) || created)
        return failure(ExplorerError::incompatible_schema, error);
    if (!verify_foreign_keys(database, error))
        return failure(ExplorerError::database, error);
    return {};
}

ExplorerResult select_snapshot(
    Database& database,
    const SnapshotSelection& selection,
    SelectedSnapshot& snapshot) {
    std::string error;
    Statement query;
    const std::string_view sql = selection.digest ?
        "SELECT id,snapshot_sha256,completed_at FROM snapshots "
        "WHERE snapshot_sha256=?1;" :
        "SELECT id,snapshot_sha256,completed_at FROM snapshots "
        "ORDER BY completed_at COLLATE BINARY DESC,id DESC LIMIT 1;";
    if (!query.prepare(database, sql, error) ||
        (selection.digest && !query.bind_text(1, *selection.digest, error)))
        return failure(ExplorerError::database, error);
    const int result = query.step();
    if (result == SQLITE_DONE)
        return failure(
            ExplorerError::snapshot_not_found,
            selection.digest ?
                "registry snapshot not found: " + *selection.digest :
                "registry catalog contains no snapshots");
    if (result != SQLITE_ROW)
        return failure(
            ExplorerError::database,
            sqlite_message(database.get(), "select registry snapshot"));
    snapshot.id = query.integer(0);
    snapshot.digest = query.text(1);
    snapshot.completed_at = query.text(2);
    return {};
}

std::string json_string(std::string_view value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result{"\""};
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        switch (character) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (character < 0x20U) {
                    result += "\\u00";
                    result.push_back(hex[character >> 4U]);
                    result.push_back(hex[character & 0x0fU]);
                } else {
                    result.push_back(static_cast<char>(character));
                }
        }
    }
    result.push_back('"');
    return result;
}

std::string json_optional(const std::optional<std::string>& value) {
    return value ? json_string(*value) : "null";
}

std::string text_value(std::string_view value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string output;
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        switch (character) {
            case '\\': output += "\\\\"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (character < 0x20U || character == 0x7fU) {
                    output += "\\x";
                    output.push_back(hex[character >> 4U]);
                    output.push_back(hex[character & 0x0fU]);
                } else {
                    output.push_back(static_cast<char>(character));
                }
        }
    }
    return output;
}

std::string text_optional(const std::optional<std::string>& value) {
    return value ? text_value(*value) : std::string{};
}

bool scalar_count(
    Database& database,
    std::string_view sql,
    sqlite3_int64 snapshot_id,
    sqlite3_int64& value,
    std::string& error) {
    Statement query;
    if (!query.prepare(database, sql, error) ||
        !query.bind_int64(1, snapshot_id, error))
        return false;
    if (query.step() != SQLITE_ROW) {
        error = sqlite_message(database.get(), "read summary count");
        return false;
    }
    value = query.integer(0);
    return true;
}

struct GroupCount {
    std::string value;
    sqlite3_int64 count{};
};

bool grouped_counts(
    Database& database,
    std::string_view sql,
    sqlite3_int64 snapshot_id,
    std::vector<GroupCount>& output,
    std::string& error) {
    Statement query;
    if (!query.prepare(database, sql, error) ||
        !query.bind_int64(1, snapshot_id, error))
        return false;
    while (true) {
        const int result = query.step();
        if (result == SQLITE_DONE) return true;
        if (result != SQLITE_ROW) {
            error = sqlite_message(database.get(), "read grouped summary");
            return false;
        }
        output.push_back({query.text(0), query.integer(1)});
    }
}

bool contradictory(const RegistryFilters& filters) {
    return filters.has_repository && filters.without_repository;
}

void add_filter_sql(
    const RegistryFilters& filters,
    std::string& sql,
    std::vector<std::string>& parameters) {
    const auto add = [&](std::string_view clause, const std::optional<std::string>& value) {
        if (!value) return;
        sql += clause;
        parameters.push_back(*value);
    };
    add(" AND sv.registry_status=?", filters.status);
    if (filters.transport) {
        sql += " AND (EXISTS(SELECT 1 FROM packages fp WHERE "
               "fp.server_version_id=sv.id AND fp.transport=?) OR "
               "EXISTS(SELECT 1 FROM remotes fr WHERE "
               "fr.server_version_id=sv.id AND fr.transport=?))";
        parameters.push_back(*filters.transport);
        parameters.push_back(*filters.transport);
    }
    add(
        " AND EXISTS(SELECT 1 FROM packages fp WHERE "
        "fp.server_version_id=sv.id AND fp.registry_type=?)",
        filters.package_registry);
    if (filters.repository_host) {
        sql += " AND EXISTS(SELECT 1 FROM repositories frepo WHERE "
               "frepo.server_version_id=sv.id AND frepo.host=?)";
        parameters.push_back(lower_ascii(*filters.repository_host));
    }
    if (filters.remote_host) {
        sql += " AND EXISTS(SELECT 1 FROM remotes frhost WHERE "
               "frhost.server_version_id=sv.id AND frhost.host=?)";
        parameters.push_back(lower_ascii(*filters.remote_host));
    }
    if (filters.has_repository)
        sql += " AND EXISTS(SELECT 1 FROM repositories fhr WHERE "
               "fhr.server_version_id=sv.id)";
    if (filters.without_repository)
        sql += " AND NOT EXISTS(SELECT 1 FROM repositories fwr WHERE "
               "fwr.server_version_id=sv.id)";
    if (filters.has_package)
        sql += " AND EXISTS(SELECT 1 FROM packages fhp WHERE "
               "fhp.server_version_id=sv.id)";
    if (filters.has_remote)
        sql += " AND EXISTS(SELECT 1 FROM remotes fhm WHERE "
               "fhm.server_version_id=sv.id)";
}

bool bind_strings(
    Statement& statement,
    int start,
    const std::vector<std::string>& values,
    std::string& error) {
    for (std::size_t index = 0U; index < values.size(); ++index) {
        if (!statement.bind_text(
                start + static_cast<int>(index), values[index], error))
            return false;
    }
    return true;
}

std::string escape_like(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 8U);
    for (const char character : value) {
        if (character == '\\' || character == '%' || character == '_')
            escaped.push_back('\\');
        escaped.push_back(character);
    }
    return escaped;
}

std::string fts_query(std::string_view value) {
    std::string output;
    std::string token;
    const auto flush = [&]() {
        if (token.empty()) return;
        if (!output.empty()) output += " AND ";
        output.push_back('"');
        for (const char character : token) {
            if (character == '"') output += "\"\"";
            else output.push_back(character);
        }
        output += "\"*";
        token.clear();
    };
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        if ((character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character >= 0x80U) {
            token.push_back(static_cast<char>(character));
        } else {
            flush();
        }
    }
    flush();
    return output;
}

struct Row {
    std::string name;
    std::string version;
    std::optional<std::string> status;
    std::optional<std::string> description;
    std::optional<std::string> repository;
    sqlite3_int64 packages{};
    sqlite3_int64 remotes{};
};

std::string render_rows(const std::vector<Row>& rows, RowsFormat format) {
    std::ostringstream output;
    for (std::size_t index = 0U; index < rows.size(); ++index) {
        const Row& row = rows[index];
        if (format == RowsFormat::jsonl) {
            output << "{\"server_identifier\":" << json_string(row.name)
                   << ",\"server_version\":" << json_string(row.version)
                   << ",\"status\":" << json_optional(row.status)
                   << ",\"description\":" << json_optional(row.description)
                   << ",\"repository\":" << json_optional(row.repository)
                   << ",\"packages\":" << row.packages
                   << ",\"remotes\":" << row.remotes << "}\n";
        } else {
            if (index != 0U) output << '\n';
            output << text_value(row.name) << '@' << text_value(row.version) << '\n'
                   << "status=" << text_optional(row.status) << '\n'
                   << "description=" << text_optional(row.description) << '\n'
                   << "repository=" << text_optional(row.repository) << '\n'
                   << "packages=" << row.packages << '\n'
                   << "remotes=" << row.remotes << '\n';
        }
    }
    return output.str();
}

ExplorerResult execute_rows(
    Database& database,
    std::string sql,
    sqlite3_int64 snapshot_id,
    std::vector<std::string> parameters,
    std::size_t limit,
    std::size_t offset,
    RowsFormat format) {
    sql += " LIMIT ? OFFSET ?;";
    std::string error;
    Statement query;
    if (!query.prepare(database, sql, error) ||
        !query.bind_int64(1, snapshot_id, error) ||
        !bind_strings(query, 2, parameters, error) ||
        !query.bind_int64(
            static_cast<int>(parameters.size()) + 2,
            static_cast<sqlite3_int64>(limit), error) ||
        !query.bind_int64(
            static_cast<int>(parameters.size()) + 3,
            static_cast<sqlite3_int64>(offset), error))
        return failure(ExplorerError::database, error);
    std::vector<Row> rows;
    while (true) {
        const int result = query.step();
        if (result == SQLITE_DONE) break;
        if (result != SQLITE_ROW)
            return failure(
                ExplorerError::database,
                sqlite_message(database.get(), "query registry rows"));
        rows.push_back({
            query.text(0), query.text(1), query.optional_text(2),
            query.optional_text(3), query.optional_text(4),
            query.integer(5), query.integer(6)});
    }
    return {ExplorerError::none, render_rows(rows, format), std::nullopt};
}

}  // namespace

ExplorerResult latest_registry_snapshot(
    const std::filesystem::path& database_path,
    RegistryBaselineSnapshot& result) {
    Database database;
    std::string search_mode;
    ExplorerResult opened = open_catalog(database_path, database, search_mode);
    if (!opened.ok()) return opened;
    SelectedSnapshot selected;
    ExplorerResult selection =
        select_snapshot(database, SnapshotSelection{}, selected);
    if (!selection.ok()) return selection;
    result.id = selected.id;
    result.snapshot_sha256 = std::move(selected.digest);
    result.completed_at = std::move(selected.completed_at);
    return {};
}

ExplorerResult summarize_registry(
    const std::filesystem::path& database_path,
    const SnapshotSelection& selection,
    SummaryFormat format) {
    Database database;
    std::string search_mode;
    ExplorerResult opened = open_catalog(database_path, database, search_mode);
    if (!opened.ok()) return opened;
    SelectedSnapshot snapshot;
    ExplorerResult selected = select_snapshot(database, selection, snapshot);
    if (!selected.ok()) return selected;

    static constexpr std::string_view count_sql[] = {
        "SELECT COUNT(*) FROM snapshot_server_versions WHERE snapshot_id=?1;",
        "SELECT COUNT(DISTINCT sv.server_identifier) "
        "FROM snapshot_server_versions ssv JOIN server_versions sv "
        "ON sv.id=ssv.server_version_id WHERE ssv.snapshot_id=?1;",
        "SELECT COUNT(*) FROM snapshot_server_versions WHERE snapshot_id=?1;",
        "SELECT COUNT(*) FROM snapshot_server_versions ssv "
        "WHERE ssv.snapshot_id=?1 AND EXISTS(SELECT 1 FROM repositories r "
        "WHERE r.server_version_id=ssv.server_version_id);",
        "SELECT COUNT(*) FROM snapshot_server_versions ssv "
        "WHERE ssv.snapshot_id=?1 AND NOT EXISTS(SELECT 1 FROM repositories r "
        "WHERE r.server_version_id=ssv.server_version_id);",
        "SELECT COUNT(*) FROM snapshot_server_versions ssv "
        "WHERE ssv.snapshot_id=?1 AND EXISTS(SELECT 1 FROM packages p "
        "WHERE p.server_version_id=ssv.server_version_id);",
        "SELECT COUNT(*) FROM snapshot_server_versions ssv "
        "WHERE ssv.snapshot_id=?1 AND EXISTS(SELECT 1 FROM remotes r "
        "WHERE r.server_version_id=ssv.server_version_id);",
        "SELECT COUNT(*) FROM snapshot_server_versions ssv "
        "WHERE ssv.snapshot_id=?1 AND EXISTS(SELECT 1 FROM packages p "
        "WHERE p.server_version_id=ssv.server_version_id) "
        "AND NOT EXISTS(SELECT 1 FROM remotes r "
        "WHERE r.server_version_id=ssv.server_version_id);",
        "SELECT COUNT(*) FROM snapshot_server_versions ssv "
        "WHERE ssv.snapshot_id=?1 AND NOT EXISTS(SELECT 1 FROM packages p "
        "WHERE p.server_version_id=ssv.server_version_id) "
        "AND EXISTS(SELECT 1 FROM remotes r "
        "WHERE r.server_version_id=ssv.server_version_id);",
        "SELECT COUNT(*) FROM snapshot_server_versions ssv "
        "WHERE ssv.snapshot_id=?1 AND EXISTS(SELECT 1 FROM packages p "
        "WHERE p.server_version_id=ssv.server_version_id) "
        "AND EXISTS(SELECT 1 FROM remotes r "
        "WHERE r.server_version_id=ssv.server_version_id);",
        "SELECT COUNT(*) FROM package_environment pe JOIN packages p "
        "ON p.id=pe.package_id JOIN snapshot_server_versions ssv "
        "ON ssv.server_version_id=p.server_version_id "
        "WHERE ssv.snapshot_id=?1;"
    };
    static constexpr std::string_view names[] = {
        "records", "unique_server_names", "versions", "with_repository",
        "without_repository", "with_package", "with_remote", "package_only",
        "remote_only", "package_and_remote", "declared_environment_variables"
    };
    std::vector<sqlite3_int64> counts;
    std::string error;
    for (const std::string_view sql : count_sql) {
        sqlite3_int64 value{};
        if (!scalar_count(database, sql, snapshot.id, value, error))
            return failure(ExplorerError::database, error);
        counts.push_back(value);
    }
    struct Group {
        std::string_view name;
        std::string_view sql;
        std::vector<GroupCount> values;
    };
    std::vector<Group> groups = {
        {"status",
         "SELECT sv.registry_status,COUNT(*) FROM snapshot_server_versions ssv "
         "JOIN server_versions sv ON sv.id=ssv.server_version_id "
         "WHERE ssv.snapshot_id=?1 AND sv.registry_status IS NOT NULL "
         "GROUP BY sv.registry_status ORDER BY sv.registry_status COLLATE BINARY;", {}},
        {"package_registry",
         "SELECT p.registry_type,COUNT(*) FROM snapshot_server_versions ssv "
         "JOIN packages p ON p.server_version_id=ssv.server_version_id "
         "WHERE ssv.snapshot_id=?1 GROUP BY p.registry_type "
         "ORDER BY p.registry_type COLLATE BINARY;", {}},
        {"package_transport",
         "SELECT p.transport,COUNT(*) FROM snapshot_server_versions ssv "
         "JOIN packages p ON p.server_version_id=ssv.server_version_id "
         "WHERE ssv.snapshot_id=?1 GROUP BY p.transport "
         "ORDER BY p.transport COLLATE BINARY;", {}},
        {"remote_transport",
         "SELECT r.transport,COUNT(*) FROM snapshot_server_versions ssv "
         "JOIN remotes r ON r.server_version_id=ssv.server_version_id "
         "WHERE ssv.snapshot_id=?1 GROUP BY r.transport "
         "ORDER BY r.transport COLLATE BINARY;", {}},
        {"repository_host",
         "SELECT r.host,COUNT(*) FROM snapshot_server_versions ssv "
         "JOIN repositories r ON r.server_version_id=ssv.server_version_id "
         "WHERE ssv.snapshot_id=?1 AND r.host IS NOT NULL GROUP BY r.host "
         "ORDER BY r.host COLLATE BINARY;", {}},
        {"remote_host",
         "SELECT r.host,COUNT(*) FROM snapshot_server_versions ssv "
         "JOIN remotes r ON r.server_version_id=ssv.server_version_id "
         "WHERE ssv.snapshot_id=?1 AND r.host IS NOT NULL GROUP BY r.host "
         "ORDER BY r.host COLLATE BINARY;", {}},
    };
    for (Group& group : groups) {
        if (!grouped_counts(
                database, group.sql, snapshot.id, group.values, error))
            return failure(ExplorerError::database, error);
    }

    std::ostringstream output;
    if (format == SummaryFormat::text) {
        output << "snapshot_id=" << snapshot.id << '\n'
               << "snapshot_sha256=" << text_value(snapshot.digest) << '\n'
               << "completed_at=" << text_value(snapshot.completed_at) << '\n';
        for (std::size_t index = 0U; index < counts.size(); ++index)
            output << names[index] << '=' << counts[index] << '\n';
        for (const Group& group : groups) {
            for (const GroupCount& value : group.values)
                output << group.name << '.' << text_value(value.value) << '='
                       << value.count << '\n';
        }
    } else {
        output << "{\"snapshot_id\":" << snapshot.id
               << ",\"snapshot_sha256\":" << json_string(snapshot.digest)
               << ",\"completed_at\":" << json_string(snapshot.completed_at);
        for (std::size_t index = 0U; index < counts.size(); ++index)
            output << ",\"" << names[index] << "\":" << counts[index];
        for (const Group& group : groups) {
            output << ",\"" << group.name << "\":{";
            for (std::size_t index = 0U; index < group.values.size(); ++index) {
                if (index != 0U) output << ',';
                output << json_string(group.values[index].value) << ':'
                       << group.values[index].count;
            }
            output << '}';
        }
        output << "}\n";
    }
    return {ExplorerError::none, output.str(), std::nullopt};
}

ExplorerResult search_registry(const RegistrySearchOptions& options) {
    if (options.query.empty())
        return failure(ExplorerError::invalid_arguments, "search query must not be empty");
    if (options.limit == 0U || options.limit > maximum_search_limit)
        return failure(
            ExplorerError::limit_exceeded,
            "search limit must be between 1 and 500");
    if (!fits_sqlite_integer(options.offset))
        return failure(
            ExplorerError::limit_exceeded,
            "search offset exceeds SQLite integer range");
    if (contradictory(options.filters))
        return failure(
            ExplorerError::invalid_arguments,
            "--has-repository contradicts --without-repository");
    Database database;
    std::string search_mode;
    ExplorerResult opened = open_catalog(options.database, database, search_mode);
    if (!opened.ok()) return opened;
    SelectedSnapshot snapshot;
    ExplorerResult selected =
        select_snapshot(database, options.snapshot, snapshot);
    if (!selected.ok()) return selected;
    const bool use_fts = search_mode == "fts5" && !fts_query(options.query).empty();
    if (options.verbose)
        std::cerr << "[registry-search] search_mode="
                  << (use_fts ? "fts5" : "like") << '\n';

    std::string sql =
        "SELECT sv.server_identifier,sv.server_version,sv.registry_status,"
        "sv.description,repo.url,"
        "(SELECT COUNT(*) FROM packages pc WHERE pc.server_version_id=sv.id),"
        "(SELECT COUNT(*) FROM remotes rc WHERE rc.server_version_id=sv.id) "
        "FROM snapshot_server_versions ssv JOIN server_versions sv "
        "ON sv.id=ssv.server_version_id "
        "LEFT JOIN repositories repo ON repo.server_version_id=sv.id ";
    std::vector<std::string> parameters;
    if (use_fts) {
        sql += "JOIN server_search ON server_search.server_version_id=sv.id "
               "WHERE ssv.snapshot_id=? AND server_search MATCH ?";
        parameters.push_back(fts_query(options.query));
    } else {
        sql += "WHERE ssv.snapshot_id=? AND ("
               "sv.server_identifier LIKE ? ESCAPE '\\' OR "
               "COALESCE(sv.description,'') LIKE ? ESCAPE '\\' OR "
               "EXISTS(SELECT 1 FROM packages sp WHERE sp.server_version_id=sv.id "
               "AND sp.identifier LIKE ? ESCAPE '\\') OR "
               "COALESCE(repo.url,'') LIKE ? ESCAPE '\\' OR "
               "EXISTS(SELECT 1 FROM remotes sr WHERE sr.server_version_id=sv.id "
               "AND (sr.url LIKE ? ESCAPE '\\' OR sr.host LIKE ? ESCAPE '\\')))";
        const std::string pattern = "%" + escape_like(options.query) + "%";
        for (int index = 0; index < 6; ++index) parameters.push_back(pattern);
    }
    add_filter_sql(options.filters, sql, parameters);
    sql += " ORDER BY CASE WHEN sv.server_identifier=? THEN 0 ELSE 1 END,"
           "CASE WHEN sv.server_identifier LIKE ? ESCAPE '\\' THEN 0 ELSE 1 END,";
    parameters.push_back(options.query);
    parameters.push_back(escape_like(options.query) + "%");
    if (use_fts) sql += "bm25(server_search),";
    sql += "sv.server_identifier COLLATE BINARY,sv.server_version COLLATE BINARY";
    return execute_rows(
        database, std::move(sql), snapshot.id, std::move(parameters),
        options.limit, options.offset, options.format);
}

ExplorerResult list_registry(const RegistryListOptions& options) {
    if (options.limit == 0U || options.limit > maximum_list_limit)
        return failure(
            ExplorerError::limit_exceeded,
            "list limit must be between 1 and 1000");
    if (!fits_sqlite_integer(options.offset))
        return failure(
            ExplorerError::limit_exceeded,
            "list offset exceeds SQLite integer range");
    if (contradictory(options.filters))
        return failure(
            ExplorerError::invalid_arguments,
            "--has-repository contradicts --without-repository");
    Database database;
    std::string search_mode;
    ExplorerResult opened = open_catalog(options.database, database, search_mode);
    if (!opened.ok()) return opened;
    SelectedSnapshot snapshot;
    ExplorerResult selected =
        select_snapshot(database, options.snapshot, snapshot);
    if (!selected.ok()) return selected;
    std::string sql =
        "SELECT sv.server_identifier,sv.server_version,sv.registry_status,"
        "sv.description,repo.url,"
        "(SELECT COUNT(*) FROM packages pc WHERE pc.server_version_id=sv.id),"
        "(SELECT COUNT(*) FROM remotes rc WHERE rc.server_version_id=sv.id) "
        "FROM snapshot_server_versions ssv JOIN server_versions sv "
        "ON sv.id=ssv.server_version_id "
        "LEFT JOIN repositories repo ON repo.server_version_id=sv.id "
        "WHERE ssv.snapshot_id=?";
    std::vector<std::string> parameters;
    add_filter_sql(options.filters, sql, parameters);
    sql += " ORDER BY sv.server_identifier COLLATE BINARY,"
           "sv.server_version COLLATE BINARY";
    return execute_rows(
        database, std::move(sql), snapshot.id, std::move(parameters),
        options.limit, options.offset, options.format);
}

namespace {

struct ShowPackage {
    std::string registry;
    std::string identifier;
    std::optional<std::string> version;
    std::string transport;
    std::vector<std::optional<std::string>> arguments;
    std::vector<RegistryEnvironmentRecord> environment;
};

struct ShowRemote {
    std::string url;
    std::string transport;
};

struct ShowRecord {
    Row row;
    std::optional<std::string> published_at;
    std::optional<std::string> updated_at;
    std::string canonical_sha256;
    std::string canonical_json;
    std::vector<ShowPackage> packages;
    std::vector<ShowRemote> remotes;
};

bool load_show_children(
    Database& database,
    sqlite3_int64 server_id,
    ShowRecord& record,
    std::string& error) {
    Statement packages;
    if (!packages.prepare(
            database,
            "SELECT id,registry_type,identifier,version,transport FROM packages "
            "WHERE server_version_id=?1 ORDER BY position;",
            error) ||
        !packages.bind_int64(1, server_id, error))
        return false;
    while (true) {
        const int package_result = packages.step();
        if (package_result == SQLITE_DONE) break;
        if (package_result != SQLITE_ROW) {
            error = sqlite_message(database.get(), "read packages");
            return false;
        }
        const sqlite3_int64 package_id = packages.integer(0);
        ShowPackage package{
            packages.text(1), packages.text(2), packages.optional_text(3),
            packages.text(4), {}, {}};
        Statement arguments;
        if (!arguments.prepare(
                database,
                "SELECT argument_value FROM package_arguments WHERE package_id=?1 "
                "ORDER BY position;",
                error) ||
            !arguments.bind_int64(1, package_id, error))
            return false;
        while (true) {
            const int argument_result = arguments.step();
            if (argument_result == SQLITE_DONE) break;
            if (argument_result != SQLITE_ROW) {
                error = sqlite_message(database.get(), "read package arguments");
                return false;
            }
            package.arguments.push_back(arguments.optional_text(0));
        }
        Statement environment;
        if (!environment.prepare(
                database,
                "SELECT name,required,description FROM package_environment "
                "WHERE package_id=?1 ORDER BY position;",
                error) ||
            !environment.bind_int64(1, package_id, error))
            return false;
        while (true) {
            const int environment_result = environment.step();
            if (environment_result == SQLITE_DONE) break;
            if (environment_result != SQLITE_ROW) {
                error = sqlite_message(database.get(), "read package environment");
                return false;
            }
            package.environment.push_back({
                environment.text(0), environment.integer(1) != 0,
                environment.optional_text(2)});
        }
        record.packages.push_back(std::move(package));
    }
    Statement remotes;
    if (!remotes.prepare(
            database,
            "SELECT url,transport FROM remotes WHERE server_version_id=?1 "
            "ORDER BY position;",
            error) ||
        !remotes.bind_int64(1, server_id, error))
        return false;
    while (true) {
        const int remote_result = remotes.step();
        if (remote_result == SQLITE_DONE) break;
        if (remote_result != SQLITE_ROW) {
            error = sqlite_message(database.get(), "read remotes");
            return false;
        }
        record.remotes.push_back({remotes.text(0), remotes.text(1)});
    }
    return true;
}

std::string show_text(
    const std::vector<ShowRecord>& records,
    std::string_view snapshot_digest,
    bool include_canonical) {
    std::ostringstream output;
    for (std::size_t index = 0U; index < records.size(); ++index) {
        const ShowRecord& record = records[index];
        if (index != 0U) output << '\n';
        output << "server_identifier=" << text_value(record.row.name) << '\n'
               << "server_version=" << text_value(record.row.version) << '\n'
               << "description=" << text_optional(record.row.description) << '\n'
               << "registry_status=" << text_optional(record.row.status) << '\n'
               << "published_at=" << text_optional(record.published_at) << '\n'
               << "updated_at=" << text_optional(record.updated_at) << '\n'
               << "repository=" << text_optional(record.row.repository) << '\n'
               << "packages=" << record.packages.size() << '\n';
        for (std::size_t package_index = 0U;
             package_index < record.packages.size(); ++package_index) {
            const ShowPackage& package = record.packages[package_index];
            const std::string prefix =
                "package." + std::to_string(package_index) + ".";
            output << prefix << "registry=" << text_value(package.registry) << '\n'
                   << prefix << "identifier=" << text_value(package.identifier) << '\n'
                   << prefix << "version=" << text_optional(package.version) << '\n'
                   << prefix << "transport=" << text_value(package.transport) << '\n';
            for (std::size_t argument = 0U;
                 argument < package.arguments.size(); ++argument)
                output << prefix << "argument." << argument << '='
                       << text_optional(package.arguments[argument]) << '\n';
            for (std::size_t environment = 0U;
                 environment < package.environment.size(); ++environment) {
                const auto& declaration = package.environment[environment];
                output << prefix << "environment." << environment
                       << ".name=" << text_value(declaration.name) << '\n'
                       << prefix << "environment." << environment
                       << ".required="
                       << (declaration.required ? "true" : "false") << '\n'
                       << prefix << "environment." << environment
                       << ".description="
                       << text_optional(declaration.description) << '\n';
            }
        }
        output << "remotes=" << record.remotes.size() << '\n';
        for (std::size_t remote = 0U; remote < record.remotes.size(); ++remote)
            output << "remote." << remote << ".url="
                   << text_value(record.remotes[remote].url)
                   << '\n'
                   << "remote." << remote
                   << ".transport="
                   << text_value(record.remotes[remote].transport) << '\n';
        output << "canonical_sha256=" << text_value(record.canonical_sha256) << '\n'
               << "snapshot_sha256=" << text_value(snapshot_digest) << '\n';
        if (include_canonical)
            output << "canonical=" << record.canonical_json << '\n';
    }
    return output.str();
}

std::string show_json(
    const std::vector<ShowRecord>& records,
    std::string_view snapshot_digest,
    bool include_canonical) {
    std::ostringstream output;
    output << '[';
    for (std::size_t index = 0U; index < records.size(); ++index) {
        if (index != 0U) output << ',';
        const ShowRecord& record = records[index];
        output << "{\"server_identifier\":" << json_string(record.row.name)
               << ",\"server_version\":" << json_string(record.row.version)
               << ",\"description\":" << json_optional(record.row.description)
               << ",\"registry_status\":" << json_optional(record.row.status)
               << ",\"published_at\":" << json_optional(record.published_at)
               << ",\"updated_at\":" << json_optional(record.updated_at)
               << ",\"repository\":" << json_optional(record.row.repository)
               << ",\"packages\":[";
        for (std::size_t package_index = 0U;
             package_index < record.packages.size(); ++package_index) {
            if (package_index != 0U) output << ',';
            const ShowPackage& package = record.packages[package_index];
            output << "{\"registry\":" << json_string(package.registry)
                   << ",\"identifier\":" << json_string(package.identifier)
                   << ",\"version\":" << json_optional(package.version)
                   << ",\"transport\":" << json_string(package.transport)
                   << ",\"arguments\":[";
            for (std::size_t argument = 0U;
                 argument < package.arguments.size(); ++argument) {
                if (argument != 0U) output << ',';
                output << json_optional(package.arguments[argument]);
            }
            output << "],\"environment\":[";
            for (std::size_t environment = 0U;
                 environment < package.environment.size(); ++environment) {
                if (environment != 0U) output << ',';
                const auto& declaration = package.environment[environment];
                output << "{\"name\":" << json_string(declaration.name)
                       << ",\"required\":"
                       << (declaration.required ? "true" : "false")
                       << ",\"description\":"
                       << json_optional(declaration.description) << '}';
            }
            output << "]}";
        }
        output << "],\"remotes\":[";
        for (std::size_t remote = 0U; remote < record.remotes.size(); ++remote) {
            if (remote != 0U) output << ',';
            output << "{\"url\":" << json_string(record.remotes[remote].url)
                   << ",\"transport\":"
                   << json_string(record.remotes[remote].transport) << '}';
        }
        output << "],\"canonical_sha256\":"
               << json_string(record.canonical_sha256)
               << ",\"snapshot_sha256\":" << json_string(snapshot_digest);
        if (include_canonical)
            output << ",\"canonical\":" << record.canonical_json;
        output << '}';
    }
    output << "]\n";
    return output.str();
}

}  // namespace

ExplorerResult show_registry(const RegistryShowOptions& options) {
    if (options.server_name.empty())
        return failure(
            ExplorerError::invalid_arguments,
            "server name must not be empty");
    Database database;
    std::string search_mode;
    ExplorerResult opened = open_catalog(options.database, database, search_mode);
    if (!opened.ok()) return opened;
    SelectedSnapshot snapshot;
    ExplorerResult selected =
        select_snapshot(database, options.snapshot, snapshot);
    if (!selected.ok()) return selected;
    std::string sql =
        "SELECT sv.id,sv.server_identifier,sv.server_version,sv.registry_status,"
        "sv.description,repo.url,sv.published_at,sv.updated_at,"
        "sv.canonical_sha256,sv.canonical_json "
        "FROM snapshot_server_versions ssv JOIN server_versions sv "
        "ON sv.id=ssv.server_version_id LEFT JOIN repositories repo "
        "ON repo.server_version_id=sv.id WHERE ssv.snapshot_id=?1 "
        "AND sv.server_identifier=?2";
    if (options.version) sql += " AND sv.server_version=?3";
    sql += " ORDER BY sv.server_version COLLATE BINARY;";
    std::string error;
    Statement query;
    if (!query.prepare(database, sql, error) ||
        !query.bind_int64(1, snapshot.id, error) ||
        !query.bind_text(2, options.server_name, error) ||
        (options.version && !query.bind_text(3, *options.version, error)))
        return failure(ExplorerError::database, error);
    std::vector<ShowRecord> records;
    while (true) {
        const int result = query.step();
        if (result == SQLITE_DONE) break;
        if (result != SQLITE_ROW)
            return failure(
                ExplorerError::database,
                sqlite_message(database.get(), "show registry server"));
        ShowRecord record;
        const sqlite3_int64 server_id = query.integer(0);
        record.row.name = query.text(1);
        record.row.version = query.text(2);
        record.row.status = query.optional_text(3);
        record.row.description = query.optional_text(4);
        record.row.repository = query.optional_text(5);
        record.published_at = query.optional_text(6);
        record.updated_at = query.optional_text(7);
        record.canonical_sha256 = query.text(8);
        record.canonical_json = query.text(9);
        if (!load_show_children(database, server_id, record, error))
            return failure(ExplorerError::database, error);
        records.push_back(std::move(record));
    }
    if (records.empty()) {
        const std::string message = options.version ?
            "server version not found: " + options.server_name + "@" +
                *options.version :
            "server not found: " + options.server_name;
        return failure(ExplorerError::server_not_found, message);
    }
    return {
        ExplorerError::none,
        options.format == ShowFormat::json ?
            show_json(records, snapshot.digest, options.include_canonical) :
            show_text(records, snapshot.digest, options.include_canonical),
        std::nullopt};
}

}  // namespace mcpo
