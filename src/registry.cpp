#include "observatory/registry.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <linux/fs.h>
#include <map>
#include <optional>
#include <poll.h>
#include <set>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>

namespace mcpo {
namespace {

#ifndef MCPO_VERSION
#define MCPO_VERSION "unknown"
#endif
#ifndef MCPO_GIT_COMMIT
#define MCPO_GIT_COMMIT "unknown"
#endif

constexpr std::size_t maximum_json_depth = 64U;
constexpr std::size_t maximum_diagnostic_bytes = 16U * 1024U;
constexpr std::size_t maximum_progress_value_bytes = 160U;
constexpr std::size_t maximum_cursor_prefix_bytes = 32U;
constexpr std::string_view registry_name = "official-mcp";

bool validate_bundle_impl(
    const std::filesystem::path& bundle,
    bool require_success,
    std::string& message);

std::string progress_value(
    std::string_view value,
    std::size_t maximum = maximum_progress_value_bytes) {
    std::string result;
    const std::size_t included = std::min(value.size(), maximum);
    result.reserve(included + 32U);
    for (std::size_t index = 0U; index < included; ++index) {
        const unsigned char c = static_cast<unsigned char>(value[index]);
        if (c >= 0x20U && c <= 0x7eU && c != '\\') {
            result.push_back(static_cast<char>(c));
        } else {
            constexpr char hex[] = "0123456789abcdef";
            result += "\\x";
            result.push_back(hex[c >> 4U]);
            result.push_back(hex[c & 0x0fU]);
        }
    }
    if (value.size() > included)
        result += "...(length=" + std::to_string(value.size()) + ")";
    return result;
}

std::string cursor_progress(const std::optional<std::string>& cursor) {
    if (!cursor) return "cursor=none";
    return "cursor_prefix=" +
        progress_value(*cursor, maximum_cursor_prefix_bytes) +
        " cursor_length=" + std::to_string(cursor->size());
}

std::string seconds_text(std::chrono::milliseconds duration) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1)
        << static_cast<double>(duration.count()) / 1000.0 << 's';
    return out.str();
}

void registry_progress(bool enabled, std::string_view text) {
    if (!enabled) return;
    std::cerr << "[registry] " << text << '\n';
    std::cerr.flush();
}

struct Json {
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;
    std::variant<std::nullptr_t, bool, std::string, Array, Object> value;
    bool number{};
};

void append_utf8(std::string& out, unsigned value) {
    if (value <= 0x7fU) {
        out.push_back(static_cast<char>(value));
    } else if (value <= 0x7ffU) {
        out.push_back(static_cast<char>(0xc0U | (value >> 6U)));
        out.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    } else if (value <= 0xffffU) {
        out.push_back(static_cast<char>(0xe0U | (value >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    } else {
        out.push_back(static_cast<char>(0xf0U | (value >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    }
}

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    bool parse(Json& out, std::string& error) {
        skip_space();
        if (!parse_value(out, 0U, error)) return false;
        skip_space();
        if (position_ != input_.size()) return fail(error, "unexpected data after JSON value");
        return true;
    }

private:
    static bool fail(std::string& error, std::string message) {
        error = std::move(message);
        return false;
    }

    void skip_space() {
        while (position_ < input_.size() &&
               (input_[position_] == ' ' || input_[position_] == '\t' ||
                input_[position_] == '\r' || input_[position_] == '\n')) {
            ++position_;
        }
    }

    bool consume(std::string_view token) {
        if (input_.substr(position_, token.size()) != token) return false;
        position_ += token.size();
        return true;
    }

    bool hex4(unsigned& value) {
        if (position_ + 4U > input_.size()) return false;
        value = 0U;
        for (unsigned i = 0U; i < 4U; ++i) {
            const char c = input_[position_++];
            unsigned digit{};
            if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = 10U + static_cast<unsigned>(c - 'a');
            else if (c >= 'A' && c <= 'F') digit = 10U + static_cast<unsigned>(c - 'A');
            else return false;
            value = value * 16U + digit;
        }
        return true;
    }

    bool parse_string(std::string& out, std::string& error) {
        if (position_ >= input_.size() || input_[position_++] != '"')
            return fail(error, "expected JSON string");
        while (position_ < input_.size()) {
            const unsigned char c = static_cast<unsigned char>(input_[position_++]);
            if (c == '"') return true;
            if (c < 0x20U) return fail(error, "control character in JSON string");
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                continue;
            }
            if (position_ >= input_.size()) return fail(error, "unterminated JSON escape");
            const char escaped = input_[position_++];
            switch (escaped) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned first{};
                    if (!hex4(first)) return fail(error, "invalid JSON Unicode escape");
                    if (first >= 0xd800U && first <= 0xdbffU) {
                        if (!consume("\\u")) return fail(error, "missing low surrogate");
                        unsigned second{};
                        if (!hex4(second) || second < 0xdc00U || second > 0xdfffU)
                            return fail(error, "invalid low surrogate");
                        first = 0x10000U + ((first - 0xd800U) << 10U) + (second - 0xdc00U);
                    } else if (first >= 0xdc00U && first <= 0xdfffU) {
                        return fail(error, "unexpected low surrogate");
                    }
                    append_utf8(out, first);
                    break;
                }
                default: return fail(error, "invalid JSON escape");
            }
        }
        return fail(error, "unterminated JSON string");
    }

    bool parse_number(Json& out, std::string& error) {
        const std::size_t begin = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        if (position_ >= input_.size()) return fail(error, "invalid JSON number");
        if (input_[position_] == '0') {
            ++position_;
        } else {
            if (input_[position_] < '1' || input_[position_] > '9')
                return fail(error, "invalid JSON number");
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') ++position_;
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            const std::size_t digits = position_;
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') ++position_;
            if (digits == position_) return fail(error, "invalid JSON fraction");
        }
        if (position_ < input_.size() &&
            (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            const std::size_t digits = position_;
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') ++position_;
            if (digits == position_) return fail(error, "invalid JSON exponent");
        }
        out.value = std::string(input_.substr(begin, position_ - begin));
        out.number = true;
        return true;
    }

    bool parse_value(Json& out, std::size_t depth, std::string& error) {
        if (depth > maximum_json_depth) return fail(error, "JSON nesting limit exceeded");
        skip_space();
        if (position_ >= input_.size()) return fail(error, "missing JSON value");
        const char c = input_[position_];
        if (c == '"') {
            std::string value;
            if (!parse_string(value, error)) return false;
            out.value = std::move(value);
            return true;
        }
        if (c == '{') {
            ++position_;
            Json::Object object;
            skip_space();
            if (position_ < input_.size() && input_[position_] == '}') {
                ++position_;
                out.value = std::move(object);
                return true;
            }
            while (true) {
                std::string key;
                if (!parse_string(key, error)) return false;
                skip_space();
                if (position_ >= input_.size() || input_[position_++] != ':')
                    return fail(error, "expected ':' after object key");
                Json child;
                if (!parse_value(child, depth + 1U, error)) return false;
                if (!object.emplace(std::move(key), std::move(child)).second)
                    return fail(error, "duplicate JSON object member");
                skip_space();
                if (position_ < input_.size() && input_[position_] == '}') {
                    ++position_;
                    out.value = std::move(object);
                    return true;
                }
                if (position_ >= input_.size() || input_[position_++] != ',')
                    return fail(error, "expected ',' or '}'");
                skip_space();
            }
        }
        if (c == '[') {
            ++position_;
            Json::Array array;
            skip_space();
            if (position_ < input_.size() && input_[position_] == ']') {
                ++position_;
                out.value = std::move(array);
                return true;
            }
            while (true) {
                Json child;
                if (!parse_value(child, depth + 1U, error)) return false;
                array.push_back(std::move(child));
                skip_space();
                if (position_ < input_.size() && input_[position_] == ']') {
                    ++position_;
                    out.value = std::move(array);
                    return true;
                }
                if (position_ >= input_.size() || input_[position_++] != ',')
                    return fail(error, "expected ',' or ']'");
                skip_space();
            }
        }
        if (consume("true")) { out.value = true; return true; }
        if (consume("false")) { out.value = false; return true; }
        if (consume("null")) { out.value = nullptr; return true; }
        return parse_number(out, error);
    }

    std::string_view input_;
    std::size_t position_{};
};

void append_json_string(std::string& out, std::string_view value) {
    static constexpr char hex[] = "0123456789abcdef";
    out.push_back('"');
    for (const unsigned char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20U) {
                    out += "\\u00";
                    out.push_back(hex[c >> 4U]);
                    out.push_back(hex[c & 0x0fU]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void canonical_json(const Json& value, std::string& out) {
    if (std::holds_alternative<std::nullptr_t>(value.value)) out += "null";
    else if (const auto* boolean = std::get_if<bool>(&value.value))
        out += *boolean ? "true" : "false";
    else if (const auto* string = std::get_if<std::string>(&value.value)) {
        if (value.number) out += *string;
        else append_json_string(out, *string);
    } else if (const auto* array = std::get_if<Json::Array>(&value.value)) {
        out.push_back('[');
        for (std::size_t i = 0U; i < array->size(); ++i) {
            if (i != 0U) out.push_back(',');
            canonical_json((*array)[i], out);
        }
        out.push_back(']');
    } else {
        out.push_back('{');
        const auto& object = std::get<Json::Object>(value.value);
        std::size_t index = 0U;
        for (const auto& [key, child] : object) {
            if (index++ != 0U) out.push_back(',');
            append_json_string(out, key);
            out.push_back(':');
            canonical_json(child, out);
        }
        out.push_back('}');
    }
}

const Json::Object* object_value(const Json& value) {
    return std::get_if<Json::Object>(&value.value);
}

const Json::Array* array_member(const Json::Object& object, std::string_view key) {
    const auto it = object.find(std::string(key));
    return it == object.end() ? nullptr : std::get_if<Json::Array>(&it->second.value);
}

const Json::Object* object_member(const Json::Object& object, std::string_view key) {
    const auto it = object.find(std::string(key));
    return it == object.end() ? nullptr : std::get_if<Json::Object>(&it->second.value);
}

const std::string* string_member(const Json::Object& object, std::string_view key) {
    const auto it = object.find(std::string(key));
    if (it == object.end() || it->second.number) return nullptr;
    return std::get_if<std::string>(&it->second.value);
}

std::string lower_ascii(std::string_view input) {
    std::string result(input);
    for (char& c : result) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return result;
}

std::string percent_encode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') {
            result.push_back(static_cast<char>(c));
        } else {
            result.push_back('%');
            result.push_back(hex[c >> 4U]);
            result.push_back(hex[c & 0x0fU]);
        }
    }
    return result;
}

std::string utc_now() {
    const std::time_t now = std::time(nullptr);
    std::tm value{};
    gmtime_r(&now, &value);
    std::array<char, 21U> buffer{};
    std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &value);
    return buffer.data();
}

std::string random_run_id() {
    std::ifstream source("/dev/urandom", std::ios::binary);
    std::array<unsigned char, 16U> bytes{};
    source.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!source) {
        const auto value = std::chrono::steady_clock::now().time_since_epoch().count();
        for (std::size_t i = 0U; i < bytes.size(); ++i)
            bytes[i] = static_cast<unsigned char>((value >> ((i % 8U) * 8U)) & 0xff);
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const unsigned char byte : bytes) out << std::setw(2) << static_cast<unsigned>(byte);
    return out.str();
}

bool write_file(const std::filesystem::path& path, std::string_view bytes, std::string& error) {
    const int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (descriptor < 0) { error = "cannot create " + path.string(); return false; }
    std::size_t written = 0U;
    while (written < bytes.size()) {
        const ssize_t count = write(descriptor, bytes.data() + written, bytes.size() - written);
        if (count <= 0) {
            close(descriptor);
            error = "cannot write " + path.string();
            return false;
        }
        written += static_cast<std::size_t>(count);
    }
    if (fsync(descriptor) != 0 || close(descriptor) != 0) {
        error = "cannot flush " + path.string();
        return false;
    }
    return true;
}

bool sync_directory(const std::filesystem::path& path, std::string& error) {
    const int descriptor = open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (descriptor < 0) return (error = "cannot open bundle directory for flush", false);
    if (fsync(descriptor) != 0 || close(descriptor) != 0)
        return (error = "cannot flush bundle directory", false);
    return true;
}

bool read_file(
    const std::filesystem::path& path,
    std::size_t maximum,
    std::string& bytes,
    std::string& error) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > maximum) { error = "invalid or oversized file: " + path.string(); return false; }
    std::ifstream input(path, std::ios::binary);
    if (!input) { error = "cannot open " + path.string(); return false; }
    bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (input.bad()) { error = "cannot read " + path.string(); return false; }
    return true;
}

