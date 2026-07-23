#include "observatory/history.hpp"
#include "observatory/inventory.hpp"
#include "observatory/observation.hpp"
#include "observatory/target_manifest.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

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

mcpo::HistoryResult parse_history(
    std::string_view text,
    std::optional<std::string> target_id = std::nullopt,
    mcpo::HistoryLimits limits = {}) {
    std::istringstream input{std::string(text)};
    return mcpo::analyze_history(input, std::move(target_id), limits);
}

constexpr std::string_view base_inventory =
    "{\"inventory_version\":1,\"server\":{\"downstream_executable\":\"node\"},"
    "\"tools\":[{\"name\":\"read_file\",\"inputSchema\":{\"type\":\"object\"}},"
    "{\"name\":\"search\",\"description\":\"Find records\"}]}";

constexpr std::string_view changed_inventory =
    "{\"inventory_version\":1,\"server\":{\"downstream_executable\":\"node\"},"
    "\"tools\":[{\"name\":\"execute\",\"inputSchema\":{}},"
    "{\"name\":\"read_file\",\"inputSchema\":{\"properties\":{},\"type\":\"object\"}}]}";

std::string observation(
    std::string_view observed_at = "2026-07-24T20:15:30Z",
    std::string_view target_id = "local:filesystem:2026.7.10",
    std::string_view inventory = base_inventory) {
    return std::string("{\"observation_version\":1,\"observed_at\":\"") +
        std::string(observed_at) + "\",\"target_id\":\"" + std::string(target_id) +
        "\",\"source_type\":\"controlled_corpus\","
        "\"sensor\":{\"name\":\"mcp-native-guard\",\"version\":\"0.1.0\"},"
        "\"configuration_profile\":\"default-no-network\",\"inventory\":" +
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
    }

    require(!parse_inventory("{}").ok(), "missing inventory fields should fail");
    require(!parse_inventory(
        "{\"inventory_version\":1,\"inventory_version\":1,\"server\":{\"downstream_executable\":\"x\"},\"tools\":[]}").ok(),
        "duplicate inventory version should fail");
    require(!parse_inventory(
        "{\"inventory_version\":1,\"server\":{\"downstream_executable\":\"x\"},\"tools\":[{\"name\":\"z\"},{\"name\":\"a\"}]}").ok(),
        "unsorted tools should fail");

    {
        const auto before = parse_inventory(base_inventory);
        const auto after = parse_inventory(changed_inventory);
        require(before.ok() && after.ok(), "drift fixtures should parse");
        const auto diff = mcpo::compare_inventories(before.inventory, after.inventory);
        require(diff.added.size() == 1U && diff.added[0] == "execute", "added tool");
        require(diff.removed.size() == 1U && diff.removed[0] == "search", "removed tool");
        require(diff.modified.size() == 1U && diff.modified[0].name == "read_file", "modified tool");
    }

    {
        const auto result = parse_observation(observation());
        require(result.ok(), "valid observation should pass");
        require(result.observation.target_id == "local:filesystem:2026.7.10", "observation target");
        std::ostringstream history;
        std::string error;
        require(mcpo::append_observation_jsonl(history, result.observation, error), "append observation");
        require(history.str().find('\n') == history.str().size() - 1U, "history record is one JSONL line");
    }

    require(!parse_observation("{}").ok(), "missing observation fields should fail");
    require(!parse_observation(
        "{\"observation_version\":1,\"observed_at\":\"24-07-2026\","
        "\"target_id\":\"x\",\"source_type\":\"s\",\"sensor\":{\"name\":\"n\",\"version\":\"v\"},"
        "\"configuration_profile\":\"p\",\"inventory\":" + std::string(base_inventory) + "}").ok(),
        "invalid observation timestamp should fail");

    {
        const std::string history_text =
            observation("2026-07-24T20:15:30Z", "target:a", base_inventory) + "\n" +
            observation("2026-07-24T18:00:00Z", "target:b", base_inventory) + "\n" +
            observation("2026-07-25T09:00:00Z", "target:a", changed_inventory) + "\n";
        const auto result = parse_history(history_text, std::string("target:a"));
        require(result.ok(), "valid history should pass");
        require(result.summary.records == 3U, "history record count");
        require(result.summary.targets == 2U, "history target count");
        require(result.summary.earliest_observed_at == "2026-07-24T18:00:00Z", "earliest timestamp");
        require(result.summary.latest_observed_at == "2026-07-25T09:00:00Z", "latest timestamp");
        require(result.target.records == 2U, "selected target count");
        require(result.target.previous.has_value() && result.target.latest.has_value(), "two latest observations retained");
        require(result.target.previous->observed_at == "2026-07-24T20:15:30Z", "previous target observation");
        require(result.target.latest->observed_at == "2026-07-25T09:00:00Z", "latest target observation");
        require(!mcpo::compare_inventories(
            result.target.previous->inventory,
            result.target.latest->inventory).identical(), "latest target drift detected");
    }

    {
        const std::string out_of_order =
            observation("2026-07-25T09:00:00Z", "target:a", changed_inventory) + "\n" +
            observation("2026-07-23T09:00:00Z", "target:a", base_inventory) + "\n" +
            observation("2026-07-24T09:00:00Z", "target:a", base_inventory) + "\n";
        const auto result = parse_history(out_of_order, std::string("target:a"));
        require(result.ok(), "out-of-order history should pass");
        require(result.target.previous->observed_at == "2026-07-24T09:00:00Z", "chronological previous selected");
        require(result.target.latest->observed_at == "2026-07-25T09:00:00Z", "chronological latest selected");
    }

    require(!parse_history(observation() + "\nnot-json\n").ok(), "malformed history line should fail");

    {
        mcpo::HistoryLimits limits;
        limits.max_records = 1U;
        const auto result = parse_history(observation() + "\n" + observation() + "\n", std::nullopt, limits);
        require(!result.ok(), "history record limit should be enforced");
    }

    {
        mcpo::HistoryLimits limits;
        limits.max_targets = 1U;
        const auto result = parse_history(
            observation("2026-07-24T20:15:30Z", "target:a") + "\n" +
            observation("2026-07-24T20:15:31Z", "target:b") + "\n",
            std::nullopt,
            limits);
        require(!result.ok(), "history target limit should be enforced");
    }

    return 0;
}
