#include "observatory/analyze.hpp"
#include "observatory/explorer.hpp"
#include "observatory/history.hpp"
#include "observatory/inventory.hpp"
#include "observatory/observation.hpp"
#include "observatory/registry.hpp"
#include "observatory/target_manifest.hpp"

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

void print_usage(std::ostream& out) {
    out << "usage:\n"
        << "  mcp-observatory about\n"
        << "  mcp-observatory validate-targets PATH\n"
        << "  mcp-observatory summarize-targets PATH\n"
        << "  mcp-observatory compare-inventories BEFORE AFTER\n"
        << "  mcp-observatory validate-observation PATH\n"
        << "  mcp-observatory ingest-observation PATH HISTORY_JSONL\n"
        << "  mcp-observatory history summarize HISTORY_JSONL\n"
        << "  mcp-observatory history latest HISTORY_JSONL TARGET_ID\n"
        << "  mcp-observatory history diff-latest HISTORY_JSONL TARGET_ID\n"
        << "  mcp-observatory registry collect --output DIRECTORY [OPTIONS]\n"
        << "  mcp-observatory registry refresh --database PATH --output DIRECTORY [OPTIONS]\n"
        << "  mcp-observatory registry checkpoint reconstruct PARTIAL_DIRECTORY [OPTIONS]\n"
        << "  mcp-observatory registry index --bundle PATH --database PATH [OPTIONS]\n"
        << "  mcp-observatory registry summarize DATABASE [OPTIONS]\n"
        << "  mcp-observatory registry search DATABASE QUERY [OPTIONS]\n"
        << "  mcp-observatory registry show DATABASE SERVER_NAME [OPTIONS]\n"
        << "  mcp-observatory registry list DATABASE [OPTIONS]\n"
        << "  mcp-observatory analyze package [OPTIONS]\n"
        << "  mcp-observatory analyze-worker --registry npm|pypi --tarball PATH [OPTIONS]\n"
        << "  mcp-observatory bundle validate DIRECTORY\n"
        << "\nanalyze package options:\n"
        << "  --database PATH               registry SQLite database\n"
        << "  --server IDENTIFIER           exact server identifier\n"
        << "  --version VERSION             exact server version\n"
        << "  --package IDENTIFIER          exact npm or PyPI package name\n"
        << "  --evidence-root PATH          evidence root (default evidence)\n"
        << "  --rules PATH                  analysis rules JSON\n"
        << "  --format text|json            output format (default text)\n"
        << "  --force                       ignore completed-run deduplication\n"
        << "  --npm-registry-url URL        npm registry base URL\n"
        << "  --pypi-registry-url URL       PyPI JSON API base URL\n"
        << "  --allow-in-process-worker     test-only: skip Docker worker\n"
        << "\nregistry collect runtime options:\n"
        << "  --request-timeout-seconds N   one HTTP attempt (default 60)\n"
        << "  --stall-timeout-seconds N     no durable page completion (default 300)\n"
        << "  --run-timeout-seconds N       total runtime; 0 is unlimited (default 0)\n"
        << "  --maximum-attempts-per-page N attempts including first (default 8)\n"
        << "  --retry-initial-seconds N     initial backoff (default 2)\n"
        << "  --retry-maximum-seconds N     maximum backoff (default 120)\n";
}