bool run_process(
    const std::vector<std::string>& arguments,
    unsigned timeout_seconds,
    std::size_t maximum_output,
    std::string& output,
    int& exit_status,
    std::string& error,
    const HttpHeartbeat& heartbeat = {},
    std::chrono::milliseconds heartbeat_interval = std::chrono::seconds(5)) {
    int pipe_fds[2]{};
    if (pipe(pipe_fds) != 0) { error = "cannot create process pipe"; return false; }
    const pid_t child = fork();
    if (child < 0) {
        close(pipe_fds[0]); close(pipe_fds[1]);
        error = "cannot create child process";
        return false;
    }
    if (child == 0) {
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[0]); close(pipe_fds[1]);
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1U);
        for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        execv(argv[0], argv.data());
        _exit(127);
    }
    close(pipe_fds[1]);
    fcntl(pipe_fds[0], F_SETFL, fcntl(pipe_fds[0], F_GETFL) | O_NONBLOCK);
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(timeout_seconds);
    auto next_heartbeat = started + heartbeat_interval;
    bool done = false;
    int status{};
    while (!done) {
        std::array<char, 4096U> buffer{};
        const ssize_t count = read(pipe_fds[0], buffer.data(), buffer.size());
        if (count > 0) {
            if (output.size() + static_cast<std::size_t>(count) > maximum_output) {
                kill(child, SIGKILL);
                waitpid(child, &status, 0);
                close(pipe_fds[0]);
                error = "child process output limit exceeded";
                return false;
            }
            output.append(buffer.data(), static_cast<std::size_t>(count));
        }
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) done = true;
        else if (waited < 0) { close(pipe_fds[0]); error = "cannot wait for child process"; return false; }
        else if (std::chrono::steady_clock::now() >= deadline) {
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            close(pipe_fds[0]);
            error = "child process timed out";
            return false;
        } else {
            const auto now = std::chrono::steady_clock::now();
            if (heartbeat && heartbeat_interval >= std::chrono::seconds(1) &&
                now >= next_heartbeat) {
                heartbeat(std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - started));
                do {
                    next_heartbeat += heartbeat_interval;
                } while (next_heartbeat <= now);
            }
            pollfd descriptor{pipe_fds[0], POLLIN, 0};
            poll(&descriptor, 1, 10);
        }
    }
    while (true) {
        std::array<char, 4096U> buffer{};
        const ssize_t count = read(pipe_fds[0], buffer.data(), buffer.size());
        if (count <= 0) break;
        if (output.size() + static_cast<std::size_t>(count) > maximum_output) {
            close(pipe_fds[0]); error = "child process output limit exceeded"; return false;
        }
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    close(pipe_fds[0]);
    exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    return true;
}

bool sha256_file(const std::filesystem::path& path, std::string& digest, std::string& error) {
    std::string output;
    int status{};
    if (!run_process(
            {"/usr/bin/openssl", "dgst", "-sha256", "-r", path.string()},
            30U, 4096U, output, status, error)) return false;
    if (status != 0 || output.size() < 64U) {
        error = "OpenSSL SHA-256 failed for " + path.string();
        return false;
    }
    digest = output.substr(0U, 64U);
    if (!std::all_of(digest.begin(), digest.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) {
        error = "invalid OpenSSL SHA-256 output";
        return false;
    }
    return true;
}

bool sha256_bytes(
    const std::filesystem::path&,
    std::string_view bytes,
    std::string& digest,
    std::string& error) {
    std::array<char, 64U> path_template{};
    std::snprintf(path_template.data(), path_template.size(), "/tmp/mcpo-hash-%ld-XXXXXX",
                  static_cast<long>(getpid()));
    const int descriptor = mkstemp(path_template.data());
    if (descriptor < 0) return (error = "cannot create SHA-256 temporary input", false);
    std::size_t written = 0U;
    while (written < bytes.size()) {
        const ssize_t count = write(
            descriptor, bytes.data() + written, bytes.size() - written);
        if (count <= 0) {
            close(descriptor);
            unlink(path_template.data());
            return (error = "cannot write SHA-256 temporary input", false);
        }
        written += static_cast<std::size_t>(count);
    }
    if (fsync(descriptor) != 0 || close(descriptor) != 0) {
        unlink(path_template.data());
        return (error = "cannot flush SHA-256 temporary input", false);
    }
    const bool success = sha256_file(path_template.data(), digest, error);
    unlink(path_template.data());
    return success;
}

std::optional<std::string> header_value(std::string_view headers, std::string_view wanted) {
    std::optional<std::string> result;
    std::size_t position = 0U;
    while (position < headers.size()) {
        const std::size_t end = headers.find('\n', position);
        std::string_view line = headers.substr(
            position, (end == std::string_view::npos ? headers.size() : end) - position);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1U);
        const std::size_t colon = line.find(':');
        if (colon != std::string_view::npos &&
            lower_ascii(line.substr(0U, colon)) == lower_ascii(wanted)) {
            std::size_t start = colon + 1U;
            while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
            result = std::string(line.substr(start));
        }
        if (end == std::string_view::npos) break;
        position = end + 1U;
    }
    return result;
}

bool curl_transport(
    const std::string& url,
    unsigned timeout_seconds,
    std::size_t maximum_bytes,
    HttpResponse& response,
    std::string& error,
    const HttpHeartbeat& heartbeat,
    std::chrono::milliseconds heartbeat_interval) {
    std::array<char, 64U> body_template{};
    std::array<char, 64U> header_template{};
    std::snprintf(body_template.data(), body_template.size(), "/tmp/mcpo-body-%ld-XXXXXX",
                  static_cast<long>(getpid()));
    std::snprintf(header_template.data(), header_template.size(), "/tmp/mcpo-head-%ld-XXXXXX",
                  static_cast<long>(getpid()));
    const int body_fd = mkstemp(body_template.data());
    const int header_fd = mkstemp(header_template.data());
    if (body_fd < 0 || header_fd < 0) {
        if (body_fd >= 0) { close(body_fd); unlink(body_template.data()); }
        if (header_fd >= 0) { close(header_fd); unlink(header_template.data()); }
        error = "cannot create bounded HTTP temporary files";
        return false;
    }
    close(body_fd); close(header_fd);
    std::string output;
    int status{};
    const std::string seconds = std::to_string(timeout_seconds);
    const std::string bytes = std::to_string(maximum_bytes);
    const bool ran = run_process(
        {"/usr/bin/curl", "--silent", "--show-error", "--request", "GET",
         "--proto", "=http,https", "--max-time", seconds, "--connect-timeout", seconds,
         "--max-filesize", bytes, "--output", body_template.data(), "--dump-header",
         header_template.data(), "--write-out", "%{http_code}\\n%{content_type}\\n%{url_effective}",
         url},
        timeout_seconds + 2U, maximum_diagnostic_bytes, output, status, error,
        heartbeat, heartbeat_interval);
    std::string body;
    std::string headers;
    if (ran) {
        std::string read_error;
        if (!read_file(body_template.data(), maximum_bytes, body, read_error) ||
            !read_file(header_template.data(), 64U * 1024U, headers, read_error)) {
            error = read_error;
        }
    }
    unlink(body_template.data()); unlink(header_template.data());
    if (!ran) return false;
    if (!error.empty()) return false;
    if (status != 0) {
        error = output.empty() ? "curl request failed" : output.substr(0U, maximum_diagnostic_bytes);
        return false;
    }
    const std::size_t first = output.find('\n');
    const std::size_t second = first == std::string::npos ? first : output.find('\n', first + 1U);
    if (first == std::string::npos || second == std::string::npos)
        return (error = "invalid curl metadata output", false);
    unsigned http_status{};
    const auto parsed = std::from_chars(output.data(), output.data() + first, http_status);
    if (parsed.ec != std::errc{}) return (error = "invalid HTTP status", false);
    response.status = http_status;
    response.content_type = output.substr(first + 1U, second - first - 1U);
    response.effective_url = output.substr(second + 1U);
    response.location = header_value(headers, "location");
    response.body = std::move(body);
    return true;
}

