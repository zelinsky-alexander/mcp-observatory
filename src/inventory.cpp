#include "observatory/inventory.hpp"

#include <algorithm>
#include <charconv>
#include <iterator>
#include <string_view>

namespace mcpo {
namespace {

class InventoryParser {
public:
    InventoryParser(std::string_view text, InventoryLimits limits)
        : text_(text), limits_(limits) {}

    [[nodiscard]] bool parse(Inventory& out, std::string& error) {
        skip_space();
        if (!consume('{')) {
            return fail(error, "inventory must be a JSON object");
        }

        bool have_version = false;
        bool have_server = false;
        bool have_tools = false;
        skip_space();
        if (consume('}')) {
            return fail(error, "inventory is missing required fields");
        }

        while (true) {
            std::string key;
            if (!parse_plain_string(key, error)) {
                return false;
            }
            skip_space();
            if (!consume(':')) {
                return fail(error, "expected ':' after inventory member");
            }
            skip_space();

            if (key == "inventory_version") {
                if (have_version) {
                    return fail(error, "duplicate inventory_version");
                }
                have_version = true;
                if (!parse_uint32(out.version, error)) {
                    return false;
                }
            } else if (key == "server") {
                if (have_server) {
                    return fail(error, "duplicate server");
                }
                have_server = true;
                if (!parse_server(out.downstream_executable, error)) {
                    return false;
                }
            } else if (key == "tools") {
                if (have_tools) {
                    return fail(error, "duplicate tools");
                }
                have_tools = true;
                if (!parse_tools(out.tools, error)) {
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
                return fail(error, "expected ',' or '}' in inventory");
            }
            skip_space();
        }

        skip_space();
        if (position_ != text_.size()) {
            return fail(error, "unexpected data after inventory");
        }
        if (!have_version || !have_server || !have_tools) {
            return fail(error, "inventory is missing required fields");
        }
        if (out.version != 1U) {
            return fail(error, "unsupported inventory_version");
        }
        if (!std::is_sorted(out.tools.begin(), out.tools.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.name < rhs.name;
            })) {
            return fail(error, "tools must be sorted by name");
        }
        for (std::size_t i = 1U; i < out.tools.size(); ++i) {
            if (out.tools[i - 1U].name == out.tools[i].name) {
                return fail(error, "duplicate tool name");
            }
        }
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
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
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
            return fail(error, "expected JSON string");
        }
        const std::size_t begin = position_;
        while (position_ < text_.size()) {
            const char c = text_[position_++];
            if (c == '"') {
                out.assign(text_.substr(begin, position_ - begin - 1U));
                return true;
            }
            if (c == '\\' || static_cast<unsigned char>(c) < 0x20U) {
                return fail(error, "escaped or control characters are not supported in identity strings");
            }
        }
        return fail(error, "unterminated JSON string");
    }