bool parse_size(std::string_view text, std::size_t& value) {
    if (text.empty() || text.front() == '-') return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool parse_unsigned(std::string_view text, unsigned& value) {
    if (text.empty() || text.front() == '-') return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool parse_u64(std::string_view text, std::uint64_t& value) {
    if (text.empty() || text.front() == '-') return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

int explorer_exit(mcpo::ExplorerError error) {
    switch (error) {
        case mcpo::ExplorerError::none: return 0;
        case mcpo::ExplorerError::invalid_arguments: return 1;
        case mcpo::ExplorerError::database: return 2;
        case mcpo::ExplorerError::bundle_validation: return 3;
        case mcpo::ExplorerError::snapshot_not_found:
        case mcpo::ExplorerError::server_not_found: return 5;
        case mcpo::ExplorerError::incompatible_schema: return 6;
        case mcpo::ExplorerError::malformed_canonical: return 7;
        case mcpo::ExplorerError::limit_exceeded: return 8;
        case mcpo::ExplorerError::database_size_exceeded: return 9;
    }
    return 2;
}

int report_explorer(const mcpo::ExplorerResult& result) {
    if (result.ok()) {
        std::cout << result.output;
        return 0;
    }
    std::cerr << result.output << '\n';
    return explorer_exit(result.error);
}

bool parse_snapshot_option(
    int argc,
    char** argv,
    int& index,
    mcpo::SnapshotSelection& selection,
    bool& latest,
    std::string& error) {
    const std::string_view argument(argv[index]);
    if (argument == "--latest") {
        if (selection.digest) {
            error = "--latest contradicts --snapshot";
            return false;
        }
        latest = true;
        return true;
    }
    if (argument != "--snapshot") return false;
    if (latest) {
        error = "--snapshot contradicts --latest";
        return false;
    }
    if (index + 1 >= argc) {
        error = "missing value for --snapshot";
        return false;
    }
    selection.digest = std::string(argv[++index]);
    return true;
}

bool parse_filter_option(
    int argc,
    char** argv,
    int& index,
    mcpo::RegistryFilters& filters,
    bool allow_hosts,
    std::string& error) {
    const std::string_view argument(argv[index]);
    if (argument == "--has-repository") {
        filters.has_repository = true;
        return true;
    }
    if (argument == "--without-repository") {
        filters.without_repository = true;
        return true;
    }
    if (argument == "--has-package") {
        filters.has_package = true;
        return true;
    }
    if (argument == "--has-remote") {
        filters.has_remote = true;
        return true;
    }
    std::optional<std::string>* target = nullptr;
    if (argument == "--status") target = &filters.status;
    else if (argument == "--transport") target = &filters.transport;
    else if (argument == "--package-registry") target = &filters.package_registry;
    else if (allow_hosts && argument == "--repository-host")
        target = &filters.repository_host;
    else if (allow_hosts && argument == "--remote-host")
        target = &filters.remote_host;
    else
        return false;
    if (index + 1 >= argc) {
        error = "missing value for " + std::string(argument);
        return false;
    }
    *target = std::string(argv[++index]);
    return true;
}

int run_registry_index(int argc, char** argv) {
    mcpo::RegistryIndexOptions options;
    bool have_bundle{};
    bool have_database{};
    for (int index = 3; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--verbose") {
            options.verbose = true;
            continue;
        }
        if (index + 1 >= argc) {
            std::cerr << "missing value for " << argument << '\n';
            return 1;
        }
        const std::string_view value(argv[++index]);
        if (argument == "--bundle") {
            options.bundle = std::string(value);
            have_bundle = true;
        } else if (argument == "--database") {
            options.database = std::string(value);
            have_database = true;
        } else if (argument == "--maximum-records") {
            if (!parse_size(value, options.maximum_records)) return 1;
        } else if (argument == "--maximum-line-bytes") {
            if (!parse_size(value, options.maximum_line_bytes)) return 1;
        } else if (argument == "--maximum-database-bytes") {
            if (!parse_u64(value, options.maximum_database_bytes)) return 1;
        } else {
            std::cerr << "unknown registry index option: " << argument << '\n';
            return 1;
        }
    }
    if (!have_bundle || !have_database) {
        std::cerr << "registry index requires --bundle and --database\n";
        return 1;
    }
    return report_explorer(mcpo::index_registry_bundle(options));
}

int run_registry_summarize(int argc, char** argv) {
    if (argc < 4) return 1;
    mcpo::SnapshotSelection selection;
    mcpo::SummaryFormat format = mcpo::SummaryFormat::text;
    bool latest{};
    for (int index = 4; index < argc; ++index) {
        std::string error;
        const int before = index;
        if (parse_snapshot_option(
                argc, argv, index, selection, latest, error)) continue;
        if (!error.empty()) {
            std::cerr << error << '\n';
            return 1;
        }
        index = before;
        const std::string_view argument(argv[index]);
        if (argument == "--format" && index + 1 < argc) {
            const std::string_view value(argv[++index]);
            if (value == "text") format = mcpo::SummaryFormat::text;
            else if (value == "json") format = mcpo::SummaryFormat::json;
            else {
                std::cerr << "invalid summarize format\n";
                return 1;
            }
        } else {
            std::cerr << "unknown registry summarize option: " << argument << '\n';
            return 1;
        }
    }
    return report_explorer(mcpo::summarize_registry(argv[3], selection, format));
}

bool parse_rows_format(
    std::string_view value,
    mcpo::RowsFormat& format) {
    if (value == "text") format = mcpo::RowsFormat::text;
    else if (value == "jsonl") format = mcpo::RowsFormat::jsonl;
    else return false;
    return true;
}

int run_registry_search(int argc, char** argv) {
    if (argc < 5) return 1;
    mcpo::RegistrySearchOptions options;
    options.database = argv[3];
    options.query = argv[4];
    bool latest{};
    for (int index = 5; index < argc; ++index) {
        std::string error;
        const int before = index;
        if (parse_snapshot_option(
                argc, argv, index, options.snapshot, latest, error)) continue;
        if (!error.empty()) {
            std::cerr << error << '\n';
            return 1;
        }
        index = before;
        if (parse_filter_option(
                argc, argv, index, options.filters, false, error)) continue;
        if (!error.empty()) {
            std::cerr << error << '\n';
            return 1;
        }
        index = before;
        const std::string_view argument(argv[index]);
        if (argument == "--limit" && index + 1 < argc) {
            if (!parse_size(argv[++index], options.limit)) return 1;
        } else if (argument == "--offset" && index + 1 < argc) {
            if (!parse_size(argv[++index], options.offset)) return 1;
        } else if (argument == "--format" && index + 1 < argc) {
            if (!parse_rows_format(argv[++index], options.format)) return 1;
        } else {
            std::cerr << "unknown registry search option: " << argument << '\n';
            return 1;
        }
    }
    return report_explorer(mcpo::search_registry(options));
}

int run_registry_list(int argc, char** argv) {
    if (argc < 4) return 1;
    mcpo::RegistryListOptions options;
    options.database = argv[3];
    bool latest{};
    for (int index = 4; index < argc; ++index) {
        std::string error;
        const int before = index;
        if (parse_snapshot_option(
                argc, argv, index, options.snapshot, latest, error)) continue;
        if (!error.empty()) {
            std::cerr << error << '\n';
            return 1;
        }
        index = before;
        if (parse_filter_option(
                argc, argv, index, options.filters, true, error)) continue;
        if (!error.empty()) {
            std::cerr << error << '\n';
            return 1;
        }
        index = before;
        const std::string_view argument(argv[index]);
        if (argument == "--limit" && index + 1 < argc) {
            if (!parse_size(argv[++index], options.limit)) return 1;
        } else if (argument == "--offset" && index + 1 < argc) {
            if (!parse_size(argv[++index], options.offset)) return 1;
        } else if (argument == "--format" && index + 1 < argc) {
            if (!parse_rows_format(argv[++index], options.format)) return 1;
        } else {
            std::cerr << "unknown registry list option: " << argument << '\n';
            return 1;
        }
    }
    return report_explorer(mcpo::list_registry(options));
}

int run_registry_show(int argc, char** argv) {
    if (argc < 5) return 1;
    mcpo::RegistryShowOptions options;
    options.database = argv[3];
    options.server_name = argv[4];
    bool latest{};
    for (int index = 5; index < argc; ++index) {
        std::string error;
        const int before = index;
        if (parse_snapshot_option(
                argc, argv, index, options.snapshot, latest, error)) continue;
        if (!error.empty()) {
            std::cerr << error << '\n';
            return 1;
        }
        index = before;
        const std::string_view argument(argv[index]);
        if (argument == "--include-canonical") {
            options.include_canonical = true;
        } else if (argument == "--version" && index + 1 < argc) {
            options.version = std::string(argv[++index]);
        } else if (argument == "--format" && index + 1 < argc) {
            const std::string_view value(argv[++index]);
            if (value == "text") options.format = mcpo::ShowFormat::text;
            else if (value == "json") options.format = mcpo::ShowFormat::json;
            else return 1;
        } else {
            std::cerr << "unknown registry show option: " << argument << '\n';
            return 1;
        }
    }
    return report_explorer(mcpo::show_registry(options));
}

bool parse_collect_option(
    int argc,
    char** argv,
    int& index,
    mcpo::RegistryCollectOptions& options,
    bool& have_output,
    std::string& error) {
        const std::string_view argument(argv[index]);
        if (argument == "--retain-raw") {
            options.retain_raw = true;
            return true;
        }
        if (argument == "--verbose") {
            options.verbose = true;
            return true;
        }
        if (index + 1 >= argc) {
            error = "missing value for " + std::string(argument);
            return false;
        }
        const std::string_view value(argv[++index]);
        if (argument == "--output") {
            options.output = std::string(value);
            have_output = true;
        } else if (argument == "--resume") {
            options.resume = std::filesystem::path(std::string(value));
        } else if (argument == "--registry-base-url") {
            options.registry_base_url = value;
        } else if (argument == "--maximum-pages") {
            if (!parse_size(value, options.limits.maximum_pages))
                error = "invalid --maximum-pages";
        } else if (argument == "--maximum-page-bytes") {
            if (!parse_size(value, options.limits.maximum_page_bytes))
                error = "invalid --maximum-page-bytes";
        } else if (argument == "--maximum-records") {
            if (!parse_size(value, options.limits.maximum_records))
                error = "invalid --maximum-records";
        } else if (argument == "--maximum-redirects") {
            if (!parse_size(value, options.limits.maximum_redirects))
                error = "invalid --maximum-redirects";
        } else if (argument == "--request-timeout-seconds") {
            unsigned parsed{};
            if (!parse_unsigned(value, parsed)) {
                error = "invalid --request-timeout-seconds";
                return false;
            }
            options.runtime.request_timeout = std::chrono::seconds(parsed);
        } else if (argument == "--stall-timeout-seconds") {
            unsigned parsed{};
            if (!parse_unsigned(value, parsed)) {
                error = "invalid --stall-timeout-seconds";
                return false;
            }
            options.runtime.stall_timeout = std::chrono::seconds(parsed);
        } else if (argument == "--run-timeout-seconds") {
            unsigned parsed{};
            if (!parse_unsigned(value, parsed)) {
                error = "invalid --run-timeout-seconds";
                return false;
            }
            options.runtime.run_timeout =
                parsed == 0U ? std::nullopt :
                    std::optional<std::chrono::seconds>(
                        std::chrono::seconds(parsed));
        } else if (argument == "--maximum-attempts-per-page") {
            if (!parse_size(
                    value, options.runtime.maximum_attempts_per_page))
                error = "invalid --maximum-attempts-per-page";
        } else if (argument == "--retry-initial-seconds") {
            unsigned parsed{};
            if (!parse_unsigned(value, parsed)) {
                error = "invalid --retry-initial-seconds";
                return false;
            }
            options.runtime.retry_initial = std::chrono::seconds(parsed);
        } else if (argument == "--retry-maximum-seconds") {
            unsigned parsed{};
            if (!parse_unsigned(value, parsed)) {
                error = "invalid --retry-maximum-seconds";
                return false;
            }
            options.runtime.retry_maximum = std::chrono::seconds(parsed);
        } else {
            --index;
            return false;
        }
        return error.empty();
}

void apply_registry_environment(mcpo::RegistryCollectOptions& options) {
    if (const char* environment = std::getenv("MCPO_REGISTRY_BASE_URL");
        environment != nullptr && *environment != '\0')
        options.registry_base_url = environment;
}

int run_registry_collect(int argc, char** argv) {
    mcpo::RegistryCollectOptions options;
    apply_registry_environment(options);
    bool have_output = false;
    for (int index = 3; index < argc; ++index) {
        std::string error;
        const int before = index;
        if (parse_collect_option(
                argc, argv, index, options, have_output, error))
            continue;
        if (!error.empty()) {
            std::cerr << error << '\n';
            return 1;
        }
        std::cerr << "unknown registry collect option: " << argv[before] << '\n';
        return 1;
    }
    if (!have_output) {
        std::cerr << "registry collect requires --output\n";
        return 1;
    }
    std::string message;
    if (!mcpo::collect_registry(options, message)) {
        std::cerr << "registry collection failed: " << message << '\n';
        return 3;
    }
    std::cout << message << '\n';
    return 0;
}

std::string json_quote(std::string_view value) {
    std::string output{"\""};
    for (char character : value) {
        if (character == '"' || character == '\\') output.push_back('\\');
        output.push_back(character);
    }
    output.push_back('"');
    return output;
}

int run_registry_refresh(int argc, char** argv) {
    mcpo::RegistryCollectOptions collect;
    apply_registry_environment(collect);
    std::filesystem::path database;
    std::optional<std::string> updated_since;
    std::string format{"text"};
    bool have_database{};
    bool have_output{};
    for (int index = 3; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--database" || argument == "--updated-since" ||
            argument == "--format") {
            if (index + 1 >= argc) {
                std::cerr << "missing value for " << argument << '\n';
                return 1;
            }
            const std::string value(argv[++index]);
            if (argument == "--database") {
                database = value;
                have_database = true;
            } else if (argument == "--updated-since") {
                updated_since = value;
            } else if (value == "text" || value == "json") {
                format = value;
            } else {
                std::cerr << "invalid refresh format\n";
                return 1;
            }
            continue;
        }
        std::string error;
        const int before = index;
        if (parse_collect_option(
                argc, argv, index, collect, have_output, error))
            continue;
        if (!error.empty()) {
            std::cerr << error << '\n';
            return 1;
        }
        std::cerr << "unknown registry refresh option: " << argv[before] << '\n';
        return 1;
    }
    if (!have_database || !have_output) {
        std::cerr << "registry refresh requires --database and --output\n";
        return 1;
    }
    if (updated_since && !mcpo::valid_utc_timestamp(*updated_since)) {
        std::cerr << "invalid_updated_since: expected YYYY-MM-DDTHH:MM:SSZ\n";
        return 1;
    }
    std::error_code path_error;
    if (!std::filesystem::is_regular_file(database, path_error) || path_error) {
        std::cerr << "no_baseline_snapshot: database has no completed snapshot\n";
        return 5;
    }
    mcpo::RegistryBaselineSnapshot baseline;
    const mcpo::ExplorerResult selected =
        mcpo::latest_registry_snapshot(database, baseline);
    if (!selected.ok()) {
        if (selected.error == mcpo::ExplorerError::snapshot_not_found) {
            std::cerr << "no_baseline_snapshot: database has no completed snapshot\n";
            return 5;
        }
        std::cerr << selected.output << '\n';
        return explorer_exit(selected.error);
    }
    collect.collection_mode = mcpo::RegistryCollectionMode::incremental;
    collect.incremental = mcpo::RegistryIncrementalProvenance{
        updated_since.value_or(baseline.completed_at),
        baseline.snapshot_sha256,
        baseline.completed_at};

    std::string collection_message;
    if (!mcpo::collect_registry(collect, collection_message)) {
        std::cerr << "registry refresh collection failed: "
                  << collection_message << '\n';
        return 3;
    }
    mcpo::RegistryBundleManifest manifest;
    std::string manifest_error;
    if (!mcpo::read_registry_bundle_manifest(
            collect.output, manifest, manifest_error)) {
        std::cerr << "collection_completed_import_failed: "
                  << manifest_error << '\n';
        return 3;
    }
    mcpo::RegistryIndexOptions index;
    index.bundle = collect.output;
    index.database = database;
    index.verbose = collect.verbose;
    const mcpo::ExplorerResult imported = mcpo::index_registry_bundle(index);
    if (!imported.ok()) {
        std::cerr << "collection_completed_import_failed: "
                  << imported.output << "; bundle=" << collect.output.string()
                  << '\n';
        return explorer_exit(imported.error);
    }
    const mcpo::RegistryIndexStats stats =
        imported.index_stats.value_or(mcpo::RegistryIndexStats{});
    if (format == "json") {
        std::cout
            << "{\"status\":\"completed\",\"collection_mode\":\"incremental\","
            << "\"updated_since\":"
            << json_quote(collect.incremental->updated_since)
            << ",\"base_snapshot_sha256\":"
            << json_quote(baseline.snapshot_sha256)
            << ",\"snapshot_sha256\":"
            << json_quote(manifest.snapshot_sha256)
            << ",\"completed_pages\":" << manifest.pages
            << ",\"received_records\":" << manifest.records_received
            << ",\"inserted_server_versions\":"
            << stats.inserted_server_versions
            << ",\"reused_server_versions\":"
            << stats.reused_server_versions
            << ",\"changed_identity_records\":"
            << stats.changed_identity_records
            << ",\"snapshot_links_created\":"
            << stats.snapshot_links_created << "}\n";
    } else {
        std::cout
            << "status=completed\n"
            << "collection_mode=incremental\n"
            << "updated_since=" << collect.incremental->updated_since << '\n'
            << "base_snapshot_sha256=" << baseline.snapshot_sha256 << '\n'
            << "snapshot_sha256=" << manifest.snapshot_sha256 << '\n'
            << "completed_pages=" << manifest.pages << '\n'
            << "received_records=" << manifest.records_received << '\n'
            << "inserted_server_versions="
            << stats.inserted_server_versions << '\n'
            << "reused_server_versions="
            << stats.reused_server_versions << '\n'
            << "changed_identity_records="
            << stats.changed_identity_records << '\n'
            << "snapshot_links_created="
            << stats.snapshot_links_created << '\n';
    }
    return 0;
}

int run_checkpoint_reconstruct(int argc, char** argv) {
    if (argc < 5) return 1;
    const std::filesystem::path partial(argv[4]);
    std::string base_url = std::string(mcpo::official_registry_base_url);
    if (const char* environment = std::getenv("MCPO_REGISTRY_BASE_URL");
        environment != nullptr && *environment != '\0') {
        base_url = environment;
    }
    mcpo::RegistryLimits limits;
    for (int index = 5; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (index + 1 >= argc) {
            std::cerr << "missing value for " << argument << '\n';
            return 1;
        }
        const std::string_view value(argv[++index]);
        if (argument == "--registry-base-url") {
            base_url = value;
        } else if (argument == "--maximum-pages") {
            if (!parse_size(value, limits.maximum_pages)) return 1;
        } else if (argument == "--maximum-page-bytes") {
            if (!parse_size(value, limits.maximum_page_bytes)) return 1;
        } else if (argument == "--maximum-records") {
            if (!parse_size(value, limits.maximum_records)) return 1;
        } else {
            std::cerr << "unknown checkpoint option: " << argument << '\n';
            return 1;
        }
    }
    std::string message;
    if (!mcpo::reconstruct_registry_checkpoint(
            partial, base_url, limits, message)) {
        std::cerr << "checkpoint reconstruction failed: " << message << '\n';
        return 3;
    }
    std::cout << message << '\n';
    return 0;
}

int analyze_exit(mcpo::AnalyzeError error) {
    switch (error) {
        case mcpo::AnalyzeError::none: return 0;
        case mcpo::AnalyzeError::invalid_arguments: return 1;
        case mcpo::AnalyzeError::database: return 2;
        case mcpo::AnalyzeError::download:
        case mcpo::AnalyzeError::container:
        case mcpo::AnalyzeError::io: return 3;
        case mcpo::AnalyzeError::package_not_found:
        case mcpo::AnalyzeError::ambiguous_package:
        case mcpo::AnalyzeError::unsupported_registry:
        case mcpo::AnalyzeError::missing_version: return 5;
        case mcpo::AnalyzeError::incompatible_schema: return 6;
        case mcpo::AnalyzeError::validation:
        case mcpo::AnalyzeError::integrity:
        case mcpo::AnalyzeError::archive: return 7;
        case mcpo::AnalyzeError::limit_exceeded: return 8;
    }
    return 2;
}

int run_analyze_package(int argc, char** argv) {
    mcpo::AnalyzePackageOptions options;
    char self_buffer[4096]{};
    const ssize_t self_length = readlink("/proc/self/exe", self_buffer, sizeof(self_buffer) - 1U);
    if (self_length > 0) {
        self_buffer[self_length] = '\0';
        options.self_executable = self_buffer;
    } else if (argc > 0) {
        options.self_executable = argv[0];
    }
    for (int index = 3; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--force") {
            options.force = true;
            continue;
        }
        if (argument == "--allow-in-process-worker") {
            options.allow_in_process_worker = true;
            continue;
        }
        if (index + 1 >= argc) {
            std::cerr << "missing value for " << argument << '\n';
            return 1;
        }
        const std::string_view value(argv[++index]);
        if (argument == "--database") options.database = std::string(value);
        else if (argument == "--server")
            options.server_identifier = std::string(value);
        else if (argument == "--version")
            options.server_version = std::string(value);
        else if (argument == "--package")
            options.package_identifier = std::string(value);
        else if (argument == "--evidence-root")
            options.evidence_root = std::string(value);
        else if (argument == "--rules")
            options.rules_path = std::string(value);
        else if (argument == "--npm-registry-url")
            options.npm_registry_url = std::string(value);
        else if (argument == "--pypi-registry-url")
            options.pypi_registry_url = std::string(value);
        else if (argument == "--format") {
            if (value == "text") options.format = mcpo::AnalyzeOutputFormat::text;
            else if (value == "json") options.format = mcpo::AnalyzeOutputFormat::json;
            else {
                std::cerr << "invalid analyze format\n";
                return 1;
            }
        } else if (argument == "--maximum-files") {
            if (!parse_size(value, options.limits.maximum_files)) return 1;
        } else if (argument == "--maximum-total-uncompressed-bytes") {
            if (!parse_size(value, options.limits.maximum_total_uncompressed_bytes))
                return 1;
        } else if (argument == "--maximum-individual-file-bytes") {
            if (!parse_size(value, options.limits.maximum_individual_file_bytes))
                return 1;
        } else if (argument == "--maximum-tarball-bytes") {
            if (!parse_size(value, options.limits.maximum_tarball_bytes)) return 1;
        } else {
            std::cerr << "unknown analyze package option: " << argument << '\n';
            return 1;
        }
    }
    if (options.database.empty() || options.server_identifier.empty() ||
        options.server_version.empty() || options.package_identifier.empty()) {
        std::cerr << "analyze package requires --database --server --version --package\n";
        return 1;
    }
    const mcpo::AnalyzePackageResult result = mcpo::analyze_package(options);
    if (!result.ok()) {
        std::cerr << result.output << '\n';
        return analyze_exit(result.error);
    }
    std::cout << result.output;
    return 0;
}