struct Artifact {
    std::string path;
    std::string sha256;
    std::uintmax_t size{};
};

bool artifact_for(
    const std::filesystem::path& root,
    std::string relative,
    Artifact& artifact,
    std::string& error) {
    const auto path = root / relative;
    std::error_code ec;
    artifact.size = std::filesystem::file_size(path, ec);
    if (ec || !sha256_file(path, artifact.sha256, error)) return false;
    artifact.path = std::move(relative);
    return true;
}

std::string artifact_json(const Artifact& artifact) {
    std::string out = "{\"path\":";
    append_json_string(out, artifact.path);
    out += ",\"sha256\":";
    append_json_string(out, artifact.sha256);
    out += ",\"size\":" + std::to_string(artifact.size) + "}";
    return out;
}

bool resolve_redirect(
    const RegistryUrl& origin,
    const std::string& current,
    std::string_view location,
    std::string& next,
    std::string& error) {
    const auto parse_request = [&](std::string_view value, RegistryUrl& parsed) {
        const std::size_t query = value.find('?');
        return parse_registry_url(value.substr(0U, query), parsed, error);
    };
    RegistryUrl target;
    if (location.starts_with("http://") || location.starts_with("https://")) {
        if (location.find('#') != std::string_view::npos ||
            !parse_request(location, target)) return false;
        if (!registry_same_origin(origin, target)) {
            error = "cross-origin redirect rejected";
            return false;
        }
        next = std::string(location);
        return true;
    }
    RegistryUrl current_url;
    if (!parse_request(current, current_url)) return false;
    std::string path;
    if (!location.empty() && location.front() == '/') {
        path = std::string(location);
    } else {
        const std::size_t slash = current_url.base_path.rfind('/');
        path = current_url.base_path.substr(0U, slash == std::string::npos ? 0U : slash + 1U);
        path += location;
    }
    const std::string authority = origin.host.front() == '[' ? origin.host : origin.host;
    next = origin.scheme + "://" + authority;
    const unsigned default_port = origin.scheme == "https" ? 443U : 80U;
    if (origin.port != default_port) next += ":" + std::to_string(origin.port);
    if (path.empty() || path.front() != '/') next.push_back('/');
    next += path;
    RegistryUrl normalized;
    if (!parse_registry_url(next, normalized, error)) return false;
    next = normalized.normalized;
    return true;
}

std::string stable_observed_at(const Json::Object& wrapper) {
    const Json::Object* metadata = object_member(wrapper, "_meta");
    if (metadata != nullptr) {
        const Json::Object* official =
            object_member(*metadata, "io.modelcontextprotocol.registry/official");
        if (official != nullptr) {
            if (const std::string* updated = string_member(*official, "updatedAt");
                updated != nullptr && valid_utc_timestamp(*updated)) return *updated;
            if (const std::string* published = string_member(*official, "publishedAt");
                published != nullptr && valid_utc_timestamp(*published)) return *published;
        }
    }
    return "1970-01-01T00:00:00Z";
}

bool valid_server_name(std::string_view value) {
    if (value.size() < 3U || value.size() > 200U) return false;
    const std::size_t slash = value.find('/');
    if (slash == std::string_view::npos || slash == 0U || slash + 1U == value.size() ||
        value.find('/', slash + 1U) != std::string_view::npos) return false;
    for (std::size_t i = 0U; i < value.size(); ++i) {
        if (i == slash) continue;
        const char c = value[i];
        const bool common = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-';
        if (!common && !(i > slash && c == '_')) return false;
    }
    return true;
}

bool valid_server_version(std::string_view value) {
    if (value.empty() || value.size() > 255U) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return c >= 0x20U && c != 0x7fU;
    });
}

bool build_record(
    const Json& entry,
    const std::filesystem::path& temporary,
    std::string& identity,
    std::string& canonical,
    std::string& content_without_observation,
    std::string& error) {
    const Json::Object* wrapper = object_value(entry);
    if (wrapper == nullptr) return (error = "server list entry must be an object", false);
    const Json::Object* server = object_member(*wrapper, "server");
    if (server == nullptr) return (error = "server list entry is missing server object", false);
    const std::string* name = string_member(*server, "name");
    const std::string* version = string_member(*server, "version");
    if (name == nullptr || !valid_server_name(*name) ||
        version == nullptr || !valid_server_version(*version))
        return (error = "server record has invalid name or exact version", false);
    identity = *name + "\n" + *version;

    Json::Object logical;
    logical.emplace("record_version", Json{std::string("1"), true});
    logical.emplace("registry", Json{std::string(registry_name), false});
    logical.emplace("server_identifier", Json{*name, false});
    logical.emplace("server_version", Json{*version, false});
    for (const std::string_view key : {"description", "repository", "packages", "remotes"}) {
        const auto found = server->find(std::string(key));
        if (found != server->end()) logical.emplace(std::string(key), found->second);
    }
    logical.emplace("original", entry);
    Json logical_json{logical, false};
    canonical_json(logical_json, content_without_observation);
    std::string digest;
    if (!sha256_bytes(temporary, content_without_observation, digest, error)) return false;
    logical.emplace("observed_at", Json{stable_observed_at(*wrapper), false});
    logical.emplace("canonical_sha256", Json{digest, false});
    Json complete{std::move(logical), false};
    canonical_json(complete, canonical);
    canonical.push_back('\n');
    return true;
}

bool parse_page(
    std::string_view body,
    const std::filesystem::path& temporary,
    std::vector<std::pair<std::string, std::string>>& records,
    std::optional<std::string>& next_cursor,
    std::string& error,
    std::optional<std::string>* declared_cursor = nullptr) {
    Json root;
    JsonParser parser(body);
    if (!parser.parse(root, error)) return false;
    const Json::Object* object = object_value(root);
    if (object == nullptr) return (error = "registry response must be a JSON object", false);
    const Json::Array* servers = array_member(*object, "servers");
    if (servers == nullptr) return (error = "registry response is missing servers array", false);
    for (const Json& entry : *servers) {
        std::string identity;
        std::string record;
        std::string logical;
        if (!build_record(entry, temporary, identity, record, logical, error)) return false;
        records.emplace_back(std::move(identity), std::move(record));
    }
    next_cursor.reset();
    if (const Json::Object* metadata = object_member(*object, "metadata"); metadata != nullptr) {
        const auto next = metadata->find("nextCursor");
        if (next != metadata->end()) {
            if (std::holds_alternative<std::nullptr_t>(next->second.value)) {
                next_cursor.reset();
            } else if (const auto* cursor = std::get_if<std::string>(&next->second.value);
                       cursor != nullptr && !next->second.number) {
                if (!cursor->empty()) next_cursor = *cursor;
            } else {
                return (error = "metadata.nextCursor must be null or a string", false);
            }
        }
        if (declared_cursor != nullptr) {
            declared_cursor->reset();
            const auto input = metadata->find("cursor");
            if (input != metadata->end()) {
                if (std::holds_alternative<std::nullptr_t>(input->second.value)) {
                    declared_cursor->reset();
                } else if (const auto* cursor = std::get_if<std::string>(&input->second.value);
                           cursor != nullptr && !input->second.number && !cursor->empty()) {
                    *declared_cursor = *cursor;
                } else {
                    return (error = "metadata.cursor must be null or a non-empty string", false);
                }
            }
        }
    } else if (servers->empty()) {
        next_cursor.reset();
    } else {
        return (error = "registry response is missing metadata object", false);
    }
    return true;
}

bool validate_artifact_path(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos) return false;
    std::filesystem::path parsed(path);
    for (const auto& component : parsed) if (component == "..") return false;
    return true;
}

struct ReconstructedState {
    std::size_t completed_pages{};
    std::size_t records_received{};
    std::optional<std::string> next_cursor;
    std::vector<std::pair<std::string, std::string>> records;
    std::vector<std::string> raw_paths;
    std::vector<Artifact> artifacts;
    std::set<std::string> cursors;
    std::string pages_jsonl;
};

bool promote_without_replace(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    std::string& error);