    [[nodiscard]] bool parse_uint32(std::uint32_t& out, std::string& error) {
        const std::size_t begin = position_;
        while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
            ++position_;
        }
        if (begin == position_) {
            return fail(error, "inventory_version must be an unsigned integer");
        }
        const std::string_view token = text_.substr(begin, position_ - begin);
        const auto result = std::from_chars(token.data(), token.data() + token.size(), out);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
            return fail(error, "inventory_version is out of range");
        }
        return true;
    }

    [[nodiscard]] bool parse_server(std::string& executable, std::string& error) {
        if (!consume('{')) {
            return fail(error, "server must be an object");
        }
        bool have_executable = false;
        skip_space();
        if (consume('}')) {
            return fail(error, "server is missing downstream_executable");
        }
        while (true) {
            std::string key;
            if (!parse_plain_string(key, error)) {
                return false;
            }
            skip_space();
            if (!consume(':')) {
                return fail(error, "expected ':' in server object");
            }
            skip_space();
            if (key == "downstream_executable") {
                if (have_executable) {
                    return fail(error, "duplicate downstream_executable");
                }
                have_executable = true;
                if (!parse_plain_string(executable, error) || executable.empty()) {
                    if (error.empty()) {
                        error = "downstream_executable must not be empty";
                    }
                    return false;
                }
            } else if (!skip_value(error, 1U)) {
                return false;
            }
            skip_space();
            if (consume('}')) {
                break;
            }
            if (!consume(',')) {
                return fail(error, "expected ',' or '}' in server object");
            }
            skip_space();
        }
        if (!have_executable) {
            return fail(error, "server is missing downstream_executable");
        }
        return true;
    }

    [[nodiscard]] bool parse_tools(std::vector<InventoryTool>& tools, std::string& error) {
        if (!consume('[')) {
            return fail(error, "tools must be an array");
        }
        skip_space();
        if (consume(']')) {
            return true;
        }
        while (true) {
            if (tools.size() >= limits_.max_tools) {
                return fail(error, "tool count exceeds configured limit");
            }
            InventoryTool tool;
            if (!parse_tool(tool, error)) {
                return false;
            }
            tools.push_back(std::move(tool));
            skip_space();
            if (consume(']')) {
                return true;
            }
            if (!consume(',')) {
                return fail(error, "expected ',' or ']' in tools array");
            }
            skip_space();
        }
    }

    [[nodiscard]] bool parse_tool(InventoryTool& tool, std::string& error) {
        const std::size_t object_begin = position_;
        if (!consume('{')) {
            return fail(error, "tool definition must be an object");
        }
        bool have_name = false;
        skip_space();
        if (consume('}')) {
            return fail(error, "tool definition is missing name");
        }
        while (true) {
            std::string key;
            if (!parse_plain_string(key, error)) {
                return false;
            }
            skip_space();
            if (!consume(':')) {
                return fail(error, "expected ':' in tool definition");
            }
            skip_space();
            if (key == "name") {
                if (have_name) {
                    return fail(error, "duplicate tool name member");
                }
                have_name = true;
                if (!parse_plain_string(tool.name, error) || tool.name.empty()) {
                    if (error.empty()) {
                        error = "tool name must not be empty";
                    }
                    return false;
                }
                if (tool.name.size() > limits_.max_tool_name_bytes) {
                    return fail(error, "tool name exceeds configured limit");
                }
            } else if (!skip_value(error, 2U)) {
                return false;
            }
            skip_space();
            if (consume('}')) {
                break;
            }
            if (!consume(',')) {
                return fail(error, "expected ',' or '}' in tool definition");
            }
            skip_space();
        }
        if (!have_name) {
            return fail(error, "tool definition is missing name");
        }
        tool.canonical_definition.assign(text_.substr(object_begin, position_ - object_begin));
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
                    return fail(error, "unterminated JSON escape");
                }
                ++position_;
            } else if (static_cast<unsigned char>(c) < 0x20U) {
                return fail(error, "control character in JSON string");
            }
        }
        return fail(error, "unterminated JSON string");
    }

    [[nodiscard]] bool skip_value(std::string& error, std::size_t depth) {
        if (depth > limits_.max_nesting_depth) {
            return fail(error, "JSON value exceeds configured nesting limit");
        }
        skip_space();
        if (position_ >= text_.size()) {
            return fail(error, "missing JSON value");
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
                    return fail(error, error.empty() ? "object key must be a string" : error);
                }
                skip_space();
                if (!consume(':')) {
                    return fail(error, "expected ':' in object");
                }
                if (!skip_value(error, depth + 1U)) {
                    return false;
                }
                skip_space();
                if (consume('}')) {
                    return true;
                }
                if (!consume(',')) {
                    return fail(error, "expected ',' or '}' in object");
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
                    return fail(error, "expected ',' or ']' in array");
                }
                skip_space();
            }
        }

        const std::size_t begin = position_;
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                break;
            }
            ++position_;
        }
        if (begin == position_) {
            return fail(error, "invalid JSON value");
        }
        return true;
    }

    std::string_view text_;
    InventoryLimits limits_;
    std::size_t position_{};
};

}  // namespace

InventoryResult read_inventory(std::istream& input, InventoryLimits limits) {
    InventoryResult result;
    std::string text;
    text.reserve(std::min<std::size_t>(limits.max_bytes, 64U * 1024U));

    char buffer[4096];
    while (input.read(buffer, static_cast<std::streamsize>(sizeof(buffer))) || input.gcount() > 0) {
        const auto count = static_cast<std::size_t>(input.gcount());
        if (text.size() > limits.max_bytes - std::min(limits.max_bytes, count)) {
            result.error = InventoryError{"inventory exceeds configured byte limit"};
            return result;
        }
        text.append(buffer, count);
        if (text.size() > limits.max_bytes) {
            result.error = InventoryError{"inventory exceeds configured byte limit"};
            return result;
        }
    }
    if (input.bad()) {
        result.error = InventoryError{"I/O failure while reading inventory"};
        return result;
    }

    std::string error;
    InventoryParser parser(text, limits);
    if (!parser.parse(result.inventory, error)) {
        result.error = InventoryError{std::move(error)};
    }
    return result;
}

InventoryDiff compare_inventories(const Inventory& before, const Inventory& after) {
    InventoryDiff diff;
    diff.executable_changed = before.downstream_executable != after.downstream_executable;

    std::size_t old_index = 0U;
    std::size_t new_index = 0U;
    while (old_index < before.tools.size() || new_index < after.tools.size()) {
        if (old_index == before.tools.size()) {
            diff.added.push_back(after.tools[new_index++].name);
            continue;
        }
        if (new_index == after.tools.size()) {
            diff.removed.push_back(before.tools[old_index++].name);
            continue;
        }

        const auto& old_tool = before.tools[old_index];
        const auto& new_tool = after.tools[new_index];
        if (old_tool.name < new_tool.name) {
            diff.removed.push_back(old_tool.name);
            ++old_index;
        } else if (new_tool.name < old_tool.name) {
            diff.added.push_back(new_tool.name);
            ++new_index;
        } else {
            if (old_tool.canonical_definition != new_tool.canonical_definition) {
                diff.modified.push_back(ModifiedTool{old_tool.name});
            }
            ++old_index;
            ++new_index;
        }
    }
    return diff;
}

}  // namespace mcpo