int run_analyze_worker(int argc, char** argv) {
    std::string registry_type;
    std::filesystem::path tarball;
    std::filesystem::path rules_path{mcpo::default_package_rules_path};
    mcpo::ArchiveLimits limits;
    for (int index = 2; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (index + 1 >= argc) {
            std::cerr << "missing value for " << argument << '\n';
            return 1;
        }
        const std::string_view value(argv[++index]);
        if (argument == "--registry") {
            if (value != "npm" && value != "pypi") {
                std::cerr << "analyze-worker registry must be npm or pypi\n";
                return 1;
            }
            registry_type = value;
        } else if (argument == "--tarball") tarball = std::string(value);
        else if (argument == "--rules") rules_path = std::string(value);
        else if (argument == "--maximum-files") {
            if (!parse_size(value, limits.maximum_files)) return 1;
        } else if (argument == "--maximum-total-uncompressed-bytes") {
            if (!parse_size(value, limits.maximum_total_uncompressed_bytes)) return 1;
        } else if (argument == "--maximum-individual-file-bytes") {
            if (!parse_size(value, limits.maximum_individual_file_bytes)) return 1;
        } else if (argument == "--maximum-tarball-bytes") {
            if (!parse_size(value, limits.maximum_tarball_bytes)) return 1;
        } else {
            std::cerr << "unknown analyze-worker option: " << argument << '\n';
            return 1;
        }
    }
    if (registry_type.empty() || tarball.empty()) {
        std::cerr << "analyze-worker requires --registry and --tarball\n";
        return 1;
    }
    return mcpo::analyze_worker_main(
        registry_type, tarball, rules_path, limits, std::cout, std::cerr);
}

