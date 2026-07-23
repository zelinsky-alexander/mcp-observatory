#pragma once

#include "observatory/observation.hpp"

#include <cstddef>
#include <istream>
#include <optional>
#include <string>

namespace mcpo {

struct HistoryLimits {
    std::size_t max_line_bytes{20U * 1024U * 1024U};
    std::size_t max_records{100'000U};
    std::size_t max_targets{10'000U};
    ObservationLimits observation{};
};

struct HistorySummary {
    std::size_t records{};
    std::size_t targets{};
    std::string earliest_observed_at;
    std::string latest_observed_at;
};

struct TargetHistory {
    std::size_t records{};
    std::optional<Observation> previous;
    std::optional<Observation> latest;
};

struct HistoryError {
    std::size_t line{};
    std::string message;
};

struct HistoryResult {
    HistorySummary summary;
    TargetHistory target;
    std::optional<HistoryError> error;

    [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

[[nodiscard]] HistoryResult analyze_history(
    std::istream& input,
    std::optional<std::string> target_id = std::nullopt,
    HistoryLimits limits = {});

}  // namespace mcpo
