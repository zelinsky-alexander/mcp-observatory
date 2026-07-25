#include "observatory/history.hpp"
#include "observatory/inventory.hpp"
#include "observatory/observation.hpp"
#include "observatory/registry.hpp"
#include "observatory/target_manifest.hpp"

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

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
        << "  mcp-observatory registry checkpoint reconstruct PARTIAL_DIRECTORY [OPTIONS]\n"
        << "  mcp-observatory bundle validate DIRECTORY\n";
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

int run_registry_collect(int argc, char** argv) {
    mcpo::RegistryCollectOptions options;
    if (const char* environment = std::getenv("MCPO_REGISTRY_BASE_URL");
        environment != nullptr && *environment != '\0') {
        options.registry_base_url = environment;
    }
    bool have_output = false;
    for (int index = 3; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--retain-raw") {
            options.retain_raw = true;
            continue;
        }
        if (argument == "--verbose") {
            options.verbose = true;
            continue;
        }
        if (index + 1 >= argc) {
            std::cerr << "missing value for " << argument << '\n';
            return 1;
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
            if (!parse_size(value, options.limits.maximum_pages)) return 1;
        } else if (argument == "--maximum-page-bytes") {
            if (!parse_size(value, options.limits.maximum_page_bytes)) return 1;
        } else if (argument == "--maximum-records") {
            if (!parse_size(value, options.limits.maximum_records)) return 1;
        } else if (argument == "--maximum-redirects") {
            if (!parse_size(value, options.limits.maximum_redirects)) return 1;
        } else if (argument == "--request-timeout-seconds") {
            if (!parse_unsigned(value, options.limits.request_timeout_seconds)) return 1;
        } else if (argument == "--run-timeout-seconds") {
            if (!parse_unsigned(value, options.limits.run_timeout_seconds)) return 1;
        } else {
            std::cerr << "unknown registry collect option: " << argument << '\n';
            return 1;
        }
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
            << "mcp-observatory 0.5.0\n"
            << "bounded longitudinal MCP history analysis\n"
            << "network activity: registry collect only\n"
            << "external process execution: explicit curl and OpenSSL only\n";
        return 0;
    }

    if (argc >= 3 && std::string_view(argv[1]) == "registry" &&
        std::string_view(argv[2]) == "collect")
        return run_registry_collect(argc, argv);
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