int run_bundle_validate(const char* path) {
    std::string message;
    if (!mcpo::validate_bundle(path, message)) {
        std::cerr << "bundle validation failed: " << message << '\n';
        return 3;
    }
    std::cout << message << '\n';
    return 0;
}

int run_manifest_command(std::string_view command, const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "cannot open target manifest: " << path << '\n';
        return 2;
    }

    const mcpo::ManifestResult result = mcpo::read_target_manifest(input);
    if (!result.ok()) {
        std::cerr << "invalid target manifest at line " << result.error->line
                  << ": " << result.error->message << '\n';
        return 3;
    }

    if (command == "validate-targets") {
        std::cout << "valid target manifest: " << result.summary.records << " records\n";
        return 0;
    }

    std::cout << "records=" << result.summary.records << '\n'
              << "local_packages=" << result.summary.local_packages << '\n'
              << "remote_endpoints=" << result.summary.remote_endpoints << '\n';
    return 0;
}

mcpo::InventoryResult load_inventory(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        mcpo::InventoryResult result;
        result.error = mcpo::InventoryError{std::string("cannot open inventory: ") + path};
        return result;
    }
    return mcpo::read_inventory(input);
}

void print_inventory_diff(const mcpo::InventoryDiff& diff) {
    std::cout << "verdict=material_drift\n"
              << "executable_changed=" << (diff.executable_changed ? "true" : "false") << '\n'
              << "added=" << diff.added.size() << '\n'
              << "removed=" << diff.removed.size() << '\n'
              << "modified=" << diff.modified.size() << '\n';
    for (const auto& name : diff.added) std::cout << "+ " << name << '\n';
    for (const auto& name : diff.removed) std::cout << "- " << name << '\n';
    for (const auto& tool : diff.modified) std::cout << "~ " << tool.name << '\n';
}

