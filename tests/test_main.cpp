#include "observatory/inventory.hpp"
#include "observatory/target_manifest.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}

mcpo::ManifestResult parse_manifest(std::string_view text, mcpo::ReadLimits limits = {}) {
    std::istringstream input{std::string(text)};
    return mcpo::read_target_manifest(input, limits);
}

mcpo::InventoryResult parse_inventory(std::string_view text, mcpo::InventoryLimits limits = {}) {
    std::istringstream input{std::string(text)};
    return mcpo::read_inventory(input, limits);
}

constexpr std::string_view base_inventory =
    "{\"inventory_version\":1,\"server\":{\"downstream_executable\":\"node\"},"
    "\"tools\":[{\"name\":\"read_file\",\"inputSchema\":{\"type\":\"object\"}},"
    "{\"name\":\"search\",\"description\":\"Find records\"}]}";

}  // namespace

int main() {
    {
        const auto result = parse_manifest(
            "{\"schema_version\":1,\"target_id\":\"registry:a:1\","
            "\"source_type\":\"official_registry\",\"kind\":\"local_package\"}\n"
            "{\"kind\":\"remote_endpoint\",\"source_type\":\"operator_submission\","
            "\"target_id\":\"remote:b\",\"schema_version\":1,\"metadata\":{\"x\":[1,true,null]}}\n");
        require(result.ok(), "valid target records should pass");
        require(result.summary.records == 2U, "target record count");
    }

    require(!parse_manifest("{}\n").ok(), "missing target fields should fail");
    require(!parse_manifest("{\"schema_version\":1,\"schema_version\":1,\"target_id\":\"x\",\"source_type\":\"y\",\"kind\":\"local_package\"}\n").ok(),
            "duplicate target security field should fail");

    {
        const auto result = parse_inventory(base_inventory);
        require(result.ok(), "valid inventory should pass");
        require(result.inventory.version == 1U, "inventory version");
        require(result.inventory.tools.size() == 2U, "inventory tool count");
        require(result.inventory.tools[0].name == "read_file", "first tool name");
    }

    require(!parse_inventory("{}").ok(), "missing inventory fields should fail");
    require(!parse_inventory(
        "{\"inventory_version\":1,\"inventory_version\":1,\"server\":{\"downstream_executable\":\"x\"},\"tools\":[]}").ok(),
        "duplicate inventory version should fail");
    require(!parse_inventory(
        "{\"inventory_version\":2,\"server\":{\"downstream_executable\":\"x\"},\"tools\":[]}").ok(),
        "unsupported inventory version should fail");
    require(!parse_inventory(
        "{\"inventory_version\":1,\"server\":{\"downstream_executable\":\"x\"},\"tools\":[{\"name\":\"z\"},{\"name\":\"a\"}]}").ok(),
        "unsorted tools should fail");
    require(!parse_inventory(
        "{\"inventory_version\":1,\"server\":{\"downstream_executable\":\"x\"},\"tools\":[{\"name\":\"a\"},{\"name\":\"a\"}]}").ok(),
        "duplicate tool names should fail");
    require(!parse_inventory(
        "{\"inventory_version\":1,\"server\":{\"downstream_executable\":\"x\"},\"tools\":[{\"name\":\"read\\u005ffile\"}]}").ok(),
        "escaped identity string should fail");

    {
        mcpo::InventoryLimits limits;
        limits.max_bytes = 16U;
        require(!parse_inventory(base_inventory, limits).ok(), "inventory byte limit should be enforced");
    }

    {
        mcpo::InventoryLimits limits;
        limits.max_tools = 1U;
        require(!parse_inventory(base_inventory, limits).ok(), "tool count limit should be enforced");
    }

    {
        const auto before = parse_inventory(base_inventory);
        const auto identical = parse_inventory(base_inventory);
        require(before.ok() && identical.ok(), "comparison fixtures should parse");
        require(mcpo::compare_inventories(before.inventory, identical.inventory).identical(),
                "equal inventories should be identical");
    }

    {
        const auto before = parse_inventory(base_inventory);
        const auto after = parse_inventory(
            "{\"inventory_version\":1,\"server\":{\"downstream_executable\":\"python\"},"
            "\"tools\":[{\"name\":\"execute\",\"inputSchema\":{}},"
            "{\"name\":\"read_file\",\"inputSchema\":{\"properties\":{},\"type\":\"object\"}}]}"
        );
        require(before.ok() && after.ok(), "drift fixtures should parse");
        const auto diff = mcpo::compare_inventories(before.inventory, after.inventory);
        require(!diff.identical(), "drift should be detected");
        require(diff.executable_changed, "executable change should be detected");
        require(diff.added.size() == 1U && diff.added[0] == "execute", "added tool");
        require(diff.removed.size() == 1U && diff.removed[0] == "search", "removed tool");
        require(diff.modified.size() == 1U && diff.modified[0].name == "read_file", "modified tool");
    }

    return 0;
}
