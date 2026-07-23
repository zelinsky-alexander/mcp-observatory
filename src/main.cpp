#include "observatory/target_manifest.hpp"

#include <fstream>
#include <iostream>
#include <string_view>

namespace {

void print_usage(std::ostream& out) {
    out << "usage:\n"
        << "  mcp-observatory about\n"
        << "  mcp-observatory validate-targets PATH\n"
        << "  mcp-observatory summarize-targets PATH\n";
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

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "about") {
        std::cout
            << "mcp-observatory 0.1.0\n"
            << "bounded longitudinal MCP security research scaffold\n"
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

    print_usage(std::cerr);
    return 1;
}
