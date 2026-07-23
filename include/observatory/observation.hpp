#pragma once

#include "observatory/inventory.hpp"

#include <cstddef>
#include <cstdint>
#include <istream>
#include <optional>
#include <ostream>
#include <string>

namespace mcpo {

struct ObservationLimits {
    std::size_t max_bytes{20U * 1024U * 1024U};
    std::size_t max_identity_bytes{1'024U};
    std::size_t max_nesting_depth{64U};
    InventoryLimits inventory{};
};

struct Observation {
    std::uint32_t version{};
    std::string observed_at;
    std::string target_id;
    std::string source_type;
    std::string sensor_name;
    std::string sensor_version;
    std::string configuration_profile;
    Inventory inventory;
    std::string inventory_json;
};

struct ObservationError {
    std::string message;
};

struct ObservationResult {
    Observation observation;
    std::optional<ObservationError> error;

    [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

[[nodiscard]] ObservationResult read_observation(
    std::istream& input,
    ObservationLimits limits = {});

[[nodiscard]] bool append_observation_jsonl(
    std::ostream& output,
    const Observation& observation,
    std::string& error);

}  // namespace mcpo
