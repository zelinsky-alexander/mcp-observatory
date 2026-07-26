#include "observatory/registry.hpp"
#include "observatory/explorer.hpp"

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
#include <limits>
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

bool valid_utf8(std::string_view input) {
    std::size_t position = 0U;
    while (position < input.size()) {
        const auto first = static_cast<unsigned char>(input[position++]);
        if (first <= 0x7fU) continue;
        unsigned continuation_count{};
        std::uint32_t value{};
        if (first >= 0xc2U && first <= 0xdfU) {
            continuation_count = 1U;
            value = first & 0x1fU;
        } else if (first >= 0xe0U && first <= 0xefU) {
            continuation_count = 2U;
            value = first & 0x0fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            continuation_count = 3U;
            value = first & 0x07U;
        } else {
            return false;
        }
        if (position + continuation_count > input.size()) return false;
        for (unsigned index = 0U; index < continuation_count; ++index) {
            const auto continuation =
                static_cast<unsigned char>(input[position++]);
            if ((continuation & 0xc0U) != 0x80U) return false;
            value = (value << 6U) | (continuation & 0x3fU);
        }
        if ((continuation_count == 2U &&
             (value < 0x800U || (value >= 0xd800U && value <= 0xdfffU))) ||
            (continuation_count == 3U &&
             (value < 0x10000U || value > 0x10ffffU)))
            return false;
    }
    return true;
}

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
        if (!valid_utf8(input_)) return fail(error, "invalid UTF-8 in JSON");
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
    for (const char raw_character : value) {
        const auto c = static_cast<unsigned char>(raw_character);
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
    for (const char raw_character : value) {
        const auto c = static_cast<unsigned char>(raw_character);
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
    const bool flush_failed = fsync(descriptor) != 0;
    const bool close_failed = close(descriptor) != 0;
    if (flush_failed || close_failed) {
        error = "cannot flush " + path.string();
        return false;
    }
    return true;
}

bool sync_directory(const std::filesystem::path& path, std::string& error) {
    const int descriptor = open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (descriptor < 0) return (error = "cannot open bundle directory for flush", false);
    const bool flush_failed = fsync(descriptor) != 0;
    const bool close_failed = close(descriptor) != 0;
    if (flush_failed || close_failed)
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

enum class ProcessFailureKind {
    none,
    timeout,
    cancelled,
    output_limit,
    local_io,
};

bool run_process(
    const std::vector<std::string>& arguments,
    std::chrono::steady_clock::duration timeout,
    std::size_t maximum_output,
    std::string& output,
    int& exit_status,
    std::string& error,
    const HttpHeartbeat& heartbeat = {},
    std::chrono::steady_clock::duration initial_wake =
        std::chrono::seconds(5),
    ProcessFailureKind* failure_kind = nullptr) {
    if (failure_kind != nullptr) *failure_kind = ProcessFailureKind::none;
    int pipe_fds[2]{};
    if (pipe(pipe_fds) != 0) {
        if (failure_kind != nullptr) *failure_kind = ProcessFailureKind::local_io;
        error = "cannot create process pipe";
        return false;
    }
    const pid_t child = fork();
    if (child < 0) {
        close(pipe_fds[0]); close(pipe_fds[1]);
        if (failure_kind != nullptr) *failure_kind = ProcessFailureKind::local_io;
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
    const auto deadline = started + timeout;
    auto next_observation = started + initial_wake;
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
                if (failure_kind != nullptr)
                    *failure_kind = ProcessFailureKind::output_limit;
                error = "child process output limit exceeded";
                return false;
            }
            output.append(buffer.data(), static_cast<std::size_t>(count));
        }
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) done = true;
        else if (waited < 0) {
            close(pipe_fds[0]);
            if (failure_kind != nullptr) *failure_kind = ProcessFailureKind::local_io;
            error = "cannot wait for child process";
            return false;
        }
        else if (std::chrono::steady_clock::now() >= deadline) {
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            close(pipe_fds[0]);
            if (failure_kind != nullptr) *failure_kind = ProcessFailureKind::timeout;
            error = "child process timed out";
            return false;
        } else {
            const auto now = std::chrono::steady_clock::now();
            if (heartbeat && initial_wake >
                    std::chrono::steady_clock::duration::zero() &&
                now >= next_observation) {
                const HttpWaitDecision decision = heartbeat(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - started));
                if (!decision.continue_waiting) {
                    kill(child, SIGKILL);
                    waitpid(child, &status, 0);
                    close(pipe_fds[0]);
                    if (failure_kind != nullptr)
                        *failure_kind = ProcessFailureKind::cancelled;
                    error = "child process cancelled";
                    return false;
                }
                if (decision.next_wake <=
                    std::chrono::steady_clock::duration::zero()) {
                    kill(child, SIGKILL);
                    waitpid(child, &status, 0);
                    close(pipe_fds[0]);
                    if (failure_kind != nullptr)
                        *failure_kind = ProcessFailureKind::cancelled;
                    error = "child process wait callback returned a zero wake interval";
                    return false;
                }
                next_observation = now + decision.next_wake;
            }
            pollfd descriptor{pipe_fds[0], POLLIN, 0};
            auto wake_at = deadline;
            if (heartbeat && initial_wake >
                    std::chrono::steady_clock::duration::zero())
                wake_at = std::min(wake_at, next_observation);
            const auto before_poll = std::chrono::steady_clock::now();
            int poll_timeout{};
            if (wake_at > before_poll) {
                const auto remaining = wake_at - before_poll;
                const auto whole_milliseconds =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        remaining);
                auto milliseconds = whole_milliseconds.count();
                if (whole_milliseconds < remaining) ++milliseconds;
                poll_timeout = static_cast<int>(std::min<std::int64_t>(
                    milliseconds,
                    std::numeric_limits<int>::max()));
            }
            poll(&descriptor, 1, poll_timeout);
        }
    }
    while (true) {
        std::array<char, 4096U> buffer{};
        const ssize_t count = read(pipe_fds[0], buffer.data(), buffer.size());
        if (count <= 0) break;
        if (output.size() + static_cast<std::size_t>(count) > maximum_output) {
            close(pipe_fds[0]);
            if (failure_kind != nullptr)
                *failure_kind = ProcessFailureKind::output_limit;
            error = "child process output limit exceeded";
            return false;
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
            std::chrono::seconds(30), 4096U, output, status, error)) return false;
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
    std::string&) {
    static constexpr std::array<std::uint32_t, 64U> round_constants{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
        0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
        0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
        0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
    std::array<std::uint32_t, 8U> state{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    const auto rotate_right = [](std::uint32_t value, unsigned count) {
        return (value >> count) | (value << (32U - count));
    };
    const auto transform = [&](const unsigned char* block) {
        std::array<std::uint32_t, 64U> words{};
        for (std::size_t index = 0U; index < 16U; ++index) {
            const std::size_t offset = index * 4U;
            words[index] =
                (static_cast<std::uint32_t>(block[offset]) << 24U) |
                (static_cast<std::uint32_t>(block[offset + 1U]) << 16U) |
                (static_cast<std::uint32_t>(block[offset + 2U]) << 8U) |
                static_cast<std::uint32_t>(block[offset + 3U]);
        }
        for (std::size_t index = 16U; index < words.size(); ++index) {
            const std::uint32_t left = words[index - 15U];
            const std::uint32_t right = words[index - 2U];
            const std::uint32_t sigma0 =
                rotate_right(left, 7U) ^ rotate_right(left, 18U) ^ (left >> 3U);
            const std::uint32_t sigma1 =
                rotate_right(right, 17U) ^ rotate_right(right, 19U) ^ (right >> 10U);
            words[index] = words[index - 16U] + sigma0 +
                words[index - 7U] + sigma1;
        }
        std::uint32_t a = state[0];
        std::uint32_t b = state[1];
        std::uint32_t c = state[2];
        std::uint32_t d = state[3];
        std::uint32_t e = state[4];
        std::uint32_t f = state[5];
        std::uint32_t g = state[6];
        std::uint32_t h = state[7];
        for (std::size_t index = 0U; index < words.size(); ++index) {
            const std::uint32_t sum1 =
                rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
            const std::uint32_t choose = (e & f) ^ (~e & g);
            const std::uint32_t temporary1 =
                h + sum1 + choose + round_constants[index] + words[index];
            const std::uint32_t sum0 =
                rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temporary2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    };

    const auto* input =
        reinterpret_cast<const unsigned char*>(bytes.data());
    std::size_t consumed = 0U;
    while (bytes.size() - consumed >= 64U) {
        transform(input + consumed);
        consumed += 64U;
    }
    std::array<unsigned char, 128U> tail{};
    const std::size_t remaining = bytes.size() - consumed;
    if (remaining != 0U)
        std::copy_n(input + consumed, remaining, tail.begin());
    tail[remaining] = 0x80U;
    const std::size_t padded_size = remaining < 56U ? 64U : 128U;
    const std::uint64_t bit_length =
        static_cast<std::uint64_t>(bytes.size()) * 8U;
    for (unsigned index = 0U; index < 8U; ++index) {
        tail[padded_size - 1U - index] =
            static_cast<unsigned char>(bit_length >> (index * 8U));
    }
    transform(tail.data());
    if (padded_size == 128U) transform(tail.data() + 64U);

    static constexpr char hex[] = "0123456789abcdef";
    digest.clear();
    digest.reserve(64U);
    for (const std::uint32_t word : state) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            const auto byte = static_cast<unsigned char>(
                word >> static_cast<unsigned>(shift));
            digest.push_back(hex[byte >> 4U]);
            digest.push_back(hex[byte & 0x0fU]);
        }
    }
    return true;
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
    std::chrono::steady_clock::duration timeout,
    std::size_t maximum_bytes,
    HttpResponse& response,
    std::string& error,
    const HttpHeartbeat& heartbeat,
    std::chrono::steady_clock::duration initial_wake) {
    response.failure_kind = HttpFailureKind::none;
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
    ProcessFailureKind process_failure{};
    const auto timeout_nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(timeout);
    std::ostringstream timeout_text;
    timeout_text << std::fixed << std::setprecision(9)
                 << static_cast<double>(timeout_nanoseconds.count()) /
                        1'000'000'000.0;
    const std::string seconds = timeout_text.str();
    const std::string bytes = std::to_string(maximum_bytes);
    const bool ran = run_process(
        {"/usr/bin/curl", "--silent", "--show-error", "--request", "GET",
         "--proto", "=http,https", "--max-time", seconds, "--connect-timeout", seconds,
         "--max-filesize", bytes, "--output", body_template.data(), "--dump-header",
         header_template.data(), "--write-out", "%{http_code}\\n%{content_type}\\n%{url_effective}",
        url},
        timeout, maximum_diagnostic_bytes, output, status, error,
        heartbeat, initial_wake, &process_failure);
    std::string body;
    std::string headers;
    if (ran) {
        std::string read_error;
        if (!read_file(body_template.data(), maximum_bytes, body, read_error) ||
            !read_file(header_template.data(), 64U * 1024U, headers, read_error)) {
            response.failure_kind = HttpFailureKind::local_io;
            error = read_error;
        }
    }
    unlink(body_template.data()); unlink(header_template.data());
    if (!ran) {
        response.failure_kind =
            process_failure == ProcessFailureKind::timeout ?
                HttpFailureKind::timeout :
            process_failure == ProcessFailureKind::cancelled ?
                HttpFailureKind::cancelled :
                HttpFailureKind::local_io;
        return false;
    }
    if (!error.empty()) return false;
    if (status != 0) {
        switch (status) {
            case 5:
            case 6:
            case 7:
            case 18:
            case 52:
            case 55:
            case 56:
                response.failure_kind = HttpFailureKind::temporary_network;
                break;
            case 28:
                response.failure_kind = HttpFailureKind::timeout;
                break;
            case 35:
            case 51:
            case 58:
            case 59:
            case 60:
            case 77:
            case 80:
            case 90:
            case 91:
                response.failure_kind = HttpFailureKind::tls;
                break;
            case 63:
                response.failure_kind = HttpFailureKind::response_too_large;
                break;
            default:
                response.failure_kind = HttpFailureKind::other;
                break;
        }
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
    response.retry_after = header_value(headers, "retry-after");
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

bool json_count(
    const Json::Object& object,
    std::string_view key,
    std::uintmax_t& value);

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
    std::string& error,
    std::optional<std::size_t> committed_page_limit = std::nullopt) {
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
        if (iterator->is_symlink(ec) || ec)
            return (error = "raw page symbolic links are rejected", false);
        std::size_t number{};
        if (!parse_page_number(filename, number, error)) return false;
        if (committed_page_limit &&
            number > *committed_page_limit)
            continue;
        if (!numbered.emplace(number, iterator->path()).second)
            return (error = "duplicate legacy raw page number: " +
                std::to_string(number), false);
    }
    if (ec) return (error = "cannot enumerate legacy raw pages", false);
    if (committed_page_limit) {
        if (numbered.size() < *committed_page_limit)
            return (error = "partial bundle is missing a checkpointed raw page", false);
        if (*committed_page_limit == 0U) {
            state.completed_pages = 0U;
            state.records_received = 0U;
            return true;
        }
    } else if (numbered.empty()) {
        return (error = "legacy partial bundle contains no raw pages", false);
    }
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

bool checkpoint_head(
    const std::filesystem::path& path,
    std::uintmax_t& version,
    std::size_t& completed_pages,
    std::string& error) {
    std::error_code status_error;
    if (std::filesystem::is_symlink(
            std::filesystem::symlink_status(path, status_error)) ||
        status_error)
        return (error = "checkpoint must not be a symbolic link", false);
    std::string bytes;
    if (!read_file(path, 4U * 1024U * 1024U, bytes, error)) return false;
    Json root;
    JsonParser parser(bytes);
    if (!parser.parse(root, error)) return false;
    const Json::Object* object = object_value(root);
    std::uintmax_t pages{};
    if (object == nullptr ||
        !json_count(*object, "checkpoint_version", version) ||
        !json_count(*object, "completed_pages", pages) ||
        pages > std::numeric_limits<std::size_t>::max())
        return (error = "checkpoint head is invalid", false);
    if (version != 1U && version != 2U)
        return (error = "unsupported checkpoint version " +
            std::to_string(version), false);
    completed_pages = static_cast<std::size_t>(pages);
    return true;
}

std::string compact_checkpoint_json(
    const RegistryUrl& origin,
    std::size_t completed_pages,
    std::size_t completed_records,
    const std::optional<std::string>& next_cursor,
    const Artifact* last,
    const Artifact& pages_artifact) {
    std::string result = "{\"checkpoint_version\":2,\"completed_pages\":" +
        std::to_string(completed_pages) + ",\"completed_records\":" +
        std::to_string(completed_records) + ",\"last_completed_page\":" +
        std::to_string(completed_pages) + ",\"last_page_path\":";
    if (last != nullptr) append_json_string(result, last->path);
    else result += "null";
    result += ",\"last_page_sha256\":";
    if (last != nullptr) append_json_string(result, last->sha256);
    else result += "null";
    result += ",\"last_page_size\":" +
        std::to_string(last == nullptr ? 0U : last->size) +
        ",\"next_cursor\":";
    if (next_cursor) append_json_string(result, *next_cursor);
    else result += "null";
    result += ",\"pages_metadata_path\":\"raw/pages.jsonl\","
        "\"pages_metadata_sha256\":";
    append_json_string(result, pages_artifact.sha256);
    result += ",\"pages_metadata_size\":" +
        std::to_string(pages_artifact.size) + ",\"records_received\":" +
        std::to_string(completed_records) +
        ",\"registry\":\"official-mcp\",\"registry_base_url\":";
    append_json_string(result, origin.normalized);
    result += ",\"status\":\"partial\",\"updated_at\":";
    append_json_string(result, utc_now());
    result += "}\n";
    return result;
}

std::string checkpoint_json(
    const RegistryUrl& origin,
    const ReconstructedState& state,
    const Artifact& pages_artifact) {
    const Artifact* last =
        state.artifacts.empty() ? nullptr : &state.artifacts.back();
    return compact_checkpoint_json(
        origin, state.completed_pages, state.records_received,
        state.next_cursor, last, pages_artifact);
}

bool json_count(
    const Json::Object& object,
    std::string_view key,
    std::uintmax_t& value) {
    const auto found = object.find(std::string(key));
    if (found == object.end() || !found->second.number) return false;
    const auto* text = std::get_if<std::string>(&found->second.value);
    if (text == nullptr || text->empty() || text->front() == '-') return false;
    const auto parsed =
        std::from_chars(text->data(), text->data() + text->size(), value);
    return parsed.ec == std::errc{} &&
        parsed.ptr == text->data() + text->size();
}

bool checkpoint_v2_matches(
    const std::filesystem::path& path,
    const Json::Object& object,
    const RegistryUrl& origin,
    const ReconstructedState& state,
    std::string& error) {
    const std::string* base_url = string_member(object, "registry_base_url");
    const std::string* registry = string_member(object, "registry");
    const std::string* status = string_member(object, "status");
    const std::string* updated_at = string_member(object, "updated_at");
    const std::string* pages_path =
        string_member(object, "pages_metadata_path");
    const std::string* pages_digest =
        string_member(object, "pages_metadata_sha256");
    std::uintmax_t completed_pages{};
    std::uintmax_t completed_records{};
    std::uintmax_t last_completed_page{};
    std::uintmax_t last_page_size{};
    std::uintmax_t pages_size{};
    if (base_url == nullptr || *base_url != origin.normalized ||
        registry == nullptr || *registry != registry_name ||
        status == nullptr || *status != "partial" ||
        updated_at == nullptr || !valid_utc_timestamp(*updated_at) ||
        pages_path == nullptr || *pages_path != "raw/pages.jsonl" ||
        pages_digest == nullptr ||
        !json_count(object, "completed_pages", completed_pages) ||
        !json_count(object, "completed_records", completed_records) ||
        !json_count(object, "last_completed_page", last_completed_page) ||
        !json_count(object, "last_page_size", last_page_size) ||
        !json_count(object, "pages_metadata_size", pages_size))
        return (error = "invalid compact checkpoint fields", false);
    if (completed_pages != state.completed_pages ||
        completed_records != state.records_received ||
        last_completed_page != completed_pages)
        return (error = "compact checkpoint counts do not match raw pages", false);
    const std::string* cursor = string_member(object, "next_cursor");
    if ((state.next_cursor && (cursor == nullptr || *cursor != *state.next_cursor)) ||
        (!state.next_cursor && cursor != nullptr))
        return (error = "compact checkpoint next_cursor mismatch", false);

    const std::string* last_path = string_member(object, "last_page_path");
    const std::string* last_digest =
        string_member(object, "last_page_sha256");
    if (completed_pages == 0U) {
        if (last_path != nullptr || last_digest != nullptr ||
            last_page_size != 0U)
            return (error = "empty compact checkpoint has last-page metadata", false);
    } else {
        std::ostringstream expected_name;
        expected_name << "raw/page-" << std::setw(6) << std::setfill('0')
                      << completed_pages << ".json";
        if (last_path == nullptr || *last_path != expected_name.str() ||
            !validate_artifact_path(*last_path) || last_digest == nullptr)
            return (error = "invalid compact checkpoint last-page path", false);
        std::error_code status_error;
        if (std::filesystem::is_symlink(
                std::filesystem::symlink_status(
                    path.parent_path() / *last_path, status_error)) ||
            status_error)
            return (error = "compact checkpoint last page must not be a symbolic link", false);
        Artifact actual;
        if (!artifact_for(path.parent_path(), *last_path, actual, error))
            return false;
        if (actual.size != last_page_size ||
            actual.sha256 != *last_digest)
            return (error = "compact checkpoint last-page artifact mismatch", false);
    }

    Artifact pages_artifact;
    std::error_code status_error;
    if (std::filesystem::is_symlink(
            std::filesystem::symlink_status(
                path.parent_path() / "raw/pages.jsonl", status_error)) ||
        status_error)
        return (error = "checkpoint page metadata must not be a symbolic link", false);
    if (!artifact_for(
            path.parent_path(), "raw/pages.jsonl", pages_artifact, error))
        return false;
    if (pages_artifact.size != pages_size ||
        pages_artifact.sha256 != *pages_digest)
        return (error = "compact checkpoint pages metadata mismatch", false);
    std::string pages_bytes;
    if (!read_file(
            path.parent_path() / "raw/pages.jsonl",
            128U * 1024U * 1024U, pages_bytes, error))
        return false;
    std::istringstream lines(pages_bytes);
    std::string line;
    std::size_t line_count{};
    std::optional<std::string> final_output;
    while (std::getline(lines, line)) {
        ++line_count;
        Json metadata_json;
        JsonParser parser(line);
        if (!parser.parse(metadata_json, error))
            return (error = "invalid checkpoint page metadata", false);
        const Json::Object* metadata = object_value(metadata_json);
        std::uintmax_t page_number{};
        const std::string* raw_path =
            metadata == nullptr ? nullptr : string_member(*metadata, "path");
        if (metadata == nullptr ||
            !json_count(*metadata, "page_number", page_number) ||
            page_number != line_count || raw_path == nullptr)
            return (error = "checkpoint page metadata sequence mismatch", false);
        std::ostringstream expected_path;
        expected_path << "raw/page-" << std::setw(6) << std::setfill('0')
                      << line_count << ".json";
        if (*raw_path != expected_path.str())
            return (error = "checkpoint page metadata path mismatch", false);
        const auto output = metadata->find("pagination_output");
        if (output == metadata->end())
            return (error = "checkpoint page metadata cursor missing", false);
        if (std::holds_alternative<std::nullptr_t>(output->second.value)) {
            final_output.reset();
        } else {
            const auto* value = std::get_if<std::string>(&output->second.value);
            if (value == nullptr || output->second.number)
                return (error = "checkpoint page metadata cursor invalid", false);
            final_output = *value;
        }
    }
    if (line_count != completed_pages || final_output != state.next_cursor)
        return (error = "checkpoint page metadata does not end at durable head", false);
    return true;
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
    std::uintmax_t checkpoint_version{};
    if (!json_count(*object, "checkpoint_version", checkpoint_version))
        return (error = "checkpoint version is missing", false);
    if (checkpoint_version == 2U)
        return checkpoint_v2_matches(path, *object, origin, state, error);
    if (checkpoint_version != 1U)
        return (error = "unsupported checkpoint version " +
            std::to_string(checkpoint_version), false);
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

using SteadyTime = std::chrono::steady_clock::time_point;

struct CollectionDeadlines {
    SteadyTime last_durable_progress;
    std::optional<SteadyTime> total_deadline;
};

enum class DeadlineFailure {
    none,
    stalled,
    total
};

struct DeadlineStatus {
    DeadlineFailure failure{DeadlineFailure::none};
    std::chrono::steady_clock::duration since_last_durable_progress{};
    std::chrono::steady_clock::duration until_next_deadline{};
    std::optional<std::chrono::steady_clock::duration> remaining_total;
};

constexpr std::uint64_t maximum_request_timeout_seconds = 86'400U;
constexpr std::uint64_t maximum_stall_timeout_seconds = 2'592'000U;
constexpr std::uint64_t maximum_run_timeout_seconds = 31'536'000U;
constexpr std::uint64_t maximum_retry_delay_seconds = 86'400U;
constexpr std::size_t maximum_attempt_count = 1'000U;

bool validate_runtime_policy(
    const RegistryRuntimePolicy& policy,
    std::string& error) {
    const auto request = policy.request_timeout.count();
    const auto stall = policy.stall_timeout.count();
    const auto initial = policy.retry_initial.count();
    const auto maximum = policy.retry_maximum.count();
    if (request <= 0 || stall <= 0 || policy.maximum_attempts_per_page == 0U ||
        initial <= 0)
        return (error =
            "request timeout, stall timeout, attempts, and retry initial delay "
            "must be greater than zero", false);
    if (maximum < initial)
        return (error = "retry maximum delay must not be less than retry initial delay", false);
    if (static_cast<std::uint64_t>(request) > maximum_request_timeout_seconds ||
        static_cast<std::uint64_t>(stall) > maximum_stall_timeout_seconds ||
        static_cast<std::uint64_t>(maximum) > maximum_retry_delay_seconds ||
        policy.maximum_attempts_per_page > maximum_attempt_count)
        return (error = "runtime policy value exceeds the supported implementation bound", false);
    if (policy.run_timeout &&
        (policy.run_timeout->count() <= 0 ||
         static_cast<std::uint64_t>(policy.run_timeout->count()) >
             maximum_run_timeout_seconds))
        return (error = "total run timeout exceeds the supported implementation bound", false);
    return true;
}

std::chrono::milliseconds nonnegative_duration(
    SteadyTime later,
    SteadyTime earlier) {
    if (later <= earlier) return std::chrono::milliseconds::zero();
    return std::chrono::duration_cast<std::chrono::milliseconds>(later - earlier);
}

DeadlineStatus classify_deadlines(
    const CollectionDeadlines& deadlines,
    const RegistryRuntimePolicy& policy,
    SteadyTime now) {
    DeadlineStatus status;
    if (now > deadlines.last_durable_progress)
        status.since_last_durable_progress =
            now - deadlines.last_durable_progress;
    const auto stall_timeout =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            policy.stall_timeout);
    const auto remaining_stall =
        status.since_last_durable_progress >= stall_timeout ?
            std::chrono::steady_clock::duration::zero() :
            stall_timeout - status.since_last_durable_progress;
    status.until_next_deadline = remaining_stall;
    if (deadlines.total_deadline) {
        const auto remaining =
            now >= *deadlines.total_deadline ?
                std::chrono::steady_clock::duration::zero() :
                *deadlines.total_deadline - now;
        status.remaining_total = remaining;
        status.until_next_deadline =
            std::min(status.until_next_deadline, remaining);
        if (now >= *deadlines.total_deadline) {
            status.failure = DeadlineFailure::total;
            return status;
        }
    }
    if (status.since_last_durable_progress >= stall_timeout)
        status.failure = DeadlineFailure::stalled;
    return status;
}

bool retryable_http_status(unsigned status) noexcept {
    return status == 408U || status == 425U || status == 429U ||
        status == 500U || status == 502U || status == 503U ||
        status == 504U;
}

bool retryable_transport_failure(HttpFailureKind kind) noexcept {
    return kind == HttpFailureKind::timeout ||
        kind == HttpFailureKind::temporary_network;
}

std::string transport_failure_category(HttpFailureKind kind) {
    switch (kind) {
        case HttpFailureKind::timeout: return "request_timeout";
        case HttpFailureKind::temporary_network: return "temporary_network";
        case HttpFailureKind::tls: return "tls_failure";
        case HttpFailureKind::protocol: return "protocol_failure";
        case HttpFailureKind::cancelled: return "cancelled";
        case HttpFailureKind::response_too_large: return "oversized_response";
        case HttpFailureKind::local_io: return "local_io_failure";
        case HttpFailureKind::other: return "http_error";
        case HttpFailureKind::none: return "http_error";
    }
    return "http_error";
}

std::chrono::seconds exponential_retry_delay(
    const RegistryRuntimePolicy& policy,
    std::size_t failed_attempt) {
    std::uint64_t delay =
        static_cast<std::uint64_t>(policy.retry_initial.count());
    const std::uint64_t cap =
        static_cast<std::uint64_t>(policy.retry_maximum.count());
    for (std::size_t index = 1U; index < failed_attempt && delay < cap; ++index) {
        if (delay > cap / 2U) {
            delay = cap;
            break;
        }
        delay *= 2U;
    }
    return std::chrono::seconds(std::min(delay, cap));
}

std::optional<std::chrono::seconds> integer_retry_after(
    const HttpResponse& response,
    const RegistryRuntimePolicy& policy) {
    if ((response.status != 429U && response.status != 503U) ||
        !response.retry_after)
        return std::nullopt;
    const std::string_view text = *response.retry_after;
    if (text.empty() ||
        !std::all_of(text.begin(), text.end(), [](char c) {
            return c >= '0' && c <= '9';
        }))
        return std::nullopt;
    std::uint64_t seconds{};
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), seconds);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        return std::nullopt;
    return std::chrono::seconds(std::min<std::uint64_t>(
        seconds, static_cast<std::uint64_t>(policy.retry_maximum.count())));
}

