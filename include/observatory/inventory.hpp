#pragma once

#include <cstddef>
#include <cstdint>
#include <istream>
#include <optional>
#include <string>
#include <vector>

namespace mcpo {

struct InventoryLimits {
    std::size_t max_bytes{16U * 1024U * 1024U};
    std::size_t max_tools{1'024U};
    std::size_t max_nesting_depth{64U};
    std::size_t max_tool_name_bytes{1'024U};
};

struct InventoryTool {
    std::string name;
    std::string canonical_definition;
};

struct Inventory {
    std::uint32_t version{};
    std::string downstream_executable;
    std::vector<InventoryTool> tools;
};

struct InventoryError {
    std::string message;
};

struct InventoryResult {
    Inventory inventory;
    std::optional<InventoryError> error;

    [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

struct ModifiedTool {
    std::string name;
};

struct InventoryDiff {
    bool executable_changed{};
    std::vector<std::string> added;
    std::vector<std::string> removed;
    std::vector<ModifiedTool> modified;

    [[nodiscard]] bool identical() const noexcept {
        return !executable_changed && added.empty() && removed.empty() && modified.empty();
    }
};

[[nodiscard]] InventoryResult read_inventory(
    std::istream& input,
    InventoryLimits limits = {});

[[nodiscard]] InventoryDiff compare_inventories(
    const Inventory& before,
    const Inventory& after);

}  // namespace mcpo
