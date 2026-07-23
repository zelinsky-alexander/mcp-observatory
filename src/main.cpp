#include "observatory/inventory.hpp"
#include "observatory/target_manifest.hpp"

#include <fstream>
#include <iostream>
#include <string_view>

namespace {

void print_usage(std::ostream& out) {
    out << "usage:\n"
        << "  mcp-observatory about\n"
        << "  mcp-observatory validate-targets PATH\n"
        << "  mcp-observatory summarize-targets PATH\n"
        << "  mcp-observatory compare-inventories BEFORE AFTER\n";
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

    std::cout << "verdict=material_drift\n"
              << "executable_changed=" << (diff.executable_changed ? "true" : "false") << '\n'
              << "added=" << diff.added.size() << '\n'
              << "removed=" << diff.removed.size() << '\n'
              << "modified=" << diff.modified.size() << '\n';

    for (const auto& name : diff.added) {
        std::cout << "+ " << name << '\n';
    }
    for (const auto& name : diff.removed) {
        std::cout << "- " << name << '\n';
    }
    for (const auto& tool : diff.modified) {
        std::cout << "~ " << tool.name << '\n';
    }
    return 4;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "about") {
        std::cout
            << "mcp-observatory 0.2.0\n"
            << "bounded longitudinal MCP inventory comparison\n"
            << "network activity: disabled\n"
            << "external process execution: disabled\n";
        return 0;
    }

    if (argc == 3) {
        const std::string_view command(argv[1]);
        if (command == "validate-targets" || command == "summarize-targets") {
            return run_manifest_command(command, argv[2]);
        }
    }

    if (argc == 4 && std::string_view(argv[1]) == "compare-inventories") {
        return run_compare(argv[2], argv[3]);
    }

    print_usage(std::cerr);
    return 1;
}