bool persist_pages_metadata(
    const std::filesystem::path& bundle,
    std::string_view pages_jsonl,
    Artifact& artifact,
    std::string& error) {
    if (!atomic_replace_file(
            bundle / "raw/pages.jsonl", pages_jsonl, false, error))
        return false;
    return artifact_for(bundle, "raw/pages.jsonl", artifact, error);
}

bool persist_compact_checkpoint(
    const std::filesystem::path& bundle,
    const RegistryUrl& origin,
    std::size_t completed_pages,
    std::size_t completed_records,
    const std::optional<std::string>& next_cursor,
    const Artifact* last_page,
    const Artifact& pages_artifact,
    std::string& error) {
    return atomic_replace_file(
        bundle / "checkpoint.json",
        compact_checkpoint_json(
            origin, completed_pages, completed_records, next_cursor,
            last_page, pages_artifact),
        false, error);
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
    const auto now = options.now ? options.now : [] {
        return std::chrono::steady_clock::now();
    };
    const auto wait = options.wait ? options.wait :
        [](std::chrono::steady_clock::duration duration) {
        std::this_thread::sleep_for(duration);
    };
    const SteadyTime run_started = now();
    const auto elapsed = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            now() - run_started);
    };
    const auto preflight_fail = [&](std::string reason) {
        registry_progress(
            options.verbose,
            "failure stage=startup category=invalid_configuration page=1"
            " completed_pages=0 completed_records=0 elapsed=" +
                seconds_text(elapsed()) + " retained=none detail=" +
                progress_value(reason));
        message = std::move(reason);
        return false;
    };

    RegistryUrl origin;
    if (!parse_registry_url(options.registry_base_url, origin, message))
        return preflight_fail(message);
    if (options.output.empty())
        return preflight_fail("output bundle path is required");
    if (options.limits.maximum_pages == 0U ||
        options.limits.maximum_page_bytes == 0U ||
        options.limits.maximum_records == 0U)
        return preflight_fail(
            "page, byte, and record limits must be greater than zero");
    if (!validate_runtime_policy(options.runtime, message))
        return preflight_fail(message);

    std::error_code filesystem_error;
    if (std::filesystem::exists(options.output, filesystem_error))
        return preflight_fail(
            "destination already exists: " + options.output.string());
    const std::filesystem::path parent =
        options.output.parent_path().empty() ? "." : options.output.parent_path();
    if (!std::filesystem::exists(parent, filesystem_error))
        return preflight_fail("output parent directory does not exist");
    const std::filesystem::path temporary =
        parent / (options.output.filename().string() + ".partial-" +
                  random_run_id());
    if (!std::filesystem::create_directory(temporary, filesystem_error) ||
        filesystem_error)
        return preflight_fail("cannot create temporary bundle directory");
    for (const std::string_view directory :
         {"raw", "canonical", "diagnostics"})
        std::filesystem::create_directories(
            temporary / directory, filesystem_error);
    if (filesystem_error) {
        message = "cannot create temporary bundle subdirectories; failed bundle retained at " +
            temporary.string();
        return false;
    }

    if (!transport) transport = curl_transport;
    const auto heartbeat_interval = std::max(
        options.heartbeat_interval, std::chrono::milliseconds(1'000));
    const std::optional<SteadyTime> total_deadline =
        options.runtime.run_timeout ?
            std::optional<SteadyTime>(run_started + *options.runtime.run_timeout) :
            std::nullopt;
    std::optional<CollectionDeadlines> deadlines;
    std::vector<std::pair<std::string, std::string>> records;
    std::map<std::string, std::string> identities;
    std::set<std::string> cursors;
    std::vector<std::string> raw_paths;
    std::vector<Artifact> raw_artifacts;
    std::string pages_jsonl;
    std::optional<std::string> cursor;
    std::size_t pages{};
    std::size_t failure_page{1U};
    std::size_t failure_attempt{};
    std::string failure_stage{"startup"};
    std::string failure_category{"persistence_failure"};
    const std::string started_at = utc_now();
    const std::string run_id = random_run_id();

    auto fail = [&](std::string reason) {
        if (reason.size() > maximum_diagnostic_bytes)
            reason.resize(maximum_diagnostic_bytes);
        std::string diagnostic = "{\"error\":";
        append_json_string(diagnostic, reason);
        diagnostic += ",\"status\":\"failed\"}\n";
        std::string ignored;
        write_file(
            temporary / "diagnostics/errors.jsonl", diagnostic, ignored);
        std::string event =
            "failure stage=" + failure_stage + " category=" + failure_category +
            " page=" + std::to_string(failure_page);
        if (failure_attempt != 0U)
            event +=
                (failure_category == "retry_budget_exhausted" ?
                    " attempts=" : " attempt=") +
                std::to_string(failure_attempt);
        event += " completed_pages=" + std::to_string(pages) +
            " completed_records=" + std::to_string(records.size());
        if (deadlines)
            event += " seconds_since_last_completed_page=" +
                std::to_string(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        nonnegative_duration(
                            now(), deadlines->last_durable_progress)).count());
        event += " elapsed=" + seconds_text(elapsed());
        if (total_deadline)
            event += " remaining_total=" +
                seconds_text(nonnegative_duration(*total_deadline, now()));
        event += " retained=" + progress_value(temporary.string()) +
            " detail=" + progress_value(reason);
        registry_progress(options.verbose, event);
        message = reason + "; failed bundle retained at " + temporary.string();
        return false;
    };
    const auto fail_deadline = [&](const DeadlineStatus& status) {
        if (status.failure == DeadlineFailure::stalled) {
            failure_stage = "progress_watchdog";
            failure_category = "collection_stalled";
            return fail(
                "no registry page was durably committed before the stall deadline");
        }
        if (status.failure == DeadlineFailure::total) {
            failure_stage = "runtime_policy";
            failure_category = "total_run_deadline_exhausted";
            return fail("total run deadline exhausted");
        }
        failure_stage = "runtime_policy";
        failure_category = "deadline_state_error";
        return fail("deadline wait reached zero without an expired deadline");
    };
    const auto deadline_status = [&] {
        if (deadlines)
            return classify_deadlines(*deadlines, options.runtime, now());
        const auto observed_at = now();
        return classify_deadlines(
            CollectionDeadlines{observed_at, total_deadline},
            options.runtime, observed_at);
    };

    registry_progress(
        options.verbose,
        "mode=" + std::string(options.resume ? "resume" : "fresh") +
            " registry=" + progress_value(origin.normalized) +
            " output=" + progress_value(options.output.string()) +
            (options.resume ?
                " resume_source=" + progress_value(options.resume->string()) :
                ""));
    registry_progress(
        options.verbose,
        "request_timeout=" +
            std::to_string(options.runtime.request_timeout.count()) +
            "s stall_timeout=" +
            std::to_string(options.runtime.stall_timeout.count()) +
            "s run_timeout=" +
            (options.runtime.run_timeout ?
                std::to_string(options.runtime.run_timeout->count()) + "s" :
                "unlimited") +
            " maximum_attempts_per_page=" +
            std::to_string(options.runtime.maximum_attempts_per_page) +
            " retry_initial=" +
            std::to_string(options.runtime.retry_initial.count()) +
            "s retry_maximum=" +
            std::to_string(options.runtime.retry_maximum.count()) + "s");

    Artifact pages_artifact;
    std::string error;
    if (!persist_pages_metadata(temporary, "", pages_artifact, error) ||
        !persist_compact_checkpoint(
            temporary, origin, 0U, 0U, std::nullopt, nullptr,
            pages_artifact, error))
        return fail("cannot initialize compact checkpoint: " + error);

    if (options.resume) {
        failure_stage = "checkpoint_validation";
        failure_category = "checkpoint_validation_failure";
        registry_progress(
            options.verbose, "resume checkpoint_validation_start");
        const auto& source = *options.resume;
        if (!std::filesystem::is_directory(source, filesystem_error) ||
            filesystem_error)
            return fail("resume partial directory does not exist");
        if (std::filesystem::exists(source / "_SUCCESS", filesystem_error))
            return fail("cannot resume a completed bundle");
        if (!std::filesystem::exists(
                source / "checkpoint.json", filesystem_error)) {
            registry_progress(
                options.verbose, "resume checkpoint_reconstruct_start");
            std::string reconstruction;
            if (!reconstruct_registry_checkpoint(
                    source, origin.normalized, options.limits, reconstruction))
                return fail(
                    "cannot reconstruct resume checkpoint: " + reconstruction);
            registry_progress(
                options.verbose, "resume checkpoint_reconstruct_complete");
        }
        std::uintmax_t checkpoint_version{};
        std::size_t committed_pages{};
        if (!checkpoint_head(
                source / "checkpoint.json", checkpoint_version,
                committed_pages, error))
            return fail("invalid resume checkpoint: " + error);
        ReconstructedState state;
        if (!reconstruct_state(
                source, origin, options.limits, state, error,
                checkpoint_version == 2U ?
                    std::optional<std::size_t>(committed_pages) :
                    std::nullopt))
            return fail("invalid resume bundle: " + error);
        if (!checkpoint_matches(
                source / "checkpoint.json", origin, state, error))
            return fail("invalid resume checkpoint: " + error);
        if (checkpoint_version == 2U &&
            !read_file(
                source / "raw/pages.jsonl", 128U * 1024U * 1024U,
                state.pages_jsonl, error))
            return fail("cannot read resume page metadata: " + error);
        registry_progress(
            options.verbose,
            "resume checkpoint_validation_complete completed_pages=" +
                std::to_string(state.completed_pages) +
                " completed_records=" +
                std::to_string(state.records_received) +
                " next_page=" +
                std::to_string(state.completed_pages + 1U));
        failure_stage = "resume_copy";
        failure_category = "persistence_failure";
        for (const std::string& relative : state.raw_paths) {
            std::string body;
            if (!read_file(
                    source / relative, options.limits.maximum_page_bytes,
                    body, error) ||
                !atomic_replace_file(
                    temporary / relative, body, true, error))
                return fail("cannot copy resume page: " + error);
        }
        records = std::move(state.records);
        cursors = std::move(state.cursors);
        raw_paths = std::move(state.raw_paths);
        raw_artifacts = std::move(state.artifacts);
        pages_jsonl = std::move(state.pages_jsonl);
        cursor = std::move(state.next_cursor);
        pages = state.completed_pages;
        if (!persist_pages_metadata(
                temporary, pages_jsonl, pages_artifact, error) ||
            !persist_compact_checkpoint(
                temporary, origin, pages, records.size(), cursor,
                raw_artifacts.empty() ? nullptr : &raw_artifacts.back(),
                pages_artifact, error))
            return fail("cannot persist resumed durable head: " + error);
        for (const auto& [identity, canonical] : records) {
            const auto [position, inserted] =
                identities.emplace(identity, canonical);
            if (!inserted && position->second != canonical)
                return fail(
                    "resume pages contain conflicting canonical identity");
        }
    }

    bool collection_complete = pages != 0U && !cursor;
    registry_progress(
        options.verbose,
        "completed_pages=" + std::to_string(pages) +
            " completed_records=" + std::to_string(records.size()) +
            " next_page=" + std::to_string(pages + 1U) + " " +
            cursor_progress(cursor));
    const SteadyTime pagination_started = now();
    if (!collection_complete)
        deadlines = CollectionDeadlines{now(), total_deadline};

    while (!collection_complete) {
        failure_page = pages + 1U;
        failure_attempt = 1U;
        if (pages >= options.limits.maximum_pages) {
            failure_stage = "response_validation";
            failure_category = "configured_page_limit_exceeded";
            return fail("maximum page count exceeded");
        }
        if (const auto status = deadline_status();
            status.failure != DeadlineFailure::none)
            return fail_deadline(status);
        const std::string request_url = registry_api_url(
            origin, cursor ?
                std::optional<std::string_view>(*cursor) : std::nullopt);
        const SteadyTime page_started = now();
        HttpResponse response;
        std::size_t redirects{};
        bool response_accepted{};

        for (std::size_t attempt = 1U;
             attempt <= options.runtime.maximum_attempts_per_page; ++attempt) {
            failure_attempt = attempt;
            std::string current_url = request_url;
            redirects = 0U;
            bool retryable{};
            std::string retry_category;
            std::string transport_error;
            while (true) {
                const auto request_deadlines = deadline_status();
                if (request_deadlines.failure != DeadlineFailure::none)
                    return fail_deadline(request_deadlines);
                auto request_timeout =
                    std::chrono::duration_cast<
                        std::chrono::steady_clock::duration>(
                        options.runtime.request_timeout);
                if (request_deadlines.remaining_total)
                    request_timeout = std::min(
                        request_timeout,
                        *request_deadlines.remaining_total);
                std::string request_event =
                    "page=" + std::to_string(failure_page) +
                    " attempt=" + std::to_string(attempt) + "/" +
                    std::to_string(
                        options.runtime.maximum_attempts_per_page) +
                    " request_start elapsed=" + seconds_text(elapsed());
                if (total_deadline)
                    request_event += " remaining_total=" +
                        seconds_text(
                            std::chrono::duration_cast<
                                std::chrono::milliseconds>(
                                *request_deadlines.remaining_total));
                request_event += " request_timeout=" +
                    seconds_text(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            request_timeout)) +
                    " since_last_page=" +
                    seconds_text(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            request_deadlines
                                .since_last_durable_progress)) +
                    " " + cursor_progress(cursor);
                registry_progress(options.verbose, request_event);
                const HttpHeartbeat heartbeat =
                    [&](std::chrono::milliseconds request_wait) {
                        const auto heartbeat_deadlines =
                            deadline_status();
                        if (heartbeat_deadlines.failure !=
                            DeadlineFailure::none)
                            return HttpWaitDecision{
                                false,
                                std::chrono::steady_clock::duration::zero()};
                        registry_progress(
                            options.verbose,
                            "heartbeat state=request_wait page=" +
                                std::to_string(failure_page) +
                                " attempt=" + std::to_string(attempt) + "/" +
                                std::to_string(
                                    options.runtime.maximum_attempts_per_page) +
                                " request_wait=" +
                                std::to_string(
                                    std::chrono::duration_cast<
                                        std::chrono::seconds>(
                                        request_wait).count()) +
                                "s since_last_page=" +
                                std::to_string(
                                    std::chrono::duration_cast<
                                        std::chrono::seconds>(
                                        heartbeat_deadlines
                                            .since_last_durable_progress)
                                        .count()) +
                                "s completed_pages=" +
                                std::to_string(pages) +
                                " completed_records=" +
                                std::to_string(records.size()) +
                                " elapsed=" +
                                std::to_string(
                                    std::chrono::duration_cast<
                                        std::chrono::seconds>(
                                        elapsed()).count()) + "s");
                        return HttpWaitDecision{
                            true,
                            std::min(
                                std::chrono::duration_cast<
                                    std::chrono::steady_clock::duration>(
                                    heartbeat_interval),
                                heartbeat_deadlines.until_next_deadline)};
                    };
                const auto request_heartbeat_interval = std::min(
                    std::chrono::duration_cast<
                        std::chrono::steady_clock::duration>(
                        heartbeat_interval),
                    request_deadlines.until_next_deadline);
                if (request_heartbeat_interval <=
                    std::chrono::steady_clock::duration::zero())
                    return fail_deadline(deadline_status());
                failure_stage = "http_request";
                response = {};
                if (!transport(
                        current_url,
                        request_timeout,
                        options.limits.maximum_page_bytes, response,
                        transport_error, heartbeat,
                        request_heartbeat_interval)) {
                    if (const auto status = deadline_status();
                        status.failure != DeadlineFailure::none)
                        return fail_deadline(status);
                    retryable =
                        retryable_transport_failure(response.failure_kind);
                    retry_category =
                        transport_failure_category(response.failure_kind);
                    if (!retryable) {
                        failure_category = retry_category;
                        return fail("HTTP request failed: " + transport_error);
                    }
                    break;
                }
                RegistryUrl effective;
                const std::size_t query = response.effective_url.find('?');
                failure_stage = "response_validation";
                failure_category = "malformed_response";
                if (!parse_registry_url(
                        std::string_view(response.effective_url).substr(
                            0U, query),
                        effective, transport_error) ||
                    !registry_same_origin(origin, effective))
                    return fail(
                        "HTTP response effective URL is outside configured origin");
                if (query != std::string::npos)
                    effective.normalized +=
                        response.effective_url.substr(query);
                response.effective_url = effective.normalized;
                if (response.status < 300U || response.status >= 400U) {
                    retryable = retryable_http_status(response.status);
                    retry_category =
                        "http_" + std::to_string(response.status);
                    break;
                }
                if (!response.location) {
                    failure_category = "redirect_policy_failure";
                    return fail("redirect response is missing Location");
                }
                if (redirects >= options.limits.maximum_redirects) {
                    failure_category = "redirect_policy_failure";
                    return fail("maximum redirect count exceeded");
                }
                if (!resolve_redirect(
                        origin, current_url, *response.location,
                        current_url, transport_error)) {
                    failure_category = "redirect_policy_failure";
                    return fail(transport_error);
                }
                ++redirects;
            }
            if (!retryable) {
                if (response.status != 200U) {
                    failure_category = "http_error";
                    return fail(
                        "registry returned HTTP " +
                        std::to_string(response.status));
                }
                response_accepted = true;
                break;
            }
            if (attempt == options.runtime.maximum_attempts_per_page) {
                failure_stage = "http_request";
                failure_category = "retry_budget_exhausted";
                return fail(
                    "retry budget exhausted after " +
                    std::to_string(attempt) + " attempts");
            }
            const auto retry_delay =
                integer_retry_after(response, options.runtime).value_or(
                    exponential_retry_delay(options.runtime, attempt));
            registry_progress(
                options.verbose,
                "page=" + std::to_string(failure_page) +
                    " attempt=" + std::to_string(attempt) + "/" +
                    std::to_string(
                        options.runtime.maximum_attempts_per_page) +
                    (response.status != 0U ?
                        " retryable_http_status=" +
                            std::to_string(response.status) :
                        " retryable_failure category=" + retry_category) +
                    " retry_in=" +
                    std::to_string(retry_delay.count()) + "s");
            auto retry_remaining =
                std::chrono::duration_cast<
                    std::chrono::steady_clock::duration>(
                    retry_delay);
            while (retry_remaining >
                   std::chrono::steady_clock::duration::zero()) {
                const auto before_wait = deadline_status();
                if (before_wait.failure != DeadlineFailure::none)
                    return fail_deadline(before_wait);
                auto chunk = std::min(
                    retry_remaining,
                    std::chrono::duration_cast<
                        std::chrono::steady_clock::duration>(
                        heartbeat_interval));
                chunk = std::min(
                    chunk, before_wait.until_next_deadline);
                if (chunk <=
                    std::chrono::steady_clock::duration::zero())
                    return fail_deadline(deadline_status());
                wait(chunk);
                retry_remaining -= chunk;
                const auto after_wait = deadline_status();
                if (after_wait.failure != DeadlineFailure::none)
                    return fail_deadline(after_wait);
                registry_progress(
                    options.verbose,
                    "heartbeat state=retry_backoff page=" +
                        std::to_string(failure_page) +
                        " next_attempt=" +
                        std::to_string(attempt + 1U) + "/" +
                        std::to_string(
                            options.runtime.maximum_attempts_per_page) +
                        " retry_remaining=" +
                        std::to_string(
                            std::chrono::duration_cast<std::chrono::seconds>(
                                std::max(
                                    retry_remaining,
                                    std::chrono::steady_clock::duration::zero()))
                                .count()) +
                        "s since_last_page=" +
                        std::to_string(
                            std::chrono::duration_cast<std::chrono::seconds>(
                                after_wait.since_last_durable_progress)
                                .count()) +
                        "s completed_pages=" + std::to_string(pages) +
                        " completed_records=" +
                        std::to_string(records.size()) +
                        " elapsed=" +
                        std::to_string(
                            std::chrono::duration_cast<std::chrono::seconds>(
                                elapsed()).count()) + "s");
            }
            if (const auto status = deadline_status();
                status.failure != DeadlineFailure::none)
                return fail_deadline(status);
        }
        if (!response_accepted) {
            failure_stage = "http_request";
            failure_category = "retry_budget_exhausted";
            return fail("retry budget exhausted");
        }

        failure_stage = "response_validation";
        failure_category = "malformed_response";
        if (response.body.size() > options.limits.maximum_page_bytes)
            return fail("registry page exceeds configured byte limit");
        if (!lower_ascii(response.content_type).starts_with(
                "application/json"))
            return fail(
                "registry response content type is not application/json");
        std::vector<std::pair<std::string, std::string>> page_records;
        std::optional<std::string> next_cursor;
        if (!parse_page(
                response.body, temporary, page_records, next_cursor, error))
            return fail("invalid registry JSON: " + error);
        const std::size_t page_record_count = page_records.size();
        if (page_records.size() >
            options.limits.maximum_records - records.size()) {
            failure_category = "configured_record_limit_exceeded";
            return fail("maximum record count exceeded");
        }
        if (next_cursor && cursors.contains(*next_cursor))
            return fail("repeated pagination cursor detected");
        std::map<std::string, std::string> page_identities;
        for (const auto& [identity, canonical] : page_records) {
            const auto committed = identities.find(identity);
            if (committed != identities.end() &&
                committed->second != canonical)
                return fail(
                    "duplicate canonical identity has conflicting content");
            const auto [position, inserted] =
                page_identities.emplace(identity, canonical);
            if (!inserted && position->second != canonical)
                return fail(
                    "duplicate canonical identity has conflicting content");
        }
        if (const auto status = deadline_status();
            status.failure != DeadlineFailure::none)
            return fail_deadline(status);

        const std::size_t page_number = pages + 1U;
        std::ostringstream name;
        name << "raw/page-" << std::setw(6) << std::setfill('0')
             << page_number << ".json";
        const std::string raw_path = name.str();
        failure_stage = "page_persistence";
        failure_category = "persistence_failure";
        if (!atomic_replace_file(
                temporary / raw_path, response.body, true, error))
            return fail(error);
        Artifact raw_artifact;
        if (!artifact_for(
                temporary, raw_path, raw_artifact, error))
            return fail(error);
        std::string metadata = "{\"content_type\":";
        append_json_string(metadata, response.content_type);
        metadata += ",\"effective_response_url\":";
        append_json_string(metadata, response.effective_url);
        metadata += ",\"http_status\":" +
            std::to_string(response.status) +
            ",\"page_number\":" + std::to_string(page_number) +
            ",\"pagination_input\":";
        if (cursor) append_json_string(metadata, *cursor);
        else metadata += "null";
        metadata += ",\"pagination_output\":";
        if (next_cursor) append_json_string(metadata, *next_cursor);
        else metadata += "null";
        metadata += ",\"path\":";
        append_json_string(metadata, raw_path);
        metadata += ",\"records\":" +
            std::to_string(page_records.size()) +
            ",\"redirect_count\":" + std::to_string(redirects) +
            ",\"request_url\":";
        append_json_string(metadata, request_url);
        metadata += ",\"response_bytes\":" +
            std::to_string(response.body.size()) +
            ",\"retrieved_at\":";
        append_json_string(metadata, utc_now());
        metadata += ",\"sha256\":";
        append_json_string(metadata, raw_artifact.sha256);
        metadata += "}\n";

        const std::string previous_metadata = pages_jsonl;
        const std::string candidate_metadata = pages_jsonl + metadata;
        failure_stage = "checkpoint_persistence";
        failure_category = "checkpoint_persistence_failure";
        if (!persist_pages_metadata(
                temporary, candidate_metadata, pages_artifact, error))
            return fail(error);
        if (!persist_compact_checkpoint(
                temporary, origin, page_number,
                records.size() + page_records.size(), next_cursor,
                &raw_artifact, pages_artifact, error)) {
            Artifact ignored_artifact;
            std::string ignored_error;
            persist_pages_metadata(
                temporary, previous_metadata,
                ignored_artifact, ignored_error);
            return fail("cannot persist compact checkpoint: " + error);
        }

        pages = page_number;
        pages_jsonl = candidate_metadata;
        raw_paths.push_back(raw_path);
        raw_artifacts.push_back(raw_artifact);
        records.insert(
            records.end(),
            std::make_move_iterator(page_records.begin()),
            std::make_move_iterator(page_records.end()));
        for (auto& [identity, canonical] : page_identities)
            identities.emplace(
                std::move(identity), std::move(canonical));
        if (next_cursor) cursors.insert(*next_cursor);
        cursor = next_cursor;
        deadlines->last_durable_progress = now();
        registry_progress(
            options.verbose,
            "page=" + std::to_string(pages) +
                " status=" + std::to_string(response.status) +
                " bytes=" + std::to_string(response.body.size()) +
                " duration=" +
                seconds_text(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now() - page_started)) +
                " records=" + std::to_string(page_record_count) +
                " total_records=" + std::to_string(records.size()) +
                " completed_pages=" + std::to_string(pages) +
                " next_cursor=" +
                std::string(next_cursor ? "yes" : "no") +
                " saved=" + raw_path + " checkpoint=committed");
        collection_complete = !next_cursor;
    }

    const SteadyTime pagination_finished = now();
    registry_progress(
        options.verbose,
        "timing phase=pagination duration=" +
            seconds_text(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    pagination_finished - pagination_started)) +
            " pages=" + std::to_string(pages) +
            " records=" + std::to_string(records.size()));
    if (const auto status = deadline_status();
        status.failure == DeadlineFailure::total)
        return fail_deadline(status);

    failure_attempt = 0U;
    failure_stage = "canonicalization";
    failure_category = "canonicalization_failure";
    const SteadyTime canonicalization_started = now();
    std::sort(
        records.begin(), records.end(),
        [](const auto& left, const auto& right) {
            return left.first < right.first ||
                (left.first == right.first &&
                 left.second < right.second);
        });
    std::string canonical;
    std::size_t unique{};
    for (std::size_t index = 0U; index < records.size();) {
        std::size_t end = index + 1U;
        while (end < records.size() &&
               records[end].first == records[index].first) {
            if (records[end].second != records[index].second)
                return fail(
                    "duplicate canonical identity has conflicting content");
            ++end;
        }
        canonical += records[index].second;
        ++unique;
        index = end;
    }
    registry_progress(
        options.verbose,
        "timing phase=canonicalization duration=" +
            seconds_text(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now() - canonicalization_started)) +
            " records=" + std::to_string(records.size()));
    if (const auto status = deadline_status();
        status.failure == DeadlineFailure::total)
        return fail_deadline(status);

    failure_stage = "artifact_persistence";
    failure_category = "persistence_failure";
    if (!write_file(
            temporary / "canonical/servers.jsonl", canonical, error) ||
        !write_file(
            temporary / "diagnostics/errors.jsonl", "", error))
        return fail(error);
    const SteadyTime manifest_started = now();
    failure_stage = "manifest_generation";
    std::vector<Artifact> artifacts;
    for (const std::string& raw : raw_paths) {
        Artifact artifact;
        if (!artifact_for(temporary, raw, artifact, error))
            return fail(error);
        artifacts.push_back(std::move(artifact));
    }
    for (const std::string_view relative :
         {"raw/pages.jsonl", "canonical/servers.jsonl",
          "diagnostics/errors.jsonl"}) {
        Artifact artifact;
        if (!artifact_for(
                temporary, std::string(relative), artifact, error))
            return fail(error);
        artifacts.push_back(std::move(artifact));
    }
    std::string snapshot_sha;
    if (!sha256_file(
            temporary / "canonical/servers.jsonl", snapshot_sha, error))
        return fail(error);
    std::string manifest = "{\"artifacts\":[";
    for (std::size_t index = 0U; index < artifacts.size(); ++index) {
        if (index != 0U) manifest.push_back(',');
        manifest += artifact_json(artifacts[index]);
    }
    manifest += "],\"bundle_version\":1,\"collector\":{\"git_commit\":";
    append_json_string(manifest, MCPO_GIT_COMMIT);
    manifest += ",\"name\":\"mcp-observatory\",\"version\":";
    append_json_string(manifest, MCPO_VERSION);
    manifest += "},\"completed_at\":";
    append_json_string(manifest, utc_now());
    manifest += ",\"counts\":{\"pages\":" + std::to_string(pages) +
        ",\"records_received\":" + std::to_string(records.size()) +
        ",\"unique_server_versions\":" + std::to_string(unique) +
        "},\"limits\":{\"maximum_attempts_per_page\":" +
        std::to_string(options.runtime.maximum_attempts_per_page) +
        ",\"maximum_page_bytes\":" +
        std::to_string(options.limits.maximum_page_bytes) +
        ",\"maximum_pages\":" +
        std::to_string(options.limits.maximum_pages) +
        ",\"maximum_records\":" +
        std::to_string(options.limits.maximum_records) +
        ",\"maximum_redirects\":" +
        std::to_string(options.limits.maximum_redirects) +
        ",\"request_timeout_seconds\":" +
        std::to_string(options.runtime.request_timeout.count()) +
        ",\"retry_initial_seconds\":" +
        std::to_string(options.runtime.retry_initial.count()) +
        ",\"retry_maximum_seconds\":" +
        std::to_string(options.runtime.retry_maximum.count()) +
        ",\"run_timeout_seconds\":" +
        std::to_string(
            options.runtime.run_timeout ?
                options.runtime.run_timeout->count() : 0) +
        ",\"stall_timeout_seconds\":" +
        std::to_string(options.runtime.stall_timeout.count()) +
        "},\"registry\":\"official-mcp\",\"registry_base_url\":";
    append_json_string(manifest, origin.normalized);
    manifest += ",\"run_id\":";
    append_json_string(manifest, run_id);
    manifest += ",\"snapshot_sha256\":";
    append_json_string(manifest, snapshot_sha);
    manifest += ",\"started_at\":";
    append_json_string(manifest, started_at);
    manifest += ",\"status\":\"complete\"}\n";
    if (!write_file(temporary / "manifest.json", manifest, error))
        return fail(error);
    registry_progress(
        options.verbose,
        "timing phase=manifest_generation duration=" +
            seconds_text(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                now() - manifest_started)) +
            " artifacts=" + std::to_string(artifacts.size()));
    if (const auto status = deadline_status();
        status.failure == DeadlineFailure::total)
        return fail_deadline(status);

    const SteadyTime validation_started = now();
    failure_stage = "final_validation";
    failure_category = "final_validation_failure";
    std::string validation;
    if (!validate_bundle_impl(temporary, false, validation))
        return fail("bundle self-validation failed: " + validation);
    registry_progress(
        options.verbose,
        "timing phase=final_validation duration=" +
            seconds_text(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now() - validation_started)) +
            " artifacts=" + std::to_string(artifacts.size()));
    if (const auto status = deadline_status();
        status.failure == DeadlineFailure::total)
        return fail_deadline(status);

    failure_stage = "success_marker";
    failure_category = "persistence_failure";
    if (!write_file(temporary / "_SUCCESS", "", error) ||
        !sync_directory(temporary / "raw", error) ||
        !sync_directory(temporary / "canonical", error) ||
        !sync_directory(temporary / "diagnostics", error) ||
        !sync_directory(temporary, error))
        return fail(error);
    const SteadyTime promotion_started = now();
    failure_stage = "atomic_promotion";
    if (!promote_without_replace(temporary, options.output, error))
        return fail(error);
    if (!sync_directory(parent, error)) {
        message =
            "bundle promoted but parent-directory flush failed: " + error;
        return false;
    }
    registry_progress(
        options.verbose,
        "timing phase=atomic_promotion duration=" +
            seconds_text(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now() - promotion_started)));
    registry_progress(
        options.verbose,
        "timing phase=total duration=" + seconds_text(elapsed()) +
            " pages=" + std::to_string(pages) +
            " records=" + std::to_string(records.size()));
    registry_progress(
        options.verbose,
        "success output=" + progress_value(options.output.string()) +
            " pages=" + std::to_string(pages) +
            " records=" + std::to_string(records.size()) +
            " unique_versions=" + std::to_string(unique));
    message = "registry collection complete: pages=" +
        std::to_string(pages) + " records=" +
        std::to_string(records.size()) + " unique=" +
        std::to_string(unique) + " snapshot_sha256=" + snapshot_sha;
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
    const std::filesystem::path canonical_path =
        bundle / "canonical/servers.jsonl";
    std::error_code canonical_ec;
    const auto canonical_size =
        std::filesystem::file_size(canonical_path, canonical_ec);
    if (canonical_ec || canonical_size > 512U * 1024U * 1024U)
        return (message = "invalid or oversized file: " +
            canonical_path.string(), false);
    std::string actual_snapshot;
    if (!sha256_file(canonical_path, actual_snapshot, error) ||
        actual_snapshot != *snapshot)
        return (message = "snapshot SHA-256 mismatch", false);
    std::string previous_identity;
    std::size_t canonical_count = 0U;
    std::ifstream lines(canonical_path, std::ios::binary);
    if (!lines) return (message = "cannot open " + canonical_path.string(), false);
    std::string line;
    while (std::getline(lines, line)) {
        ++canonical_count;
        if (line.empty())
            return (message = "empty canonical JSONL record at line " +
                std::to_string(canonical_count), false);
        if (line.size() > 8U * 1024U * 1024U)
            return (message = "oversized canonical JSONL record at line " +
                std::to_string(canonical_count), false);
        Json record_json;
        JsonParser record_parser(line);
        if (!record_parser.parse(record_json, error))
            return (message = "invalid canonical JSONL at line " +
                std::to_string(canonical_count) + ": " + error, false);
        std::string normalized;
        canonical_json(record_json, normalized);
        if (normalized != line)
            return (message = "canonical JSONL record is not normalized at line " +
                std::to_string(canonical_count), false);
        const Json::Object* record = object_value(record_json);
        if (record == nullptr)
            return (message = "canonical record must be an object at line " +
                std::to_string(canonical_count), false);
        const std::string* name = string_member(*record, "server_identifier");
        const std::string* version = string_member(*record, "server_version");
        const std::string* observed = string_member(*record, "observed_at");
        const std::string* declared_record_hash = string_member(*record, "canonical_sha256");
        if (name == nullptr || version == nullptr || observed == nullptr ||
            declared_record_hash == nullptr || !valid_utc_timestamp(*observed))
            return (message = "invalid canonical identity, timestamp, or hash at line " +
                std::to_string(canonical_count), false);
        Json::Object logical = *record;
        logical.erase("observed_at");
        logical.erase("canonical_sha256");
        Json logical_json{std::move(logical), false};
        std::string logical_bytes;
        canonical_json(logical_json, logical_bytes);
        std::string actual_record_hash;
        if (!sha256_bytes(bundle, logical_bytes, actual_record_hash, error) ||
            actual_record_hash != *declared_record_hash)
            return (message = "canonical record content hash mismatch at line " +
                std::to_string(canonical_count), false);
        const std::string identity = *name + "\n" + *version;
        if (!previous_identity.empty() && identity <= previous_identity)
            return (message = "canonical records are not strictly ordered at line " +
                std::to_string(canonical_count), false);
        previous_identity = identity;
    }
    if (lines.bad())
        return (message = "cannot read " + canonical_path.string(), false);
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
    const std::filesystem::path page_index_path = bundle / "raw/pages.jsonl";
    std::error_code page_index_ec;
    const auto page_index_size =
        std::filesystem::file_size(page_index_path, page_index_ec);
    if (page_index_ec || page_index_size > 128U * 1024U * 1024U)
        return (message = "invalid or oversized file: " +
            page_index_path.string(), false);
    std::ifstream page_lines(page_index_path, std::ios::binary);
    if (!page_lines)
        return (message = "cannot open " + page_index_path.string(), false);
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
    if (page_lines.bad())
        return (message = "cannot read " + page_index_path.string(), false);
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