int run_compare(const char* before_path, const char* after_path) {
    const mcpo::InventoryResult before = load_inventory(before_path);
    if (!before.ok()) {
        std::cerr << "invalid before inventory: " << before.error->message << '\n';
        return 3;
    }
    const mcpo::InventoryResult after = load_inventory(after_path);
    if (!after.ok()) {
        std::cerr << "invalid after inventory: " << after.error->message << '\n';
        return 3;
    }

    const mcpo::InventoryDiff diff = mcpo::compare_inventories(before.inventory, after.inventory);
    if (diff.identical()) {
        std::cout << "verdict=identical\n";
        return 0;
    }
    print_inventory_diff(diff);
    return 4;
}

mcpo::ObservationResult load_observation(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        mcpo::ObservationResult result;
        result.error = mcpo::ObservationError{std::string("cannot open observation: ") + path};
        return result;
    }
    return mcpo::read_observation(input);
}

int run_validate_observation(const char* path) {
    const mcpo::ObservationResult result = load_observation(path);
    if (!result.ok()) {
        std::cerr << "invalid observation: " << result.error->message << '\n';
        return 3;
    }
    std::cout << "valid observation\n"
              << "target_id=" << result.observation.target_id << '\n'
              << "observed_at=" << result.observation.observed_at << '\n'
              << "tools=" << result.observation.inventory.tools.size() << '\n';
    return 0;
}

