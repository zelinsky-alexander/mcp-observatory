#include "observatory/target_manifest.hpp"

#include <charconv>
#include <limits>
#include <string_view>

namespace mcpo {
namespace {

class LineParser {
public:
    explicit LineParser(std::string_view text) : text_(text) {}

    [[nodiscard]] bool parse(TargetRecord& out, std::string& error) {
        skip_space();
        if (!consume('{')) {
            error = "record must be a JSON object";
            return false;
        }

        bool have_schema = false;
        bool have_id = false;
        bool have_source = false;
        bool have_kind = false;

        skip_space();
        if (consume('}')) {
            error = "record is missing required fields";
            return false;
        }

        while (true) {
            std::string key;
            if (!parse_plain_string(key, error)) {
                return false;
            }
            skip_space();
            if (!consume(':')) {
                error = "expected ':' after object key";
                return false;
            }
            skip_space();

            if (key == "schema_version") {
                if (have_schema) {
                    error = "duplicate schema_version";
                    return false;
                }
                have_schema = true;
                if (!parse_schema_version(out.schema_version, error)) {
                    return false;
                }
            } else if (key == "target_id") {
                if (have_id) {
                    error = "duplicate target_id";
                    return false;
                }
                have_id = true;
                if (!parse_plain_string(out.target_id, error) || out.target_id.empty()) {
                    if (error.empty()) {
                        error = "target_id must not be empty";
                    }
                    return false;
                }
            } else if (key == "source_type") {
                if (have_source) {
                    error = "duplicate source_type";
                    return false;
                }
                have_source = true;
                if (!parse_plain_string(out.source_type, error) || out.source_type.empty()) {
                    if (error.empty()) {
                        error = "source_type must not be empty";
                    }
                    return false;
                }
            } else if (key == "kind") {
                if (have_kind) {
                    error = "duplicate kind";
                    return false;
                }
                have_kind = true;
                std::string value;
                if (!parse_plain_string(value, error)) {
                    return false;
                }
                if (value == "local_package") {
                    out.kind = TargetKind::local_package;
                } else if (value == "remote_endpoint") {
                    out.kind = TargetKind::remote_endpoint;
                } else {
                    error = "kind must be local_package or remote_endpoint";
                    return false;
                }
            } else if (!skip_value(error, 0U)) {
                return false;
            }

            skip_space();
            if (consume('}')) {
                break;
            }
            if (!consume(',')) {
                error = "expected ',' or '}'";
                return false;
            }
            skip_space();
        }

        skip_space();
        if (position_ != text_.size()) {
            error = "unexpected data after JSON object";
            return false;
        }
        if (!have_schema || !have_id || !have_source || !have_kind) {
            error = "record is missing required fields";
            return false;
        }
        if (out.schema_version != 1U) {
            error = "unsupported schema_version";
            return false;
        }
        return true;
    }

private:
    static constexpr std::size_t max_skip_depth = 32U;

    void skip_space() noexcept {
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c != ' ' && c != '\t' && c != '\r') {
                break;
            }
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

    [[nodiscard]] bool parse_plain_string(std::string& out, std::string& error) {
        if (!consume('"')) {
            error = "expected JSON string";
            return false;
        }
        const std::size_t begin = position_;
        while (position_ < text_.size()) {
            const char c = text_[position_++];
            if (c == '"') {
                out.assign(text_.substr(begin, position_ - begin - 1U));
                return true;
            }
            if (c == '\\' || static_cast<unsigned char>(c) < 0x20U) {
                error = "escaped or control characters are not supported in manifest strings";
                return false;
            }
        }
        error = "unterminated JSON string";
        return false;
    }

    [[nodiscard]] bool parse_schema_version(std::uint32_t& value, std::string& error) {
        const std::size_t begin = position_;
        while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
            ++position_;
        }
        if (begin == position_) {
            error = "schema_version must be an unsigned integer";
            return false;
        }
        const auto token = text_.substr(begin, position_ - begin);
        const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
            error = "schema_version is out of range";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool skip_string(std::string& error) {
        if (!consume('"')) {
            return false;
        }
        while (position_ < text_.size()) {
            const char c = text_[position_++];
            if (c == '"') {
                return true;
            }
            if (c == '\\') {
                if (position_ >= text_.size()) {
                    error = "unterminated string escape";
                    return false;
                }
                ++position_;
            } else if (static_cast<unsigned char>(c) < 0x20U) {
                error = "control character in JSON string";
                return false;
            }
        }
        error = "unterminated JSON string";
        return false;
    }

    [[nodiscard]] bool skip_value(std::string& error, std::size_t depth) {
        if (depth > max_skip_depth) {
            error = "unknown value exceeds nesting limit";
            return false;
        }
        skip_space();
        if (position_ >= text_.size()) {
            error = "missing JSON value";
            return false;
        }
        if (text_[position_] == '"') {
            return skip_string(error);
        }
        if (text_[position_] == '{') {
            ++position_;
            skip_space();
            if (consume('}')) {
                return true;
            }
            while (true) {
                if (!skip_string(error)) {
                    if (error.empty()) {
                        error = "object key must be a string";
                    }
                    return false;
                }
                skip_space();
                if (!consume(':') || !skip_value(error, depth + 1U)) {
                    if (error.empty()) {
                        error = "malformed object value";
                    }
                    return false;
                }
                skip_space();
                if (consume('}')) {
                    return true;
                }
                if (!consume(',')) {
                    error = "expected ',' or '}' in object";
                    return false;
                }
                skip_space();
            }
        }
        if (text_[position_] == '[') {
            ++position_;
            skip_space();
            if (consume(']')) {
                return true;
            }
            while (true) {
                if (!skip_value(error, depth + 1U)) {
                    return false;
                }
                skip_space();
                if (consume(']')) {
                    return true;
                }
                if (!consume(',')) {
                    error = "expected ',' or ']' in array";
                    return false;
                }
                skip_space();
            }
        }

        const std::size_t begin = position_;
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' || c == '\r') {
                break;
            }
            ++position_;
        }
        if (begin == position_) {
            error = "invalid JSON value";
            return false;
        }
        return true;
    }

    std::string_view text_;
    std::size_t position_{};
};

}  // namespace

ManifestResult read_target_manifest(std::istream& input, ReadLimits limits) {
    ManifestResult result;
    std::string line;
    std::size_t line_number = 0U;

    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty()) {
            continue;
        }
        if (line.size() > limits.max_line_bytes) {
            result.error = ManifestError{line_number, "line exceeds max_line_bytes"};
            return result;
        }
        if (result.summary.records >= limits.max_records) {
            result.error = ManifestError{line_number, "manifest exceeds max_records"};
            return result;
        }

        TargetRecord record;
        std::string error;
        LineParser parser(line);
        if (!parser.parse(record, error)) {
            result.error = ManifestError{line_number, std::move(error)};
            return result;
        }

        ++result.summary.records;
        if (record.kind == TargetKind::local_package) {
            ++result.summary.local_packages;
        } else {
            ++result.summary.remote_endpoints;
        }
    }

    if (input.bad()) {
        result.error = ManifestError{line_number, "I/O failure while reading manifest"};
    }
    return result;
}

std::string_view to_string(TargetKind kind) noexcept {
    return kind == TargetKind::local_package ? "local_package" : "remote_endpoint";
}

}  // namespace mcpo