bool parse_page_number(
    std::string_view filename,
    std::size_t& number,
    std::string& error) {
    constexpr std::string_view prefix = "page-";
    constexpr std::string_view suffix = ".json";
    if (!filename.starts_with(prefix) || !filename.ends_with(suffix))
        return false;
    const std::string_view digits =
        filename.substr(prefix.size(), filename.size() - prefix.size() - suffix.size());
    if (digits.empty() ||
        !std::all_of(digits.begin(), digits.end(), [](char c) {
            return c >= '0' && c <= '9';
        })) {
        error = "legacy raw page filename has non-numeric page number: " +
            std::string(filename);
        return false;
    }
    const auto parsed = std::from_chars(
        digits.data(), digits.data() + digits.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() ||
        number == 0U) {
        error = "legacy raw page filename has invalid page number: " +
            std::string(filename);
        return false;
    }
    return true;
}

bool atomic_replace_file(
    const std::filesystem::path& path,
    std::string_view bytes,
    bool no_replace,
    std::string& error) {
    const auto temporary =
        path.parent_path() / (path.filename().string() + ".tmp-" + random_run_id());
    if (!write_file(temporary, bytes, error)) return false;
    if (no_replace) {
        if (!promote_without_replace(temporary, path, error)) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
    } else {
        std::error_code ec;
        std::filesystem::rename(temporary, path, ec);
        if (ec) {
            std::filesystem::remove(temporary, ec);
            error = "cannot atomically replace " + path.string();
            return false;
        }
    }
    return sync_directory(path.parent_path(), error);
}

bool reconstruct_state(
    const std::filesystem::path& partial,
    const RegistryUrl& origin,
    RegistryLimits limits,
    ReconstructedState& state,
    std::string& error) {
    state = {};
    const auto raw_directory = partial / "raw";
    std::error_code ec;
    if (!std::filesystem::is_directory(raw_directory, ec) || ec)
        return (error = "legacy partial bundle is missing raw directory", false);
    std::map<std::size_t, std::filesystem::path> numbered;
    for (std::filesystem::directory_iterator iterator(raw_directory, ec), end;
         !ec && iterator != end; iterator.increment(ec)) {
        if (!iterator->is_regular_file(ec)) continue;
        const std::string filename = iterator->path().filename().string();
        if (!filename.starts_with("page-") || !filename.ends_with(".json")) continue;
        std::size_t number{};
        if (!parse_page_number(filename, number, error)) return false;
        if (!numbered.emplace(number, iterator->path()).second)
            return (error = "duplicate legacy raw page number: " +
                std::to_string(number), false);
    }
    if (ec) return (error = "cannot enumerate legacy raw pages", false);
    if (numbered.empty()) return (error = "legacy partial bundle contains no raw pages", false);
    if (numbered.size() > limits.maximum_pages)
        return (error = "legacy page count exceeds configured limit", false);
    std::size_t expected_number = 1U;
    std::optional<std::string> expected_input;
    for (const auto& [number, path] : numbered) {
        if (number != expected_number)
            return (error = "missing legacy raw page number " +
                std::to_string(expected_number), false);
        std::string body;
        if (!read_file(path, limits.maximum_page_bytes, body, error)) return false;
        const std::size_t before = state.records.size();
        std::optional<std::string> next;
        std::optional<std::string> declared_input;
        if (!parse_page(
                body, partial, state.records, next, error, &declared_input))
            return (error = "invalid legacy page " + std::to_string(number) +
                ": " + error, false);
        if (state.records.size() > limits.maximum_records)
            return (error = "legacy record count exceeds configured limit", false);
        if (number > 1U && !expected_input.has_value())
            return (error = "legacy page continuation is broken before page " +
                std::to_string(number), false);
        if (declared_input.has_value() && declared_input != expected_input)
            return (error = "legacy cursor continuity mismatch at page " +
                std::to_string(number), false);
        std::string digest;
        if (!sha256_file(path, digest, error)) return false;
        const std::string relative = "raw/" + path.filename().string();
        Artifact artifact;
        artifact.path = relative;
        artifact.sha256 = digest;
        artifact.size = body.size();
        state.artifacts.push_back(artifact);
        state.raw_paths.push_back(relative);
        const std::string request_url = registry_api_url(
            origin, expected_input ?
                std::optional<std::string_view>(*expected_input) : std::nullopt);
        std::string metadata =
            "{\"content_type\":\"application/json\",\"effective_response_url\":";
        append_json_string(metadata, request_url);
        metadata += ",\"http_status\":200,\"page_number\":" +
            std::to_string(number) + ",\"pagination_input\":";
        if (expected_input) append_json_string(metadata, *expected_input);
        else metadata += "null";
        metadata += ",\"pagination_output\":";
        if (next) append_json_string(metadata, *next);
        else metadata += "null";
        metadata += ",\"path\":";
        append_json_string(metadata, relative);
        metadata += ",\"records\":" +
            std::to_string(state.records.size() - before) +
            ",\"reconstructed\":true,\"redirect_count\":0,\"request_url\":";
        append_json_string(metadata, request_url);
        metadata += ",\"response_bytes\":" + std::to_string(body.size()) +
            ",\"retrieved_at\":\"1970-01-01T00:00:00Z\",\"sha256\":";
        append_json_string(metadata, digest);
        metadata += "}\n";
        state.pages_jsonl += metadata;
        if (next && !state.cursors.insert(*next).second)
            return (error = "repeated legacy pagination cursor detected", false);
        expected_input = std::move(next);
        ++expected_number;
    }
    state.completed_pages = numbered.size();
    state.records_received = state.records.size();
    state.next_cursor = std::move(expected_input);
    return true;
}

std::string checkpoint_json(
    const RegistryUrl& origin,
    const ReconstructedState& state,
    const Artifact& pages_artifact) {
    std::string result = "{\"artifacts\":[";
    for (std::size_t index = 0U; index < state.artifacts.size(); ++index) {
        if (index != 0U) result.push_back(',');
        result += artifact_json(state.artifacts[index]);
    }
    if (!state.artifacts.empty()) result.push_back(',');
    result += artifact_json(pages_artifact);
    result += "],\"checkpoint_version\":1,\"completed_pages\":" +
        std::to_string(state.completed_pages) + ",\"next_cursor\":";
    if (state.next_cursor) append_json_string(result, *state.next_cursor);
    else result += "null";
    result += ",\"records_received\":" +
        std::to_string(state.records_received) +
        ",\"reconstructed_at\":";
    append_json_string(result, utc_now());
    result += ",\"registry\":\"official-mcp\",\"registry_base_url\":";
    append_json_string(result, origin.normalized);
    result += ",\"status\":\"partial\"}\n";
    return result;
}

bool checkpoint_matches(
    const std::filesystem::path& path,
    const RegistryUrl& origin,
    const ReconstructedState& state,
    std::string& error) {
    std::string bytes;
    if (!read_file(path, 4U * 1024U * 1024U, bytes, error)) return false;
    Json root;
    JsonParser parser(bytes);
    if (!parser.parse(root, error)) return false;
    const Json::Object* object = object_value(root);
    if (object == nullptr) return (error = "checkpoint must be an object", false);
    const std::string* base_url = string_member(*object, "registry_base_url");
    const std::string* registry = string_member(*object, "registry");
    if (base_url == nullptr || *base_url != origin.normalized ||
        registry == nullptr || *registry != registry_name)
        return (error = "checkpoint registry provenance does not match resume URL", false);
    const auto read_count = [&](std::string_view key, std::size_t expected) {
        const auto found = object->find(std::string(key));
        if (found == object->end() || !found->second.number) return false;
        const auto* text = std::get_if<std::string>(&found->second.value);
        if (text == nullptr) return false;
        std::size_t value{};
        const auto parsed = std::from_chars(
            text->data(), text->data() + text->size(), value);
        return parsed.ec == std::errc{} &&
            parsed.ptr == text->data() + text->size() && value == expected;
    };
    if (!read_count("completed_pages", state.completed_pages) ||
        !read_count("records_received", state.records_received))
        return (error = "checkpoint counts do not match raw pages", false);
    const std::string* cursor = string_member(*object, "next_cursor");
    if ((state.next_cursor.has_value() && (cursor == nullptr || *cursor != *state.next_cursor)) ||
        (!state.next_cursor.has_value() && cursor != nullptr))
        return (error = "checkpoint next_cursor does not match raw pages", false);
    std::map<std::string, Artifact> expected_artifacts;
    for (const Artifact& artifact : state.artifacts)
        expected_artifacts.emplace(artifact.path, artifact);
    Artifact pages_artifact;
    if (!artifact_for(
            path.parent_path(), "raw/pages.jsonl", pages_artifact, error))
        return false;
    expected_artifacts.emplace(pages_artifact.path, pages_artifact);
    const Json::Array* artifacts = array_member(*object, "artifacts");
    if (artifacts == nullptr || artifacts->size() != expected_artifacts.size())
        return (error = "checkpoint artifact declarations do not match raw pages", false);
    std::set<std::string> declared_paths;
    for (const Json& entry : *artifacts) {
        const Json::Object* declared = object_value(entry);
        if (declared == nullptr)
            return (error = "invalid checkpoint artifact declaration", false);
        const std::string* artifact_path = string_member(*declared, "path");
        const std::string* digest = string_member(*declared, "sha256");
        const auto size = declared->find("size");
        if (artifact_path == nullptr || digest == nullptr || size == declared->end() ||
            !size->second.number || !declared_paths.insert(*artifact_path).second)
            return (error = "invalid checkpoint artifact declaration", false);
        const auto expected = expected_artifacts.find(*artifact_path);
        const auto* size_text = std::get_if<std::string>(&size->second.value);
        std::uintmax_t parsed_size{};
        if (expected == expected_artifacts.end() || size_text == nullptr)
            return (error = "unexpected checkpoint artifact", false);
        const auto parsed = std::from_chars(
            size_text->data(), size_text->data() + size_text->size(), parsed_size);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != size_text->data() + size_text->size() ||
            parsed_size != expected->second.size ||
            *digest != expected->second.sha256)
            return (error = "checkpoint artifact metadata mismatch", false);
    }
    return true;
}