int run_ingest_observation(const char* path, const char* history_path) {
    const mcpo::ObservationResult result = load_observation(path);
    if (!result.ok()) {
        std::cerr << "invalid observation: " << result.error->message << '\n';
        return 3;
    }

    std::ofstream output(history_path, std::ios::binary | std::ios::app);
    if (!output) {
        std::cerr << "cannot open history file: " << history_path << '\n';
        return 2;
    }
    std::string error;
    if (!mcpo::append_observation_jsonl(output, result.observation, error)) {
        std::cerr << error << ": " << history_path << '\n';
        return 2;
    }
    std::cout << "ingested observation\n"
              << "target_id=" << result.observation.target_id << '\n'
              << "observed_at=" << result.observation.observed_at << '\n'
              << "history=" << history_path << '\n';
    return 0;
}

mcpo::HistoryResult load_history(const char* path, std::optional<std::string> target_id) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        mcpo::HistoryResult result;
        result.error = mcpo::HistoryError{0U, std::string("cannot open history file: ") + path};
        return result;
    }
    return mcpo::analyze_history(input, std::move(target_id));
}

int report_history_error(const mcpo::HistoryResult& result) {
    std::cerr << "invalid history";
    if (result.error->line != 0U) std::cerr << " at line " << result.error->line;
    std::cerr << ": " << result.error->message << '\n';
    return 3;
}

