#include "observatory/observation.hpp"

#include <algorithm>
#include <charconv>
#include <sstream>
#include <string_view>

namespace mcpo {
namespace {

[[nodiscard]] bool valid_utc_timestamp(std::string_view value) noexcept {
    if (value.size() != 20U || value[4] != '-' || value[7] != '-' ||
        value[10] != 'T' || value[13] != ':' || value[16] != ':' || value[19] != 'Z') {
        return false;
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 4U || i == 7U || i == 10U || i == 13U || i == 16U || i == 19U) {
            continue;
        }
        if (value[i] < '0' || value[i] > '9') {
            return false;
        }
    }
    const auto number = [value](std::size_t offset) {
        return static_cast<unsigned>((value[offset] - '0') * 10 + (value[offset + 1U] - '0'));
    };
    const unsigned month = number(5U);
    const unsigned day = number(8U);
    const unsigned hour = number(11U);
    const unsigned minute = number(14U);
    const unsigned second = number(17U);
    return month >= 1U && month <= 12U && day >= 1U && day <= 31U &&
           hour <= 23U && minute <= 59U && second <= 60U;
}

[[nodiscard]] std::string compact_json(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    bool in_string = false;
    bool escaped = false;
    for (const char c : input) {
        if (in_string) {
            output.push_back(c);
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
        } else if (c == '"') {
            in_string = true;
            output.push_back(c);
        } else if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            output.push_back(c);
        }
    }
    return output;
}

class Parser {
public:
    Parser(std::string_view text, ObservationLimits limits) : text_(text), limits_(limits) {}

    [[nodiscard]] bool parse(Observation& out, std::string& error) {
        skip_space();
        if (!consume('{')) return fail(error, "observation must be a JSON object");

        bool have_version = false;
        bool have_time = false;
        bool have_target = false;
        bool have_source = false;
        bool have_sensor = false;
        bool have_profile = false;
        bool have_inventory = false;

        skip_space();
        if (consume('}')) return fail(error, "observation is missing required fields");
        while (true) {
            std::string key;
            if (!plain_string(key, error)) return false;
            skip_space();
            if (!consume(':')) return fail(error, "expected ':' after observation member");
            skip_space();

            if (key == "observation_version") {
                if (have_version) return fail(error, "duplicate observation_version");
                have_version = true;
                if (!uint32(out.version, error)) return false;
            } else if (key == "observed_at") {
                if (have_time) return fail(error, "duplicate observed_at");
                have_time = true;
                if (!identity(out.observed_at, error) || !valid_utc_timestamp(out.observed_at))
                    return fail(error, "observed_at must use YYYY-MM-DDTHH:MM:SSZ");
            } else if (key == "target_id") {
                if (have_target) return fail(error, "duplicate target_id");
                have_target = true;
                if (!identity(out.target_id, error)) return false;
            } else if (key == "source_type") {
                if (have_source) return fail(error, "duplicate source_type");
                have_source = true;
                if (!identity(out.source_type, error)) return false;
            } else if (key == "sensor") {
                if (have_sensor) return fail(error, "duplicate sensor");
                have_sensor = true;
                if (!sensor(out, error)) return false;
            } else if (key == "configuration_profile") {
                if (have_profile) return fail(error, "duplicate configuration_profile");
                have_profile = true;
                if (!identity(out.configuration_profile, error)) return false;
            } else if (key == "inventory") {
                if (have_inventory) return fail(error, "duplicate inventory");
                have_inventory = true;
                const std::size_t begin = position_;
                if (!skip_value(error, 0U)) return false;
                out.inventory_json.assign(text_.substr(begin, position_ - begin));
            } else if (!skip_value(error, 0U)) {
                return false;
            }

            skip_space();
            if (consume('}')) break;
            if (!consume(',')) return fail(error, "expected ',' or '}' in observation");
            skip_space();
        }
        skip_space();
        if (position_ != text_.size()) return fail(error, "unexpected data after observation");
        if (!have_version || !have_time || !have_target || !have_source || !have_sensor ||
            !have_profile || !have_inventory) {
            return fail(error, "observation is missing required fields");
        }
        if (out.version != 1U) return fail(error, "unsupported observation_version");

        std::istringstream inventory_input(out.inventory_json);
        const InventoryResult inventory_result = read_inventory(inventory_input, limits_.inventory);
        if (!inventory_result.ok()) {
            return fail(error, "invalid embedded inventory: " + inventory_result.error->message);
        }
        out.inventory = inventory_result.inventory;
        out.inventory_json = compact_json(out.inventory_json);
        return true;
    }

private:
    [[nodiscard]] static bool fail(std::string& error, std::string message) {
        error = std::move(message);
        return false;
    }