bool promote_without_replace(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    std::string& error) {
    const long result = syscall(
        SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(),
        RENAME_NOREPLACE);
    if (result == 0) return true;
    error = "cannot promote completed bundle without replacing a destination";
    return false;
}

}  // namespace

bool valid_utc_timestamp(std::string_view value) noexcept {
    if (value.size() != 20U || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':' || value[19] != 'Z') return false;
    for (const std::size_t index : {0U,1U,2U,3U,5U,6U,8U,9U,11U,12U,14U,15U,17U,18U})
        if (value[index] < '0' || value[index] > '9') return false;
    const auto number = [&](std::size_t index) {
        return static_cast<unsigned>((value[index] - '0') * 10 + value[index + 1U] - '0');
    };
    const unsigned month = number(5U);
    const unsigned day = number(8U);
    return month >= 1U && month <= 12U && day >= 1U && day <= 31U &&
        number(11U) <= 23U && number(14U) <= 59U && number(17U) <= 59U;
}

bool parse_registry_url(std::string_view text, RegistryUrl& result, std::string& error) {
    result = {};
    if (text.empty() || text.find('#') != std::string_view::npos)
        return (error = text.empty() ? "registry URL is empty" : "registry URL fragments are rejected", false);
    const std::size_t scheme_end = text.find("://");
    if (scheme_end == std::string_view::npos) return (error = "registry URL requires a scheme", false);
    result.scheme = lower_ascii(text.substr(0U, scheme_end));
    if (result.scheme != "http" && result.scheme != "https")
        return (error = "unsupported registry URL scheme", false);
    const std::size_t authority_begin = scheme_end + 3U;
    const std::size_t authority_end = text.find_first_of("/?", authority_begin);
    const std::string_view authority = text.substr(
        authority_begin,
        (authority_end == std::string_view::npos ? text.size() : authority_end) - authority_begin);
    if (authority.empty()) return (error = "registry URL host is empty", false);
    if (authority.find('@') != std::string_view::npos)
        return (error = "embedded registry URL credentials are rejected", false);
    std::string_view port_text;
    if (authority.front() == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos) return (error = "invalid bracketed registry host", false);
        result.host = lower_ascii(authority.substr(0U, close + 1U));
        if (close + 1U < authority.size()) {
            if (authority[close + 1U] != ':') return (error = "invalid registry URL authority", false);
            port_text = authority.substr(close + 2U);
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            if (authority.find(':') != colon) return (error = "IPv6 registry hosts require brackets", false);
            result.host = lower_ascii(authority.substr(0U, colon));
            port_text = authority.substr(colon + 1U);
        } else result.host = lower_ascii(authority);
    }
    if (result.host.empty()) return (error = "registry URL host is empty", false);
    result.port = result.scheme == "https" ? 443U : 80U;
    if (!port_text.empty()) {
        unsigned parsed{};
        const auto converted = std::from_chars(
            port_text.data(), port_text.data() + port_text.size(), parsed);
        if (converted.ec != std::errc{} || converted.ptr != port_text.data() + port_text.size() ||
            parsed == 0U || parsed > 65535U) return (error = "invalid registry URL port", false);
        result.port = parsed;
    } else if ((authority.back() == ':') || (authority.front() == '[' && authority.back() == ':')) {
        return (error = "invalid registry URL port", false);
    }
    if (result.scheme == "http" && result.host != "localhost" &&
        result.host != "127.0.0.1" && result.host != "[::1]")
        return (error = "plain HTTP is allowed only for explicit local hosts", false);
    if (authority_end == std::string_view::npos) result.base_path.clear();
    else {
        if (text[authority_end] == '?') return (error = "registry base URL query is not supported", false);
        result.base_path = std::string(text.substr(authority_end));
        if (result.base_path.find('?') != std::string::npos)
            return (error = "registry base URL query is not supported", false);
    }
    while (result.base_path.size() > 1U && result.base_path.back() == '/')
        result.base_path.pop_back();
    result.normalized = result.scheme + "://" + result.host;
    const unsigned default_port = result.scheme == "https" ? 443U : 80U;
    if (result.port != default_port) result.normalized += ":" + std::to_string(result.port);
    result.normalized += result.base_path;
    return true;
}

bool registry_same_origin(const RegistryUrl& left, const RegistryUrl& right) noexcept {
    return left.scheme == right.scheme && left.host == right.host && left.port == right.port;
}

std::string registry_api_url(const RegistryUrl& base, std::optional<std::string_view> cursor) {
    std::string result = base.normalized;
    result += "/v0.1/servers";
    if (cursor.has_value()) result += "?cursor=" + percent_encode(*cursor);
    return result;
}

bool reconstruct_registry_checkpoint(
    const std::filesystem::path& partial_bundle,
    std::string_view registry_base_url,
    RegistryLimits limits,
    std::string& message) {
    RegistryUrl origin;
    if (!parse_registry_url(registry_base_url, origin, message)) return false;
    if (limits.maximum_pages == 0U || limits.maximum_page_bytes == 0U ||
        limits.maximum_records == 0U)
        return (message = "checkpoint reconstruction limits must be greater than zero", false);
    std::error_code ec;
    if (!std::filesystem::is_directory(partial_bundle, ec) || ec)
        return (message = "legacy partial bundle directory does not exist", false);
    if (std::filesystem::exists(partial_bundle / "_SUCCESS", ec))
        return (message = "cannot reconstruct a completed bundle", false);
    if (std::filesystem::exists(partial_bundle / "checkpoint.json", ec))
        return (message = "checkpoint.json already exists", false);
    ReconstructedState state;
    if (!reconstruct_state(partial_bundle, origin, limits, state, message)) return false;
    if (!atomic_replace_file(
            partial_bundle / "raw/pages.jsonl", state.pages_jsonl, false, message))
        return false;
    Artifact pages_artifact;
    if (!artifact_for(
            partial_bundle, "raw/pages.jsonl", pages_artifact, message))
        return false;
    const std::string checkpoint =
        checkpoint_json(origin, state, pages_artifact);
    if (!atomic_replace_file(
            partial_bundle / "checkpoint.json", checkpoint, true, message))
        return false;
    if (std::filesystem::exists(partial_bundle / "_SUCCESS", ec))
        return (message = "unexpected _SUCCESS after checkpoint reconstruction", false);
    message = "checkpoint reconstructed: completed_pages=" +
        std::to_string(state.completed_pages) + " records_received=" +
        std::to_string(state.records_received) + " next_cursor=" +
        (state.next_cursor ? *state.next_cursor : "none");
    return true;
}