int run_history_summarize(const char* path) {
    const mcpo::HistoryResult result = load_history(path, std::nullopt);
    if (!result.ok()) return report_history_error(result);

    std::cout << "records=" << result.summary.records << '\n'
              << "targets=" << result.summary.targets << '\n'
              << "earliest_observed_at=" << result.summary.earliest_observed_at << '\n'
              << "latest_observed_at=" << result.summary.latest_observed_at << '\n';
    return 0;
}

int run_history_latest(const char* path, const char* target_id) {
    const mcpo::HistoryResult result = load_history(path, std::string(target_id));
    if (!result.ok()) return report_history_error(result);
    if (!result.target.latest.has_value()) {
        std::cerr << "target not found in history: " << target_id << '\n';
        return 5;
    }

    std::string error;
    if (!mcpo::append_observation_jsonl(std::cout, *result.target.latest, error)) {
        std::cerr << error << '\n';
        return 2;
    }
    return 0;
}

int run_history_diff_latest(const char* path, const char* target_id) {
    const mcpo::HistoryResult result = load_history(path, std::string(target_id));
    if (!result.ok()) return report_history_error(result);
    if (!result.target.latest.has_value() || !result.target.previous.has_value()) {
        std::cerr << "target requires at least two observations: " << target_id << '\n';
        return 5;
    }

    std::cout << "target_id=" << target_id << '\n'
              << "before_observed_at=" << result.target.previous->observed_at << '\n'
              << "after_observed_at=" << result.target.latest->observed_at << '\n';
    const mcpo::InventoryDiff diff = mcpo::compare_inventories(
        result.target.previous->inventory,
        result.target.latest->inventory);
    if (diff.identical()) {
        std::cout << "verdict=identical\n";
        return 0;
    }
    print_inventory_diff(diff);
    return 4;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "about") {
        std::cout
            << "mcp-observatory 0.6.0\n"
            << "bounded longitudinal MCP history analysis\n"
            << "network activity: registry collect/refresh and analyze package download\n"
            << "external process execution: curl, OpenSSL, gzip, and Docker analyze-worker\n";
        return 0;
    }

    if (argc >= 3 && std::string_view(argv[1]) == "analyze" &&
        std::string_view(argv[2]) == "package")
        return run_analyze_package(argc, argv);
    if (argc >= 2 && std::string_view(argv[1]) == "analyze-worker")
        return run_analyze_worker(argc, argv);

    if (argc >= 3 && std::string_view(argv[1]) == "registry" &&
        std::string_view(argv[2]) == "collect")
        return run_registry_collect(argc, argv);
    if (argc >= 3 && std::string_view(argv[1]) == "registry" &&
        std::string_view(argv[2]) == "refresh")
        return run_registry_refresh(argc, argv);
    if (argc >= 3 && std::string_view(argv[1]) == "registry" &&
        std::string_view(argv[2]) == "index")
        return run_registry_index(argc, argv);
    if (argc >= 4 && std::string_view(argv[1]) == "registry" &&
        std::string_view(argv[2]) == "summarize")
        return run_registry_summarize(argc, argv);
    if (argc >= 5 && std::string_view(argv[1]) == "registry" &&
        std::string_view(argv[2]) == "search")
        return run_registry_search(argc, argv);
    if (argc >= 5 && std::string_view(argv[1]) == "registry" &&
        std::string_view(argv[2]) == "show")
        return run_registry_show(argc, argv);
    if (argc >= 4 && std::string_view(argv[1]) == "registry" &&
        std::string_view(argv[2]) == "list")
        return run_registry_list(argc, argv);
    if (argc >= 5 && std::string_view(argv[1]) == "registry" &&
        std::string_view(argv[2]) == "checkpoint" &&
        std::string_view(argv[3]) == "reconstruct")
        return run_checkpoint_reconstruct(argc, argv);
    if (argc == 4 && std::string_view(argv[1]) == "bundle" &&
        std::string_view(argv[2]) == "validate")
        return run_bundle_validate(argv[3]);

    if (argc == 3) {
        const std::string_view command(argv[1]);
        if (command == "validate-targets" || command == "summarize-targets")
            return run_manifest_command(command, argv[2]);
        if (command == "validate-observation") return run_validate_observation(argv[2]);
    }

    if (argc == 4 && std::string_view(argv[1]) == "compare-inventories")
        return run_compare(argv[2], argv[3]);
    if (argc == 4 && std::string_view(argv[1]) == "ingest-observation")
        return run_ingest_observation(argv[2], argv[3]);
    if (argc == 4 && std::string_view(argv[1]) == "history" &&
        std::string_view(argv[2]) == "summarize")
        return run_history_summarize(argv[3]);
    if (argc == 5 && std::string_view(argv[1]) == "history") {
        const std::string_view command(argv[2]);
        if (command == "latest") return run_history_latest(argv[3], argv[4]);
        if (command == "diff-latest") return run_history_diff_latest(argv[3], argv[4]);
    }

    print_usage(std::cerr);
    return 1;
}
