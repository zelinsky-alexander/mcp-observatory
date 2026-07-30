#pragma once

#include "observatory/acquisition.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcpo {

inline constexpr std::string_view package_analyzer_name = "mcp-observatory-static";
inline constexpr std::string_view package_analyzer_version = "1.1.0";
inline constexpr std::string_view default_npm_registry_url = "https://registry.npmjs.org";
inline constexpr std::string_view default_pypi_registry_url = "https://pypi.org/pypi";
inline constexpr std::string_view default_package_rules_path =
    "rules/artifact-static-analysis-v1.json";

enum class AnalyzeError {
    none,
    invalid_arguments,
    database,
    incompatible_schema,
    package_not_found,
    ambiguous_package,
    unsupported_registry,
    missing_version,
    download,
    integrity,
    archive,
    container,
    validation,
    limit_exceeded,
    io,
};

enum class AnalyzeOutputFormat { text, json };

enum class FindingOperationError {
    none,
    invalid_arguments,
    database,
    incompatible_schema,
    finding_not_found,
    conflict,
    evidence,
    limit_exceeded,
    io,
};

struct ArchiveLimits {
    std::size_t maximum_files{10'000U};
    std::size_t maximum_total_uncompressed_bytes{64U * 1024U * 1024U};
    std::size_t maximum_individual_file_bytes{8U * 1024U * 1024U};
    std::size_t maximum_tarball_bytes{32U * 1024U * 1024U};
    std::size_t maximum_evidence_snippet_bytes{240U};
    std::size_t maximum_analyzer_output_bytes{16U * 1024U * 1024U};
};

struct AnalyzePackageOptions {
    std::filesystem::path database;
    std::string server_identifier;
    std::string server_version;
    std::string package_identifier;
    std::filesystem::path evidence_root{"evidence"};
    AnalyzeOutputFormat format{AnalyzeOutputFormat::text};
    bool force{};
    std::string npm_registry_url{default_npm_registry_url};
    std::string pypi_registry_url{default_pypi_registry_url};
    std::filesystem::path rules_path{default_package_rules_path};
    ArchiveLimits limits{};
    std::chrono::seconds download_timeout{60};
    std::chrono::seconds container_timeout{120};
    std::string docker_binary{"/usr/bin/docker"};
    std::string analyzer_image{"mcp-observatory-static-analyzer:1"};
    std::filesystem::path self_executable;
    bool allow_in_process_worker{};
};

struct ResolvedPackage {
    std::int64_t package_id{};
    std::int64_t server_version_id{};
    std::string server_identifier;
    std::string server_version;
    std::string registry_type;
    std::string package_identifier;
    std::optional<std::string> package_version;
    std::string transport;
};

struct NpmDistMetadata {
    std::string name;
    std::string version;
    std::string tarball_url;
    std::string integrity;
    std::optional<std::string> shasum;
};

struct AnalysisFinding {
    std::string rule_id;
    std::string category;
    std::string severity;
    std::string confidence;
    std::string disposition{"unreviewed"};
    std::string title;
    std::string subject_path;
    std::optional<std::uint32_t> line_number;
    std::optional<std::string> symbol;
    std::string evidence;
    std::string explanation;
};

struct AnalysisFileRecord {
    std::string archive_path;
    std::string file_type;
    std::uint64_t byte_size{};
    std::string sha256;
    bool executable{};
    bool native_binary{};
    bool generated{};
    bool minified{};
};

struct AnalysisDependency {
    std::string dependency_type;
    std::string dependency_name;
    std::string declared_version;
    std::optional<std::string> resolved_version;
    bool direct{true};
    bool development{};
};

struct AnalysisEvidenceFile {
    std::string evidence_type;
    std::string relative_path;
    std::string sha256;
    std::uint64_t byte_size{};
    std::string media_type;
};

struct AnalyzerWorkerResult {
    std::string status{"ok"};
    std::string ruleset_version;
    std::string package_name;
    std::string package_version;
    std::optional<std::string> license;
    std::optional<std::string> repository;
    std::optional<std::string> main_entry;
    std::optional<std::string> module_entry;
    std::optional<std::string> engines_node;
    std::vector<std::pair<std::string, std::string>> bin_entries;
    std::vector<std::pair<std::string, std::string>> lifecycle_scripts;
    std::vector<AnalysisDependency> dependencies;
    std::vector<AnalysisFileRecord> files;
    std::vector<AnalysisFinding> findings;
    std::string archive_inventory_json;
    std::string package_manifest_json;
    std::string summary_json;
    bool has_native_code{};
    std::size_t analyzed_file_count{};
};

struct AnalyzePackageResult {
    AnalyzeError error{AnalyzeError::none};
    std::string output;
    std::optional<std::int64_t> analysis_run_id;
    std::optional<std::string> artifact_sha256;
    std::optional<std::filesystem::path> evidence_directory;
    bool reused_existing{};
    [[nodiscard]] bool ok() const noexcept { return error == AnalyzeError::none; }
};

struct FindingSourceOptions {
    std::filesystem::path database;
    std::filesystem::path evidence_root{"evidence"};
    std::int64_t finding_id{};
    AnalyzeOutputFormat format{AnalyzeOutputFormat::text};
    bool raw_output{};
    ArchiveLimits limits{};
    std::size_t maximum_source_bytes{128U * 1024U};
};

struct ReviewFindingOptions {
    std::filesystem::path database;
    std::int64_t finding_id{};
    std::string expected_disposition;
    std::string disposition;
    std::string reviewer;
    AnalyzeOutputFormat format{AnalyzeOutputFormat::text};
};

struct FindingOperationResult {
    FindingOperationError error{FindingOperationError::none};
    std::string output;
    [[nodiscard]] bool ok() const noexcept {
        return error == FindingOperationError::none;
    }
};

using AnalyzeDownloadTransport = std::function<bool(
    const std::string& url,
    std::chrono::steady_clock::duration timeout,
    std::size_t maximum_bytes,
    std::string& body,
    std::string& error)>;

using AnalyzeWorkerRunner = std::function<bool(
    std::string_view registry_type,
    const std::filesystem::path& tarball,
    const std::filesystem::path& rules_path,
    const ArchiveLimits& limits,
    AnalyzerWorkerResult& result,
    std::string& raw_json,
    std::string& error)>;

[[nodiscard]] AnalyzeError resolve_exact_package(
    const std::filesystem::path& database,
    std::string_view server_identifier,
    std::string_view server_version,
    std::string_view package_identifier,
    ResolvedPackage& resolved,
    std::string& error);

[[nodiscard]] bool parse_npm_version_metadata(
    std::string_view json_text,
    std::string_view expected_name,
    std::string_view expected_version,
    NpmDistMetadata& metadata,
    std::string& error);

[[nodiscard]] bool parse_pypi_release_metadata(
    std::string_view json_text,
    std::string_view expected_name,
    std::string_view expected_version,
    const AcquisitionLimits& limits,
    ArtifactDescriptor& descriptor,
    std::string& error);

[[nodiscard]] bool verify_npm_integrity(
    std::string_view artifact_bytes,
    std::string_view published_integrity,
    std::string& error);

[[nodiscard]] bool verify_pypi_integrity(
    std::string_view artifact_bytes,
    std::string_view published_sha256,
    std::uint64_t published_size,
    std::string& error);

[[nodiscard]] bool analyze_npm_tarball_bytes(
    std::string_view tarball_bytes,
    const std::filesystem::path& rules_path,
    const ArchiveLimits& limits,
    AnalyzerWorkerResult& result,
    std::string& error);

[[nodiscard]] bool analyze_package_tarball_bytes(
    std::string_view registry_type,
    std::string_view tarball_bytes,
    const std::filesystem::path& rules_path,
    const ArchiveLimits& limits,
    AnalyzerWorkerResult& result,
    std::string& error);

[[nodiscard]] bool parse_analyzer_worker_json(
    std::string_view json_text,
    const ArchiveLimits& limits,
    AnalyzerWorkerResult& result,
    std::string& error);

[[nodiscard]] AnalyzePackageResult analyze_package(
    const AnalyzePackageOptions& options,
    AnalyzeDownloadTransport download = {},
    AnalyzeWorkerRunner worker = {});

[[nodiscard]] FindingOperationResult read_finding_source(
    const FindingSourceOptions& options);

[[nodiscard]] FindingOperationResult review_finding(
    const ReviewFindingOptions& options);

[[nodiscard]] int analyze_worker_main(
    std::string_view registry_type,
    const std::filesystem::path& tarball,
    const std::filesystem::path& rules_path,
    const ArchiveLimits& limits,
    std::ostream& out,
    std::ostream& err);

}  // namespace mcpo