    void skip_space() noexcept {
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            ++position_;
        }
    }

    [[nodiscard]] bool consume(char expected) noexcept {
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool plain_string(std::string& out, std::string& error) {
        if (!consume('"')) return fail(error, "expected JSON string");
        const std::size_t begin = position_;
        while (position_ < text_.size()) {
            const char c = text_[position_++];
            if (c == '"') {
                out.assign(text_.substr(begin, position_ - begin - 1U));
                return true;
            }
            if (c == '\\' || static_cast<unsigned char>(c) < 0x20U)
                return fail(error, "escaped or control characters are not supported in identity strings");
        }
        return fail(error, "unterminated JSON string");
    }

    [[nodiscard]] bool identity(std::string& out, std::string& error) {
        if (!plain_string(out, error)) return false;
        if (out.empty()) return fail(error, "identity string must not be empty");
        if (out.size() > limits_.max_identity_bytes)
            return fail(error, "identity string exceeds configured limit");
        return true;
    }

    [[nodiscard]] bool uint32(std::uint32_t& out, std::string& error) {
        const std::size_t begin = position_;
        while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
        if (begin == position_) return fail(error, "observation_version must be an unsigned integer");
        const std::string_view token = text_.substr(begin, position_ - begin);
        const auto result = std::from_chars(token.data(), token.data() + token.size(), out);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size())
            return fail(error, "observation_version is out of range");
        return true;
    }

    [[nodiscard]] bool sensor(Observation& out, std::string& error) {
        if (!consume('{')) return fail(error, "sensor must be an object");
        bool have_name = false;
        bool have_version = false;
        skip_space();
        if (consume('}')) return fail(error, "sensor is missing required fields");
        while (true) {
            std::string key;
            if (!plain_string(key, error)) return false;
            skip_space();
            if (!consume(':')) return fail(error, "expected ':' in sensor object");
            skip_space();
            if (key == "name") {
                if (have_name) return fail(error, "duplicate sensor name");
                have_name = true;
                if (!identity(out.sensor_name, error)) return false;
            } else if (key == "version") {
                if (have_version) return fail(error, "duplicate sensor version");
                have_version = true;
                if (!identity(out.sensor_version, error)) return false;
            } else if (!skip_value(error, 1U)) {
                return false;
            }
            skip_space();
            if (consume('}')) break;
            if (!consume(',')) return fail(error, "expected ',' or '}' in sensor object");
            skip_space();
        }
        if (!have_name || !have_version) return fail(error, "sensor is missing required fields");
        return true;
    }

    [[nodiscard]] bool skip_string(std::string& error) {
        if (!consume('"')) return false;
        while (position_ < text_.size()) {
            const char c = text_[position_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (position_ >= text_.size()) return fail(error, "unterminated JSON escape");
                ++position_;
            } else if (static_cast<unsigned char>(c) < 0x20U) {
                return fail(error, "control character in JSON string");
            }
        }
        return fail(error, "unterminated JSON string");
    }

    [[nodiscard]] bool skip_value(std::string& error, std::size_t depth) {
        if (depth > limits_.max_nesting_depth)
            return fail(error, "JSON value exceeds configured nesting limit");
        skip_space();
        if (position_ >= text_.size()) return fail(error, "missing JSON value");
        if (text_[position_] == '"') return skip_string(error);
        if (text_[position_] == '{') {
            ++position_;
            skip_space();
            if (consume('}')) return true;
            while (true) {
                if (!skip_string(error)) return fail(error, error.empty() ? "object key must be a string" : error);
                skip_space();
                if (!consume(':')) return fail(error, "expected ':' in object");
                if (!skip_value(error, depth + 1U)) return false;
                skip_space();
                if (consume('}')) return true;
                if (!consume(',')) return fail(error, "expected ',' or '}' in object");
                skip_space();
            }
        }
        if (text_[position_] == '[') {
            ++position_;
            skip_space();
            if (consume(']')) return true;
            while (true) {
                if (!skip_value(error, depth + 1U)) return false;
                skip_space();
                if (consume(']')) return true;
                if (!consume(',')) return fail(error, "expected ',' or ']' in array");
                skip_space();
            }
        }
        const std::size_t begin = position_;
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' || c == '\r' || c == '\n') break;
            ++position_;
        }
        if (begin == position_) return fail(error, "invalid JSON value");
        return true;
    }

    std::string_view text_;
    ObservationLimits limits_;
    std::size_t position_{};
};

}  // namespace

ObservationResult read_observation(std::istream& input, ObservationLimits limits) {
    ObservationResult result;
    std::string text;
    text.reserve(std::min<std::size_t>(limits.max_bytes, 64U * 1024U));
    char buffer[4096];
    while (input.read(buffer, static_cast<std::streamsize>(sizeof(buffer))) || input.gcount() > 0) {
        const std::size_t count = static_cast<std::size_t>(input.gcount());
        if (count > limits.max_bytes - std::min(limits.max_bytes, text.size())) {
            result.error = ObservationError{"observation exceeds configured byte limit"};
            return result;
        }
        text.append(buffer, count);
    }
    if (input.bad()) {
        result.error = ObservationError{"I/O failure while reading observation"};
        return result;
    }
    std::string error;
    Parser parser(text, limits);
    if (!parser.parse(result.observation, error)) result.error = ObservationError{std::move(error)};
    return result;
}

bool append_observation_jsonl(std::ostream& output, const Observation& observation, std::string& error) {
    output << "{\"observation_version\":" << observation.version
           << ",\"observed_at\":\"" << observation.observed_at
           << "\",\"target_id\":\"" << observation.target_id
           << "\",\"source_type\":\"" << observation.source_type
           << "\",\"sensor\":{\"name\":\"" << observation.sensor_name
           << "\",\"version\":\"" << observation.sensor_version
           << "\"},\"configuration_profile\":\"" << observation.configuration_profile
           << "\",\"inventory\":" << observation.inventory_json << "}\n";
    if (!output) {
        error = "failed to append observation";
        return false;
    }
    return true;
}

}  // namespace mcpo
