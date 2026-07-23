#include "observatory/history.hpp"

#include <set>
#include <sstream>
#include <utility>

namespace mcpo {
namespace {

void update_target_history(TargetHistory& history, Observation observation) {
    ++history.records;
    if (!history.latest.has_value() || observation.observed_at >= history.latest->observed_at) {
        history.previous = std::move(history.latest);
        history.latest = std::move(observation);
        return;
    }
    if (!history.previous.has_value() || observation.observed_at >= history.previous->observed_at) {
        history.previous = std::move(observation);
    }
}

}  // namespace

HistoryResult analyze_history(
    std::istream& input,
    std::optional<std::string> target_id,
    HistoryLimits limits) {
    HistoryResult result;
    std::set<std::string> targets;
    std::string line;
    std::size_t line_number = 0U;

    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty()) {
            continue;
        }
        if (line.size() > limits.max_line_bytes) {
            result.error = HistoryError{line_number, "history line exceeds configured byte limit"};
            return result;
        }
        if (result.summary.records >= limits.max_records) {
            result.error = HistoryError{line_number, "history exceeds configured record limit"};
            return result;
        }

        std::istringstream observation_input(line);
        ObservationResult parsed = read_observation(observation_input, limits.observation);
        if (!parsed.ok()) {
            result.error = HistoryError{line_number, "invalid observation: " + parsed.error->message};
            return result;
        }

        ++result.summary.records;
        const std::string& observed_at = parsed.observation.observed_at;
        if (result.summary.earliest_observed_at.empty() ||
            observed_at < result.summary.earliest_observed_at) {
            result.summary.earliest_observed_at = observed_at;
        }
        if (result.summary.latest_observed_at.empty() ||
            observed_at > result.summary.latest_observed_at) {
            result.summary.latest_observed_at = observed_at;
        }

        const auto [iterator, inserted] = targets.insert(parsed.observation.target_id);
        static_cast<void>(iterator);
        if (inserted && targets.size() > limits.max_targets) {
            result.error = HistoryError{line_number, "history exceeds configured target limit"};
            return result;
        }

        if (target_id.has_value() && parsed.observation.target_id == *target_id) {
            update_target_history(result.target, std::move(parsed.observation));
        }
    }

    if (input.bad()) {
        result.error = HistoryError{line_number, "I/O failure while reading history"};
        return result;
    }

    result.summary.targets = targets.size();
    return result;
}

}  // namespace mcpo
