#include "observatory/history.hpp"
#include "observatory/inventory.hpp"
#include "observatory/observation.hpp"
#include "observatory/target_manifest.hpp"

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
        << "  mcp-observatory history diff-latest HISTORY_JSONL TARGET_ID\n";
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
            << "mcp-observatory 0.4.0\n"
            << "bounded longitudinal MCP history analysis\n"
            << "network activity: disabled\n"
            << "external process execution: disabled\n";
        return 0;
    }

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
