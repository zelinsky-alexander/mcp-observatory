#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcpo {

inline constexpr int registry_explorer_schema_version = 1;

enum class ExplorerError {
    none,
    invalid_arguments,
    bundle_validation,
    database,
    incompatible_schema,
    malformed_canonical,
    snapshot_not_found,
    server_not_found,
    limit_exceeded,
    database_size_exceeded,
};

struct RegistryIndexStats {
    std::size_t inserted_server_versions{};
    std::size_t reused_server_versions{};
    std::size_t changed_identity_records{};
    std::size_t snapshot_links_created{};
    bool snapshot_already_indexed{};
};

struct ExplorerResult {
    ExplorerError error{ExplorerError::none};
    std::string output;
    std::optional<RegistryIndexStats> index_stats;
    [[nodiscard]] bool ok() const noexcept { return error == ExplorerError::none; }
};

struct RegistryIndexOptions {
    std::filesystem::path bundle;
    std::filesystem::path database;
    std::size_t maximum_records{500'000U};
    std::size_t maximum_line_bytes{8U * 1024U * 1024U};
    std::uint64_t maximum_database_bytes{4'294'967'296ULL};
    bool verbose{};
};

struct SnapshotSelection {
    std::optional<std::string> digest;
};

enum class SummaryFormat { text, json };
enum class RowsFormat { text, jsonl };
enum class ShowFormat { text, json };

struct RegistryFilters {
    std::optional<std::string> status;
    std::optional<std::string> transport;
    std::optional<std::string> package_registry;
    std::optional<std::string> repository_host;
    std::optional<std::string> remote_host;
    bool has_repository{};
    bool without_repository{};
    bool has_package{};
    bool has_remote{};
};

struct RegistrySearchOptions {
    std::filesystem::path database;
    std::string query;
    SnapshotSelection snapshot;
    RegistryFilters filters;
    std::size_t limit{20U};
    std::size_t offset{};
    RowsFormat format{RowsFormat::text};
    bool verbose{};
};

struct RegistryListOptions {
    std::filesystem::path database;
    SnapshotSelection snapshot;
    RegistryFilters filters;
    std::size_t limit{50U};
    std::size_t offset{};
    RowsFormat format{RowsFormat::text};
};

struct RegistryShowOptions {
    std::filesystem::path database;
    std::string server_name;
    SnapshotSelection snapshot;
    std::optional<std::string> version;
    bool include_canonical{};
    ShowFormat format{ShowFormat::text};
};

struct RegistryRepositoryRecord {
    std::optional<std::string> source;
    std::optional<std::string> url;
};

struct RegistryPackageArgumentRecord {
    std::optional<std::string> value;
};

struct RegistryEnvironmentRecord {
    std::string name;
    bool required{};
    std::optional<std::string> description;
};

struct RegistryPackageRecord {
    std::string registry_type;
    std::string identifier;
    std::optional<std::string> version;
    std::string transport;
    std::vector<RegistryPackageArgumentRecord> arguments;
    std::vector<RegistryEnvironmentRecord> environment;
};

struct RegistryRemoteRecord {
    std::string url;
    std::string transport;
};

struct RegistryCanonicalRecord {
    std::string server_identifier;
    std::string server_version;
    std::optional<std::string> description;
    std::optional<std::string> registry_status;
    std::optional<std::string> published_at;
    std::optional<std::string> updated_at;
    std::string canonical_sha256;
    std::optional<RegistryRepositoryRecord> repository;
    std::vector<RegistryPackageRecord> packages;
    std::vector<RegistryRemoteRecord> remotes;
};

struct RegistryBundleManifest {
    std::string snapshot_sha256;
    std::string completed_at;
    std::string started_at;
    std::string registry_base_url;
    std::optional<std::string> collector_name;
    std::optional<std::string> collector_version;
    std::optional<std::string> collector_git_commit;
    std::size_t bundle_version{};
    std::size_t pages{};
    std::size_t records_received{};
    std::size_t unique_server_versions{};
    std::string collection_mode{"full"};
    std::optional<std::string> updated_since;
    std::optional<std::string> base_snapshot_sha256;
    std::optional<std::string> base_snapshot_completed_at;
};

struct RegistryBaselineSnapshot {
    std::int64_t id{};
    std::string snapshot_sha256;
    std::string completed_at;
};

[[nodiscard]] bool read_registry_bundle_manifest(
    const std::filesystem::path& bundle,
    RegistryBundleManifest& manifest,
    std::string& error);

[[nodiscard]] bool parse_registry_canonical_record(
    std::string_view line,
    RegistryCanonicalRecord& record,
    std::string& error);

[[nodiscard]] ExplorerResult index_registry_bundle(const RegistryIndexOptions& options);
[[nodiscard]] ExplorerResult latest_registry_snapshot(
    const std::filesystem::path& database,
    RegistryBaselineSnapshot& snapshot);
[[nodiscard]] ExplorerResult summarize_registry(
    const std::filesystem::path& database,
    const SnapshotSelection& snapshot,
    SummaryFormat format);
[[nodiscard]] ExplorerResult search_registry(const RegistrySearchOptions& options);
[[nodiscard]] ExplorerResult list_registry(const RegistryListOptions& options);
[[nodiscard]] ExplorerResult show_registry(const RegistryShowOptions& options);

}  // namespace mcpo