bool collect_registry(
    const RegistryCollectOptions& options,
    std::string& message,
    HttpTransport transport) {
    const auto run_started = std::chrono::steady_clock::now();
    const auto preflight_fail = [&](std::string stage, std::string category,
                                    std::string reason) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - run_started);
        const auto budget = std::chrono::seconds(
            options.limits.run_timeout_seconds);
        registry_progress(
            options.verbose,
            "failure stage=" + std::move(stage) +
                " category=" + std::move(category) +
                " page=1 completed_pages=0 completed_records=0 elapsed=" +
                seconds_text(elapsed) +
                " remaining=" + seconds_text(
                    std::max(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            budget - elapsed),
                        std::chrono::milliseconds::zero())) +
                " retained=none detail=" + progress_value(reason));
        message = std::move(reason);
        return false;
    };
    RegistryUrl origin;
    if (!parse_registry_url(options.registry_base_url, origin, message))
        return preflight_fail("startup", "invalid_configuration", message);
    if (options.output.empty())
        return preflight_fail(
            "startup", "invalid_configuration", "output bundle path is required");
    if (options.limits.maximum_pages == 0U || options.limits.maximum_page_bytes == 0U ||
        options.limits.maximum_records == 0U || options.limits.request_timeout_seconds == 0U ||
        options.limits.run_timeout_seconds == 0U)
        return preflight_fail(
            "startup", "invalid_configuration",
            "all configured limits except maximum redirects must be greater than zero");
    std::error_code ec;
    if (std::filesystem::exists(options.output, ec))
        return preflight_fail(
            "startup", "persistence_failure",
            "destination already exists: " + options.output.string());
    const std::filesystem::path parent =
        options.output.parent_path().empty() ? "." : options.output.parent_path();
    if (!std::filesystem::exists(parent, ec))
        return preflight_fail(
            "startup", "persistence_failure",
            "output parent directory does not exist");
    const std::filesystem::path temporary =
        parent / (options.output.filename().string() + ".partial-" + random_run_id());
    if (!std::filesystem::create_directory(temporary, ec) || ec)
        return preflight_fail(
            "startup", "persistence_failure",
            "cannot create temporary bundle directory");
    std::filesystem::create_directories(temporary / "raw", ec);
    std::filesystem::create_directories(temporary / "canonical", ec);
    std::filesystem::create_directories(temporary / "diagnostics", ec);
    const std::string started_at = utc_now();
    const std::string run_id = random_run_id();
    const auto run_deadline = run_started +
        std::chrono::seconds(options.limits.run_timeout_seconds);
    const auto heartbeat_interval = std::max(
        options.heartbeat_interval, std::chrono::milliseconds(1'000));
    if (!transport) transport = curl_transport;

    std::vector<std::pair<std::string, std::string>> records;
    std::set<std::string> cursors;
    std::string pages_jsonl;
    std::vector<std::string> raw_paths;
    std::optional<std::string> cursor;
    std::size_t pages = 0U;
    bool collection_complete = false;
    std::string failure_stage = "startup";
    std::string failure_category = "persistence_failure";
    std::size_t failure_page = 1U;
    const auto elapsed = [&]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - run_started);
    };
    const auto remaining = [&]() {
        const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
            run_deadline - std::chrono::steady_clock::now());
        return std::max(value, std::chrono::milliseconds::zero());
    };
    auto fail = [&](std::string reason) {
        if (reason.size() > maximum_diagnostic_bytes) reason.resize(maximum_diagnostic_bytes);
        std::string diagnostic = "{\"error\":";
        append_json_string(diagnostic, reason);
        diagnostic += ",\"status\":\"failed\"}\n";
        std::string ignored;
        write_file(temporary / "diagnostics/errors.jsonl", diagnostic, ignored);
        const std::filesystem::path retained =
            std::filesystem::exists(temporary) ? temporary : options.output;
        registry_progress(
            options.verbose,
            "failure stage=" + failure_stage +
                " category=" + failure_category +
                " page=" + std::to_string(failure_page) +
                " completed_pages=" + std::to_string(pages) +
                " completed_records=" + std::to_string(records.size()) +
                " elapsed=" + seconds_text(elapsed()) +
                " remaining=" + seconds_text(remaining()) +
                " retained=" + progress_value(retained.string()) +
                " detail=" + progress_value(reason));
        message = reason + "; failed bundle retained at " + temporary.string();
        return false;
    };

    registry_progress(
        options.verbose,
        "mode=" + std::string(options.resume ? "resume" : "new") +
            " registry=" + progress_value(origin.normalized) +
            " output=" + progress_value(options.output.string()) +
            (options.resume ?
                " resume_source=" + progress_value(options.resume->string()) : ""));
    registry_progress(
        options.verbose,
        "request_timeout=" +
            std::to_string(options.limits.request_timeout_seconds) +
            "s run_timeout=" + std::to_string(options.limits.run_timeout_seconds) +
            "s maximum_pages=" + std::to_string(options.limits.maximum_pages) +
            " maximum_records=" + std::to_string(options.limits.maximum_records));

    if (options.resume.has_value()) {
        const std::filesystem::path& resume = *options.resume;
        failure_stage = "checkpoint_loading";
        failure_category = "checkpoint_validation_failure";
        registry_progress(options.verbose, "resume checkpoint_load");
        if (std::filesystem::equivalent(resume, options.output, ec) && !ec)
            return fail("resume directory and output destination must differ");
        if (!std::filesystem::exists(resume / "checkpoint.json", ec)) {
            registry_progress(options.verbose, "resume checkpoint_reconstruct_start");
            std::string checkpoint_message;
            if (!reconstruct_registry_checkpoint(
                    resume, origin.normalized, options.limits, checkpoint_message))
                return fail("cannot reconstruct resume checkpoint: " + checkpoint_message);
            registry_progress(options.verbose, "resume checkpoint_reconstruct_complete");
        }
        ReconstructedState state;
        std::string resume_error;
        registry_progress(options.verbose, "resume raw_artifact_validation_start");
        if (!reconstruct_state(resume, origin, options.limits, state, resume_error))
            return fail("invalid resume bundle: " + resume_error);
        registry_progress(
            options.verbose,
            "resume raw_artifact_validation_complete pages=" +
                std::to_string(state.completed_pages) +
                " artifacts=" + std::to_string(state.artifacts.size() + 1U));
        failure_stage = "checkpoint_validation";
        registry_progress(
            options.verbose,
            "resume checkpoint_validation pages=" +
                std::to_string(state.completed_pages) +
                " artifacts=" + std::to_string(state.artifacts.size() + 1U));
        if (!checkpoint_matches(
                resume / "checkpoint.json", origin, state, resume_error))
            return fail("invalid resume checkpoint: " + resume_error);
        registry_progress(options.verbose, "resume checkpoint_validation_complete");
        failure_stage = "resume_copy";
        failure_category = "persistence_failure";
        std::uintmax_t copied_bytes = 0U;
        registry_progress(
            options.verbose,
            "resume copy_start pages=" + std::to_string(state.raw_paths.size()));
        std::size_t copied_pages = 0U;
        for (const std::string& relative : state.raw_paths) {
            std::string body;
            if (!read_file(
                    resume / relative, options.limits.maximum_page_bytes,
                    body, resume_error) ||
                !write_file(temporary / relative, body, resume_error))
                return fail("cannot copy resume page: " + resume_error);
            copied_bytes += body.size();
            ++copied_pages;
            if (copied_pages % 50U == 0U && copied_pages < state.raw_paths.size())
                registry_progress(
                    options.verbose,
                    "resume copy pages=" + std::to_string(copied_pages) + "/" +
                        std::to_string(state.raw_paths.size()) +
                        " bytes=" + std::to_string(copied_bytes));
        }
        registry_progress(
            options.verbose,
            "resume copy_complete pages=" + std::to_string(copied_pages) +
                " bytes=" + std::to_string(copied_bytes));
        records = std::move(state.records);
        cursors = std::move(state.cursors);
        pages_jsonl = std::move(state.pages_jsonl);
        raw_paths = std::move(state.raw_paths);
        cursor = std::move(state.next_cursor);
        pages = state.completed_pages;
        collection_complete = !cursor.has_value();
        registry_progress(
            options.verbose,
            "resume continuation_page=" + std::to_string(pages + 1U) +
                " " + cursor_progress(cursor));
    }

    registry_progress(
        options.verbose,
        "completed_pages=" + std::to_string(pages) +
            " completed_records=" + std::to_string(records.size()) +
            " next_page=" + std::to_string(pages + 1U) +
            " next_cursor=" + std::string(cursor ? "yes" : "no") +
            " " + cursor_progress(cursor));

    while (!collection_complete) {
        failure_page = pages + 1U;
        failure_stage = "pagination";
        failure_category = "persistence_failure";
        if (pages >= options.limits.maximum_pages) return fail("maximum page count exceeded");
        if (std::chrono::steady_clock::now() >= run_deadline) {
            failure_category = "total_run_deadline_exhausted";
            return fail("total run timeout exceeded");
        }
        const std::string request_url = registry_api_url(
            origin, cursor ? std::optional<std::string_view>(*cursor) : std::nullopt);
        std::string current_url = request_url;
        HttpResponse response;
        std::size_t redirects = 0U;
        const auto page_started = std::chrono::steady_clock::now();
        while (true) {
            const auto remaining_duration =
                run_deadline - std::chrono::steady_clock::now();
            const auto remaining_milliseconds =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    remaining_duration).count();
            if (remaining_milliseconds <= 0) {
                failure_category = "total_run_deadline_exhausted";
                return fail("total run timeout exceeded");
            }
            const auto remaining_seconds =
                (remaining_milliseconds + 999LL) / 1000LL;
            const unsigned timeout = std::min(
                options.limits.request_timeout_seconds,
                static_cast<unsigned>(remaining_seconds));
            registry_progress(
                options.verbose,
                "page=" + std::to_string(pages + 1U) +
                    " attempt=1 request_start elapsed=" + seconds_text(elapsed()) +
                    " remaining=" + seconds_text(
                        std::chrono::milliseconds(remaining_milliseconds)) +
                    " timeout=" + std::to_string(timeout) + ".0s " +
                    cursor_progress(cursor));
            std::string transport_error;
            const HttpHeartbeat heartbeat = [&](std::chrono::milliseconds waiting) {
                registry_progress(
                    options.verbose,
                    "page=" + std::to_string(pages + 1U) +
                        " attempt=1 waiting=" +
                        std::to_string(
                            std::chrono::duration_cast<std::chrono::seconds>(
                                waiting).count()) +
                        "s remaining=" + seconds_text(remaining()));
            };
            failure_stage = "http_request";
            if (!transport(current_url, timeout, options.limits.maximum_page_bytes,
                           response, transport_error, heartbeat, heartbeat_interval)) {
                const bool deadline_limited =
                    timeout < options.limits.request_timeout_seconds;
                const bool deadline_reached =
                    std::chrono::steady_clock::now() >= run_deadline;
                if (deadline_limited || deadline_reached) {
                    failure_category = "total_run_deadline_exhausted";
                    return fail("total run deadline exhausted during HTTP request: " +
                        transport_error);
                }
                const std::string lowered = lower_ascii(transport_error);
                failure_category =
                    lowered.find("connect") != std::string::npos &&
                            lowered.find("tim") != std::string::npos ?
                        "connect_timeout" :
                        (lowered.find("tim") != std::string::npos ?
                            "request_timeout" : "http_error");
                return fail("HTTP request failed: " + transport_error);
            }
            RegistryUrl effective;
            const std::size_t effective_query = response.effective_url.find('?');
            failure_stage = "response_validation";
            failure_category = "malformed_response";
            if (!parse_registry_url(
                    std::string_view(response.effective_url).substr(0U, effective_query),
                    effective, transport_error) ||
                !registry_same_origin(origin, effective))
                return fail("HTTP response effective URL is outside configured origin");
            if (effective_query != std::string::npos)
                effective.normalized += response.effective_url.substr(effective_query);
            response.effective_url = effective.normalized;
            if (response.status < 300U || response.status >= 400U) break;
            if (!response.location.has_value()) {
                failure_category = "http_error";
                return fail("redirect response is missing Location");
            }
            if (redirects >= options.limits.maximum_redirects) {
                failure_category = "http_error";
                return fail("maximum redirect count exceeded");
            }
            if (!resolve_redirect(origin, current_url, *response.location, current_url, transport_error)) {
                failure_category = "http_error";
                return fail(transport_error);
            }
            ++redirects;
        }
        if (response.status != 200U) {
            failure_category = "http_error";
            return fail("registry returned HTTP " + std::to_string(response.status));
        }
        if (response.body.size() > options.limits.maximum_page_bytes)
            return fail("registry page exceeds configured byte limit");
        const std::string content_type = lower_ascii(response.content_type);
        if (!content_type.starts_with("application/json"))
            return fail("registry response content type is not application/json");
        const std::size_t page_number = pages + 1U;
        failure_page = page_number;
        failure_stage = "page_persistence";
        failure_category = "persistence_failure";
        std::ostringstream page_name;
        page_name << "raw/page-" << std::setw(6) << std::setfill('0')
                  << page_number << ".json";
        const std::string raw_path = page_name.str();
        std::string error;
        if (!write_file(temporary / raw_path, response.body, error)) return fail(error);
        raw_paths.push_back(raw_path);
        std::string raw_sha;
        if (!sha256_file(temporary / raw_path, raw_sha, error)) return fail(error);
        std::optional<std::string> next_cursor;
        const std::size_t before = records.size();
        if (!parse_page(response.body, temporary, records, next_cursor, error)) {
            records.resize(before);
            failure_stage = "response_validation";
            failure_category = "malformed_response";
            return fail("invalid registry JSON: " + error);
        }
        if (std::chrono::steady_clock::now() >= run_deadline) {
            records.resize(before);
            failure_category = "total_run_deadline_exhausted";
            return fail("total run timeout exceeded");
        }
        if (records.size() > options.limits.maximum_records) {
            records.resize(before);
            return fail("maximum record count exceeded");
        }
        ++pages;
        const std::string retrieved_at = utc_now();
        std::string metadata = "{\"content_type\":";
        append_json_string(metadata, response.content_type);
        metadata += ",\"effective_response_url\":";
        append_json_string(metadata, response.effective_url);
        metadata += ",\"http_status\":" + std::to_string(response.status) +
            ",\"page_number\":" + std::to_string(pages) + ",\"pagination_input\":";
        if (cursor) append_json_string(metadata, *cursor); else metadata += "null";
        metadata += ",\"pagination_output\":";
        if (next_cursor) append_json_string(metadata, *next_cursor); else metadata += "null";
        metadata += ",\"path\":";
        append_json_string(metadata, raw_path);
        metadata += ",\"records\":" + std::to_string(records.size() - before) +
            ",\"redirect_count\":" + std::to_string(redirects) + ",\"request_url\":";
        append_json_string(metadata, request_url);
        metadata += ",\"response_bytes\":" + std::to_string(response.body.size()) +
            ",\"retrieved_at\":";
        append_json_string(metadata, retrieved_at);
        metadata += ",\"sha256\":";
        append_json_string(metadata, raw_sha);
        metadata += "}\n";
        pages_jsonl += metadata;
        registry_progress(
            options.verbose,
            "page=" + std::to_string(pages) +
                " status=" + std::to_string(response.status) +
                " bytes=" + std::to_string(response.body.size()) +
                " duration=" + seconds_text(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - page_started)) +
                " records=" + std::to_string(records.size() - before) +
                " total_records=" + std::to_string(records.size()) +
                " completed_pages=" + std::to_string(pages) +
                " next_cursor=" + std::string(next_cursor ? "yes" : "no") +
                " saved=" + raw_path + " checkpoint=not_applicable");
        if (!next_cursor.has_value()) {
            collection_complete = true;
            break;
        }
        if (!cursors.insert(*next_cursor).second) return fail("repeated pagination cursor detected");
        cursor = std::move(next_cursor);
    }

    registry_progress(
        options.verbose,
        "pagination_complete pages=" + std::to_string(pages) +
            " records=" + std::to_string(records.size()));
    failure_stage = "canonicalization";
    failure_category = "canonicalization_failure";
    registry_progress(options.verbose, "canonicalization_start");
    std::sort(records.begin(), records.end(), [](const auto& left, const auto& right) {
        return left.first < right.first || (left.first == right.first && left.second < right.second);
    });
    std::string canonical;
    std::size_t unique = 0U;
    for (std::size_t i = 0U; i < records.size();) {
        std::size_t end = i + 1U;
        while (end < records.size() && records[end].first == records[i].first) {
            if (records[end].second != records[i].second)
                return fail("duplicate canonical identity has conflicting content");
            ++end;
        }
        canonical += records[i].second;
        ++unique;
        i = end;
        if (unique % 10'000U == 0U)
            registry_progress(
                options.verbose,
                "canonicalization progress_records=" + std::to_string(i) + "/" +
                    std::to_string(records.size()) +
                    " unique_versions=" + std::to_string(unique));
    }
    registry_progress(
        options.verbose,
        "canonicalization_complete unique_versions=" + std::to_string(unique));
    std::string error;
    if (std::chrono::steady_clock::now() >= run_deadline) {
        failure_category = "total_run_deadline_exhausted";
        return fail("total run timeout exceeded");
    }
    failure_stage = "artifact_persistence";
    failure_category = "persistence_failure";
    if (!write_file(temporary / "raw/pages.jsonl", pages_jsonl, error) ||
        !write_file(temporary / "canonical/servers.jsonl", canonical, error) ||
        !write_file(temporary / "diagnostics/errors.jsonl", "", error)) return fail(error);
    failure_stage = "manifest_generation";
    registry_progress(options.verbose, "manifest_generation");
    std::vector<Artifact> artifacts;
    for (const std::string& raw : raw_paths) {
        Artifact artifact;
        if (!artifact_for(temporary, raw, artifact, error)) return fail(error);
        artifacts.push_back(std::move(artifact));
        if (artifacts.size() % 50U == 0U && artifacts.size() < raw_paths.size())
            registry_progress(
                options.verbose,
                "manifest_generation artifacts=" +
                    std::to_string(artifacts.size()) + "/" +
                    std::to_string(raw_paths.size() + 3U));
    }
    for (const std::string relative :
         {"raw/pages.jsonl", "canonical/servers.jsonl", "diagnostics/errors.jsonl"}) {
        Artifact artifact;
        if (!artifact_for(temporary, relative, artifact, error)) return fail(error);
        artifacts.push_back(std::move(artifact));
    }
    std::string snapshot_sha;
    if (!sha256_file(temporary / "canonical/servers.jsonl", snapshot_sha, error)) return fail(error);
    const std::string completed_at = utc_now();
    std::string manifest = "{\"artifacts\":[";
    for (std::size_t i = 0U; i < artifacts.size(); ++i) {
        if (i != 0U) manifest.push_back(',');
        manifest += artifact_json(artifacts[i]);
    }
    manifest += "],\"bundle_version\":1,\"collector\":{\"git_commit\":";
    append_json_string(manifest, MCPO_GIT_COMMIT);
    manifest += ",\"name\":\"mcp-observatory\",\"version\":";
    append_json_string(manifest, MCPO_VERSION);
    manifest += "},\"completed_at\":";
    append_json_string(manifest, completed_at);
    manifest += ",\"counts\":{\"pages\":" + std::to_string(pages) +
        ",\"records_received\":" + std::to_string(records.size()) +
        ",\"unique_server_versions\":" + std::to_string(unique) +
        "},\"limits\":{\"maximum_page_bytes\":" +
        std::to_string(options.limits.maximum_page_bytes) + ",\"maximum_pages\":" +
        std::to_string(options.limits.maximum_pages) + ",\"maximum_records\":" +
        std::to_string(options.limits.maximum_records) + ",\"maximum_redirects\":" +
        std::to_string(options.limits.maximum_redirects) + ",\"request_timeout_seconds\":" +
        std::to_string(options.limits.request_timeout_seconds) + ",\"run_timeout_seconds\":" +
        std::to_string(options.limits.run_timeout_seconds) + "},\"registry\":\"official-mcp\","
        "\"registry_base_url\":";
    append_json_string(manifest, origin.normalized);
    manifest += ",\"run_id\":";
    append_json_string(manifest, run_id);
    manifest += ",\"snapshot_sha256\":";
    append_json_string(manifest, snapshot_sha);
    manifest += ",\"started_at\":";
    append_json_string(manifest, started_at);
    manifest += ",\"status\":\"complete\"}\n";
    if (!write_file(temporary / "manifest.json", manifest, error)) return fail(error);
    failure_stage = "final_validation";
    failure_category = "final_validation_failure";
    registry_progress(options.verbose, "final_bundle_validation");
    std::string validation;
    if (!validate_bundle_impl(temporary, false, validation))
        return fail("bundle self-validation failed: " + validation);
    registry_progress(options.verbose, "validation_complete");
    failure_stage = "success_marker";
    failure_category = "persistence_failure";
    registry_progress(options.verbose, "success_marker_create path=_SUCCESS");
    if (!write_file(temporary / "_SUCCESS", "", error)) return fail(error);
    if (!sync_directory(temporary / "raw", error) ||
        !sync_directory(temporary / "canonical", error) ||
        !sync_directory(temporary / "diagnostics", error) ||
        !sync_directory(temporary, error)) return fail(error);
    failure_stage = "atomic_promotion";
    registry_progress(
        options.verbose,
        "atomic_promotion from=" + progress_value(temporary.string()) +
            " to=" + progress_value(options.output.string()));
    if (!promote_without_replace(temporary, options.output, error)) return fail(error);
    if (!sync_directory(parent, error)) {
        registry_progress(
            options.verbose,
            "failure stage=atomic_promotion category=persistence_failure"
                " page=" + std::to_string(failure_page) +
                " completed_pages=" + std::to_string(pages) +
                " completed_records=" + std::to_string(records.size()) +
                " elapsed=" + seconds_text(elapsed()) +
                " remaining=" + seconds_text(remaining()) +
                " retained=" + progress_value(options.output.string()) +
                " detail=" + progress_value(error));
        message = "bundle promoted but parent-directory flush failed: " + error;
        return false;
    }
    message = "registry collection complete: pages=" + std::to_string(pages) +
        " records=" + std::to_string(records.size()) + " unique=" + std::to_string(unique) +
        " snapshot_sha256=" + snapshot_sha;
    registry_progress(
        options.verbose,
        "success output=" + progress_value(options.output.string()) +
            " pages=" + std::to_string(pages) +
            " records=" + std::to_string(records.size()) +
            " unique_versions=" + std::to_string(unique) +
            " duration=" + seconds_text(elapsed()));
    return true;
}