namespace {

bool json_size_member(
    const Json::Object& object,
    std::string_view key,
    std::size_t& value) {
    const auto found = object.find(std::string(key));
    if (found == object.end() || !found->second.number) return false;
    const auto* text = std::get_if<std::string>(&found->second.value);
    if (text == nullptr || text->empty() || text->front() == '-') return false;
    const auto parsed = std::from_chars(text->data(), text->data() + text->size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text->data() + text->size();
}

bool optional_string_member(
    const Json::Object& object,
    std::string_view key,
    std::optional<std::string>& output,
    std::string& error) {
    const auto found = object.find(std::string(key));
    if (found == object.end() ||
        std::holds_alternative<std::nullptr_t>(found->second.value)) {
        output.reset();
        return true;
    }
    if (found->second.number) {
        error = std::string(key) + " must be a string or null";
        return false;
    }
    const auto* value = std::get_if<std::string>(&found->second.value);
    if (value == nullptr) {
        error = std::string(key) + " must be a string or null";
        return false;
    }
    output = *value;
    return true;
}

bool required_nonempty_string(
    const Json::Object& object,
    std::string_view key,
    std::string& output,
    std::string& error) {
    const std::string* value = string_member(object, key);
    if (value == nullptr || value->empty()) {
        error = std::string(key) + " must be a non-empty string";
        return false;
    }
    output = *value;
    return true;
}

bool required_string(
    const Json::Object& object,
    std::string_view key,
    std::string& output,
    std::string& error) {
    const std::string* value = string_member(object, key);
    if (value == nullptr) {
        error = std::string(key) + " must be a string";
        return false;
    }
    output = *value;
    return true;
}

bool optional_bool_member(
    const Json::Object& object,
    std::string_view key,
    bool& output,
    std::string& error) {
    const auto found = object.find(std::string(key));
    if (found == object.end()) {
        output = false;
        return true;
    }
    const auto* value = std::get_if<bool>(&found->second.value);
    if (value == nullptr) {
        error = std::string(key) + " must be a boolean";
        return false;
    }
    output = *value;
    return true;
}

bool parse_transport(
    const Json::Object& parent,
    std::string& transport,
    std::string& error) {
    const Json::Object* object = object_member(parent, "transport");
    if (object == nullptr ||
        !required_nonempty_string(*object, "type", transport, error)) {
        if (error.empty()) error = "transport must be an object with a type";
        return false;
    }
    return true;
}

}  // namespace

bool read_registry_bundle_manifest(
    const std::filesystem::path& bundle,
    RegistryBundleManifest& result,
    std::string& error) {
    std::string bytes;
    if (!read_file(bundle / "manifest.json", 4U * 1024U * 1024U, bytes, error))
        return false;
    Json root;
    JsonParser parser(bytes);
    if (!parser.parse(root, error)) {
        error = "malformed manifest.json: " + error;
        return false;
    }
    const Json::Object* manifest = object_value(root);
    const Json::Object* counts =
        manifest == nullptr ? nullptr : object_member(*manifest, "counts");
    if (manifest == nullptr || counts == nullptr ||
        !required_nonempty_string(
            *manifest, "snapshot_sha256", result.snapshot_sha256, error) ||
        !required_nonempty_string(
            *manifest, "completed_at", result.completed_at, error) ||
        !required_nonempty_string(
            *manifest, "started_at", result.started_at, error) ||
        !required_nonempty_string(
            *manifest, "registry_base_url", result.registry_base_url, error) ||
        !json_size_member(*manifest, "bundle_version", result.bundle_version) ||
        !json_size_member(*counts, "pages", result.pages) ||
        !json_size_member(*counts, "records_received", result.records_received) ||
        !json_size_member(
            *counts, "unique_server_versions", result.unique_server_versions)) {
        if (error.empty()) error = "manifest metadata or counts are invalid";
        return false;
    }
    const Json::Object* collector = object_member(*manifest, "collector");
    if (collector != nullptr) {
        if (!optional_string_member(
                *collector, "name", result.collector_name, error) ||
            !optional_string_member(
                *collector, "version", result.collector_version, error) ||
            !optional_string_member(
                *collector, "git_commit", result.collector_git_commit, error))
            return false;
    }
    return true;
}

bool parse_registry_canonical_record(
    std::string_view line,
    RegistryCanonicalRecord& result,
    std::string& error) {
    result = {};
    Json root;
    JsonParser parser(line);
    if (!parser.parse(root, error)) return false;
    const Json::Object* record = object_value(root);
    if (record == nullptr) return (error = "canonical record must be an object", false);
    if (!required_nonempty_string(
            *record, "server_identifier", result.server_identifier, error) ||
        !valid_server_name(result.server_identifier) ||
        !required_nonempty_string(
            *record, "server_version", result.server_version, error) ||
        !valid_server_version(result.server_version) ||
        !required_nonempty_string(
            *record, "canonical_sha256", result.canonical_sha256, error)) {
        if (error.empty()) error = "invalid canonical identity";
        return false;
    }
    if (!optional_string_member(
            *record, "description", result.description, error))
        return false;

    const auto repository_it = record->find("repository");
    if (repository_it != record->end()) {
        const Json::Object* repository =
            std::get_if<Json::Object>(&repository_it->second.value);
        RegistryRepositoryRecord parsed;
        if (repository == nullptr ||
            !optional_string_member(
                *repository, "source", parsed.source, error) ||
            !optional_string_member(
                *repository, "url", parsed.url, error)) {
            if (error.empty()) error = "repository must be an object";
            return false;
        }
        result.repository = std::move(parsed);
    }

    const auto packages_it = record->find("packages");
    if (packages_it != record->end()) {
        const auto* packages = std::get_if<Json::Array>(&packages_it->second.value);
        if (packages == nullptr) return (error = "packages must be an array", false);
        for (const Json& entry : *packages) {
            const Json::Object* package = object_value(entry);
            RegistryPackageRecord parsed;
            if (package == nullptr ||
                !required_nonempty_string(
                    *package, "registryType", parsed.registry_type, error) ||
                !required_nonempty_string(
                    *package, "identifier", parsed.identifier, error) ||
                !optional_string_member(
                    *package, "version", parsed.version, error) ||
                !parse_transport(*package, parsed.transport, error)) {
                if (error.empty()) error = "invalid package declaration";
                return false;
            }
            const auto arguments_it = package->find("packageArguments");
            if (arguments_it != package->end()) {
                const auto* arguments =
                    std::get_if<Json::Array>(&arguments_it->second.value);
                if (arguments == nullptr)
                    return (error = "packageArguments must be an array", false);
                for (const Json& argument_value : *arguments) {
                    const Json::Object* argument = object_value(argument_value);
                    RegistryPackageArgumentRecord parsed_argument;
                    if (argument == nullptr) {
                        error = "package argument must be an object";
                        return false;
                    }
                    if (!optional_string_member(
                            *argument, "value", parsed_argument.value, error))
                        return false;
                    parsed.arguments.push_back(std::move(parsed_argument));
                }
            }
            const auto environment_it = package->find("environmentVariables");
            if (environment_it != package->end()) {
                const auto* environment =
                    std::get_if<Json::Array>(&environment_it->second.value);
                if (environment == nullptr)
                    return (error = "environmentVariables must be an array", false);
                for (const Json& environment_value : *environment) {
                    const Json::Object* declaration = object_value(environment_value);
                    RegistryEnvironmentRecord parsed_environment;
                    if (declaration == nullptr ||
                        !required_string(
                            *declaration, "name", parsed_environment.name, error) ||
                        !optional_bool_member(
                            *declaration, "isRequired",
                            parsed_environment.required, error) ||
                        !optional_string_member(
                            *declaration, "description",
                            parsed_environment.description, error)) {
                        if (error.empty()) error = "invalid environment declaration";
                        return false;
                    }
                    parsed.environment.push_back(std::move(parsed_environment));
                }
            }
            result.packages.push_back(std::move(parsed));
        }
    }

    const auto remotes_it = record->find("remotes");
    if (remotes_it != record->end()) {
        const auto* remotes = std::get_if<Json::Array>(&remotes_it->second.value);
        if (remotes == nullptr) return (error = "remotes must be an array", false);
        for (const Json& entry : *remotes) {
            const Json::Object* remote = object_value(entry);
            RegistryRemoteRecord parsed;
            if (remote == nullptr ||
                !required_nonempty_string(*remote, "url", parsed.url, error) ||
                !required_nonempty_string(*remote, "type", parsed.transport, error)) {
                if (error.empty()) error = "invalid remote declaration";
                return false;
            }
            result.remotes.push_back(std::move(parsed));
        }
    }

    const Json::Object* original = object_member(*record, "original");
    const Json::Object* metadata = original == nullptr ?
        nullptr : object_member(*original, "_meta");
    const Json::Object* official = metadata == nullptr ?
        nullptr : object_member(*metadata, "io.modelcontextprotocol.registry/official");
    if (official != nullptr) {
        if (!optional_string_member(
                *official, "status", result.registry_status, error) ||
            !optional_string_member(
                *official, "publishedAt", result.published_at, error) ||
            !optional_string_member(
                *official, "updatedAt", result.updated_at, error))
            return false;
    }
    return true;
}

}  // namespace mcpo
