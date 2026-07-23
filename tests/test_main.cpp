#include "observatory/inventory.hpp"
#include "observatory/observation.hpp"
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

mcpo::ObservationResult parse_observation(std::string_view text, mcpo::ObservationLimits limits = {}) {
    std::istringstream input{std::string(text)};
    return mcpo::read_observation(input, limits);
}

constexpr std::string_view base_inventory =
    "{\"inventory_version\":1,\"server\":{\"downstream_executable\":\"node\"},"
    "\"tools\":[{\"name\":\"read_file\",\"inputSchema\":{\"type\":\"object\"}},"
    "{\"name\":\"search\",\"description\":\"Find records\"}]}";

std::string observation(std::string_view inventory = base_inventory) {
    return std::string(
        "{\"observation_version\":1,\"observed_at\":\"2026-07-24T20:15:30Z\","
        "\"target_id\":\"local:filesystem:2026.7.10\",\"source_type\":\"controlled_corpus\","
        "\"sensor\":{\"name\":\"mcp-native-guard\",\"version\":\"0.1.0\"},"
        "\"configuration_profile\":\"default-no-network\",\"inventory\":") +
        std::string(inventory) + "}";
}

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

    {
        const auto before = parse_inventory(base_inventory);
        const auto after = parse_inventory(
            "{\"inventory_version\":1,\"server\":{\"downstream_executable\":\"python\"},"
            "\"tools\":[{\"name\":\"execute\",\"inputSchema\":{}},"
            "{\"name\":\"read_file\",\"inputSchema\":{\"properties\":{},\"type\":\"object\"}}]}"
        );
        require(before.ok() && after.ok(), "drift fixtures should parse");
        const auto diff = mcpo::compare_inventories(before.inventory, after.inventory);
        require(diff.executable_changed, "executable change should be detected");
        require(diff.added.size() == 1U && diff.added[0] == "execute", "added tool");
        require(diff.removed.size() == 1U && diff.removed[0] == "search", "removed tool");
        require(diff.modified.size() == 1U && diff.modified[0].name == "read_file", "modified tool");
    }

    {
        const auto result = parse_observation(observation());
        require(result.ok(), "valid observation should pass");
        require(result.observation.version == 1U, "observation version");
        require(result.observation.target_id == "local:filesystem:2026.7.10", "observation target");
        require(result.observation.sensor_name == "mcp-native-guard", "sensor name");
        require(result.observation.inventory.tools.size() == 2U, "embedded inventory parsed");

        std::ostringstream history;
        std::string error;
        require(mcpo::append_observation_jsonl(history, result.observation, error), "append observation");
        require(history.str().find('\n') == history.str().size() - 1U, "history record is one JSONL line");
    }

    require(!parse_observation("{}").ok(), "missing observation fields should fail");
    require(!parse_observation(
        "{\"observation_version\":2,\"observed_at\":\"2026-07-24T20:15:30Z\","
        "\"target_id\":\"x\",\"source_type\":\"s\",\"sensor\":{\"name\":\"n\",\"version\":\"v\"},"
        "\"configuration_profile\":\"p\",\"inventory\":" + std::string(base_inventory) + "}").ok(),
        "unsupported observation version should fail");
    require(!parse_observation(
        "{\"observation_version\":1,\"observed_at\":\"24-07-2026\","
        "\"target_id\":\"x\",\"source_type\":\"s\",\"sensor\":{\"name\":\"n\",\"version\":\"v\"},"
        "\"configuration_profile\":\"p\",\"inventory\":" + std::string(base_inventory) + "}").ok(),
        "invalid observation timestamp should fail");
    require(!parse_observation(
        "{\"observation_version\":1,\"observation_version\":1,\"observed_at\":\"2026-07-24T20:15:30Z\","
        "\"target_id\":\"x\",\"source_type\":\"s\",\"sensor\":{\"name\":\"n\",\"version\":\"v\"},"
        "\"configuration_profile\":\"p\",\"inventory\":" + std::string(base_inventory) + "}").ok(),
        "duplicate observation field should fail");
    require(!parse_observation(observation("{}" )).ok(), "invalid embedded inventory should fail");

    {
        mcpo::ObservationLimits limits;
        limits.max_bytes = 32U;
        require(!parse_observation(observation(), limits).ok(), "observation byte limit should be enforced");
    }

    return 0;
}