namespace {

bool validate_bundle_impl(
    const std::filesystem::path& bundle,
    bool require_success,
    std::string& message) {
    if (require_success && !std::filesystem::is_regular_file(bundle / "_SUCCESS"))
        return (message = "missing _SUCCESS marker", false);
    std::string manifest_bytes;
    if (!read_file(bundle / "manifest.json", 4U * 1024U * 1024U, manifest_bytes, message)) return false;
    Json manifest_json;
    JsonParser parser(manifest_bytes);
    if (!parser.parse(manifest_json, message)) return false;
    const Json::Object* manifest = object_value(manifest_json);
    if (manifest == nullptr) return (message = "manifest must be a JSON object", false);
    const std::string* status = string_member(*manifest, "status");
    const std::string* base_text = string_member(*manifest, "registry_base_url");
    const std::string* snapshot = string_member(*manifest, "snapshot_sha256");
    const std::string* started = string_member(*manifest, "started_at");
    const std::string* completed = string_member(*manifest, "completed_at");
    const std::string* registry = string_member(*manifest, "registry");
    const Json::Array* artifacts = array_member(*manifest, "artifacts");
    if (status == nullptr || *status != "complete" || base_text == nullptr ||
        snapshot == nullptr || started == nullptr || completed == nullptr ||
        !valid_utc_timestamp(*started) || !valid_utc_timestamp(*completed) ||
        *completed < *started || registry == nullptr || *registry != registry_name ||
        artifacts == nullptr)
        return (message = "manifest is missing required complete-bundle fields", false);
    RegistryUrl base;
    std::string error;
    if (!parse_registry_url(*base_text, base, error) || base.normalized != *base_text)
        return (message = "manifest registry_base_url is not normalized", false);
    for (const Json& entry : *artifacts) {
        const Json::Object* artifact = object_value(entry);
        if (artifact == nullptr) return (message = "artifact declaration must be an object", false);
        const std::string* path = string_member(*artifact, "path");
        const std::string* digest = string_member(*artifact, "sha256");
        const auto size_it = artifact->find("size");
        if (path == nullptr || digest == nullptr || size_it == artifact->end() ||
            !size_it->second.number || !validate_artifact_path(*path))
            return (message = "invalid artifact declaration", false);
        const std::string* size_text = std::get_if<std::string>(&size_it->second.value);
        std::uintmax_t expected_size{};
        if (size_text == nullptr) return (message = "invalid artifact size", false);
        const auto parsed = std::from_chars(
            size_text->data(), size_text->data() + size_text->size(), expected_size);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != size_text->data() + size_text->size())
            return (message = "invalid artifact size", false);
        std::error_code ec;
        const auto actual_size = std::filesystem::file_size(bundle / *path, ec);
        if (ec || actual_size != expected_size)
            return (message = "artifact size mismatch: " + *path, false);
        std::string actual_digest;
        if (!sha256_file(bundle / *path, actual_digest, error) || actual_digest != *digest)
            return (message = "artifact SHA-256 mismatch: " + *path, false);
    }
    std::string canonical;
    if (!read_file(bundle / "canonical/servers.jsonl", 512U * 1024U * 1024U, canonical, message))
        return false;
    std::string actual_snapshot;
    if (!sha256_file(bundle / "canonical/servers.jsonl", actual_snapshot, error) ||
        actual_snapshot != *snapshot)
        return (message = "snapshot SHA-256 mismatch", false);
    std::string previous_identity;
    std::size_t canonical_count = 0U;
    std::istringstream lines(canonical);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.empty()) return (message = "empty canonical JSONL record", false);
        Json record_json;
        JsonParser record_parser(line);
        if (!record_parser.parse(record_json, error)) return (message = "invalid canonical JSONL", false);
        std::string normalized;
        canonical_json(record_json, normalized);
        if (normalized != line) return (message = "canonical JSONL record is not normalized", false);
        const Json::Object* record = object_value(record_json);
        if (record == nullptr) return (message = "canonical record must be an object", false);
        const std::string* name = string_member(*record, "server_identifier");
        const std::string* version = string_member(*record, "server_version");
        const std::string* observed = string_member(*record, "observed_at");
        const std::string* declared_record_hash = string_member(*record, "canonical_sha256");
        if (name == nullptr || version == nullptr || observed == nullptr ||
            declared_record_hash == nullptr || !valid_utc_timestamp(*observed))
            return (message = "invalid canonical identity, timestamp, or hash", false);
        Json::Object logical = *record;
        logical.erase("observed_at");
        logical.erase("canonical_sha256");
        Json logical_json{std::move(logical), false};
        std::string logical_bytes;
        canonical_json(logical_json, logical_bytes);
        std::string actual_record_hash;
        if (!sha256_bytes(bundle, logical_bytes, actual_record_hash, error) ||
            actual_record_hash != *declared_record_hash)
            return (message = "canonical record content hash mismatch", false);
        const std::string identity = *name + "\n" + *version;
        if (!previous_identity.empty() && identity <= previous_identity)
            return (message = "canonical records are not strictly ordered", false);
        previous_identity = identity;
        ++canonical_count;
    }
    const Json::Object* counts = object_member(*manifest, "counts");
    if (counts == nullptr) return (message = "manifest counts missing", false);
    const auto unique_it = counts->find("unique_server_versions");
    if (unique_it == counts->end() || !unique_it->second.number)
        return (message = "manifest unique count missing", false);
    std::size_t expected{};
    const std::string* expected_text = std::get_if<std::string>(&unique_it->second.value);
    if (expected_text == nullptr)
        return (message = "canonical record count mismatch", false);
    const auto converted = std::from_chars(
        expected_text->data(), expected_text->data() + expected_text->size(), expected);
    if (converted.ec != std::errc{} || converted.ptr != expected_text->data() + expected_text->size() ||
        expected != canonical_count)
        return (message = "canonical record count mismatch", false);
    std::string pages;
    if (!read_file(bundle / "raw/pages.jsonl", 128U * 1024U * 1024U, pages, message)) return false;
    std::istringstream page_lines(pages);
    std::size_t page_count = 0U;
    while (std::getline(page_lines, line)) {
        if (line.empty()) return (message = "empty raw page metadata record", false);
        Json page;
        JsonParser page_parser(line);
        if (!page_parser.parse(page, error)) return (message = "invalid raw page metadata", false);
        const Json::Object* page_object = object_value(page);
        if (page_object == nullptr) return (message = "raw page metadata must be an object", false);
        const std::string* timestamp = string_member(*page_object, "retrieved_at");
        const std::string* request = string_member(*page_object, "request_url");
        const std::string* effective = string_member(*page_object, "effective_response_url");
        const std::string* raw_path = string_member(*page_object, "path");
        const std::string* raw_digest = string_member(*page_object, "sha256");
        const auto bytes_it = page_object->find("response_bytes");
        if (timestamp == nullptr || !valid_utc_timestamp(*timestamp) || request == nullptr ||
            effective == nullptr || raw_path == nullptr || raw_digest == nullptr ||
            bytes_it == page_object->end() || !bytes_it->second.number ||
            !validate_artifact_path(*raw_path) || request->find('@') != std::string::npos ||
            effective->find('@') != std::string::npos)
            return (message = "invalid raw page provenance metadata", false);
        const std::string* raw_bytes_text = std::get_if<std::string>(&bytes_it->second.value);
        std::uintmax_t raw_bytes{};
        if (raw_bytes_text == nullptr)
            return (message = "raw page byte count mismatch", false);
        const auto raw_size_parsed = std::from_chars(
            raw_bytes_text->data(), raw_bytes_text->data() + raw_bytes_text->size(),
            raw_bytes);
        std::error_code page_ec;
        if (raw_size_parsed.ec != std::errc{} ||
            raw_size_parsed.ptr != raw_bytes_text->data() + raw_bytes_text->size() ||
            std::filesystem::file_size(bundle / *raw_path, page_ec) != raw_bytes || page_ec)
            return (message = "raw page byte count mismatch", false);
        std::string actual_raw_digest;
        if (!sha256_file(bundle / *raw_path, actual_raw_digest, error) ||
            actual_raw_digest != *raw_digest)
            return (message = "raw page SHA-256 mismatch", false);
        ++page_count;
    }
    const auto pages_it = counts->find("pages");
    std::size_t expected_pages{};
    const std::string* pages_text = pages_it == counts->end() ? nullptr :
        std::get_if<std::string>(&pages_it->second.value);
    if (pages_text == nullptr) return (message = "raw page metadata count mismatch", false);
    const auto pages_parsed = std::from_chars(
        pages_text->data(), pages_text->data() + pages_text->size(), expected_pages);
    if (pages_parsed.ec != std::errc{} ||
        pages_parsed.ptr != pages_text->data() + pages_text->size() ||
        expected_pages != page_count)
        return (message = "raw page metadata count mismatch", false);
    message = "bundle valid: records=" + std::to_string(canonical_count) +
        " snapshot_sha256=" + actual_snapshot;
    return true;
}

}  // namespace

bool validate_bundle(const std::filesystem::path& bundle, std::string& message) {
    return validate_bundle_impl(bundle, true, message);
}

}  // namespace mcpo
