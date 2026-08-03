#include "observatory/analyze.hpp"
#include "observatory/catalog_lock.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <poll.h>
#include <set>
#include <signal.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace mcpo {
namespace {

#ifndef MCPO_VERSION
#define MCPO_VERSION "unknown"
#endif

constexpr std::string_view analysis_type_v1 = "npm_package_static_v1";

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
    if (value <= 0x7fU) out.push_back(static_cast<char>(value));
    else if (value <= 0x7ffU) {
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
        if (!valid_utf8(input_)) {
            error = "invalid UTF-8 in JSON";
            return false;
        }
        skip_space();
        if (!parse_value(out, 0U, error)) return false;
        skip_space();
        if (position_ != input_.size()) {
            error = "unexpected data after JSON value";
            return false;
        }
        return true;
    }

private:
    void skip_space() {
        while (position_ < input_.size() &&
               (input_[position_] == ' ' || input_[position_] == '\t' ||
                input_[position_] == '\r' || input_[position_] == '\n'))
            ++position_;
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
            else if (c >= 'a' && c <= 'f')
                digit = 10U + static_cast<unsigned>(c - 'a');
            else if (c >= 'A' && c <= 'F')
                digit = 10U + static_cast<unsigned>(c - 'A');
            else
                return false;
            value = value * 16U + digit;
        }
        return true;
    }

    bool parse_string(std::string& out, std::string& error) {
        if (position_ >= input_.size() || input_[position_++] != '"') {
            error = "expected JSON string";
            return false;
        }
        while (position_ < input_.size()) {
            const unsigned char c =
                static_cast<unsigned char>(input_[position_++]);
            if (c == '"') return true;
            if (c < 0x20U) {
                error = "control character in JSON string";
                return false;
            }
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                continue;
            }
            if (position_ >= input_.size()) {
                error = "unterminated JSON escape";
                return false;
            }
            const char escaped = input_[position_++];
            switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    out.push_back(escaped);
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u': {
                    unsigned codepoint{};
                    if (!hex4(codepoint)) {
                        error = "invalid unicode escape";
                        return false;
                    }
                    if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
                        if (!consume("\\u")) {
                            error = "missing low surrogate";
                            return false;
                        }
                        unsigned low{};
                        if (!hex4(low) || low < 0xdc00U || low > 0xdfffU) {
                            error = "invalid low surrogate";
                            return false;
                        }
                        codepoint = 0x10000U +
                            ((codepoint - 0xd800U) << 10U) + (low - 0xdc00U);
                    } else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU) {
                        error = "unexpected low surrogate";
                        return false;
                    }
                    append_utf8(out, codepoint);
                    break;
                }
                default:
                    error = "invalid JSON escape";
                    return false;
            }
        }
        error = "unterminated JSON string";
        return false;
    }

    bool parse_number(Json& out, std::string& error) {
        const std::size_t start = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        if (position_ >= input_.size() ||
            !std::isdigit(static_cast<unsigned char>(input_[position_]))) {
            error = "invalid JSON number";
            return false;
        }
        if (input_[position_] == '0') {
            ++position_;
        } else {
            while (position_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[position_])))
                ++position_;
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            if (position_ >= input_.size() ||
                !std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                error = "invalid JSON fraction";
                return false;
            }
            while (position_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[position_])))
                ++position_;
        }
        if (position_ < input_.size() &&
            (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-'))
                ++position_;
            if (position_ >= input_.size() ||
                !std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                error = "invalid JSON exponent";
                return false;
            }
            while (position_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[position_])))
                ++position_;
        }
        out.value = std::string(input_.substr(start, position_ - start));
        out.number = true;
        return true;
    }

    bool parse_value(Json& out, std::size_t depth, std::string& error) {
        if (depth > 64U) {
            error = "JSON nesting too deep";
            return false;
        }
        skip_space();
        if (position_ >= input_.size()) {
            error = "unexpected end of JSON";
            return false;
        }
        const char c = input_[position_];
        if (c == '"') {
            std::string text;
            if (!parse_string(text, error)) return false;
            out.value = std::move(text);
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
                skip_space();
                std::string key;
                if (!parse_string(key, error)) return false;
                skip_space();
                if (position_ >= input_.size() || input_[position_++] != ':') {
                    error = "expected ':' in object";
                    return false;
                }
                Json child;
                if (!parse_value(child, depth + 1U, error)) return false;
                if (!object.emplace(std::move(key), std::move(child)).second) {
                    error = "duplicate JSON object key";
                    return false;
                }
                skip_space();
                if (position_ >= input_.size()) {
                    error = "unterminated JSON object";
                    return false;
                }
                if (input_[position_] == '}') {
                    ++position_;
                    out.value = std::move(object);
                    return true;
                }
                if (input_[position_++] != ',') {
                    error = "expected ',' in object";
                    return false;
                }
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
                if (position_ >= input_.size()) {
                    error = "unterminated JSON array";
                    return false;
                }
                if (input_[position_] == ']') {
                    ++position_;
                    out.value = std::move(array);
                    return true;
                }
                if (input_[position_++] != ',') {
                    error = "expected ',' in array";
                    return false;
                }
            }
        }
        if (consume("true")) {
            out.value = true;
            return true;
        }
        if (consume("false")) {
            out.value = false;
            return true;
        }
        if (consume("null")) {
            out.value = nullptr;
            return true;
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c)))
            return parse_number(out, error);
        error = "unexpected JSON token";
        return false;
    }

    std::string_view input_;
    std::size_t position_{};
};

std::string json_escape(std::string_view value) {
    std::string out{"\""};
    for (unsigned char c : value) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20U) {
                    char buffer[8]{};
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}

const Json::Object* as_object(const Json& value) {
    return std::get_if<Json::Object>(&value.value);
}

const Json::Array* as_array(const Json& value) {
    return std::get_if<Json::Array>(&value.value);
}

const std::string* as_string(const Json& value) {
    if (value.number) return nullptr;
    return std::get_if<std::string>(&value.value);
}

const std::string* as_number_text(const Json& value) {
    if (!value.number) return nullptr;
    return std::get_if<std::string>(&value.value);
}

std::string utc_now() {
    const std::time_t now = std::time(nullptr);
    std::tm value{};
    gmtime_r(&now, &value);
    char buffer[21]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &value);
    return buffer;
}

std::string sqlite_message(sqlite3* database, std::string_view operation) {
    return std::string(operation) + ": " +
        (database == nullptr ? "SQLite handle unavailable" : sqlite3_errmsg(database));
}

class Database {
public:
    Database() = default;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    ~Database() {
        if (handle_ != nullptr) sqlite3_close(handle_);
    }

    bool open(const std::filesystem::path& path, int flags, std::string& error) {
        if ((flags & SQLITE_OPEN_READWRITE) != 0 &&
            !writer_lock_.acquire(path, error))
            return false;
        const int result = sqlite3_open_v2(path.c_str(), &handle_, flags, nullptr);
        if (result != SQLITE_OK) {
            error = sqlite_message(handle_, "open database " + path.string());
            return false;
        }
        sqlite3_extended_result_codes(handle_, 1);
        sqlite3_busy_timeout(handle_, 5'000);
        return execute("PRAGMA foreign_keys = ON;", error);
    }

    bool execute(std::string_view sql, std::string& error) {
        char* detail = nullptr;
        const int result = sqlite3_exec(
            handle_, std::string(sql).c_str(), nullptr, nullptr, &detail);
        if (result == SQLITE_OK) return true;
        error = detail == nullptr ? sqlite_message(handle_, "SQLite execute")
                                  : std::string(detail);
        sqlite3_free(detail);
        return false;
    }

    sqlite3* get() const noexcept { return handle_; }

private:
    CatalogWriterLock writer_lock_;
    sqlite3* handle_{};
};

class Statement {
public:
    Statement() = default;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    ~Statement() {
        if (statement_ != nullptr) sqlite3_finalize(statement_);
    }

    bool prepare(Database& database, std::string_view sql, std::string& error) {
        if (statement_ != nullptr) {
            sqlite3_finalize(statement_);
            statement_ = nullptr;
        }
        const int result = sqlite3_prepare_v2(
            database.get(), std::string(sql).c_str(), -1, &statement_, nullptr);
        if (result != SQLITE_OK) {
            error = sqlite_message(database.get(), "prepare statement");
            return false;
        }
        return true;
    }

    bool bind_text(int index, std::string_view value, std::string& error) {
        const int result = sqlite3_bind_text(
            statement_,
            index,
            value.data(),
            static_cast<int>(value.size()),
            SQLITE_TRANSIENT);
        if (result != SQLITE_OK) {
            error = "cannot bind text parameter";
            return false;
        }
        return true;
    }

    bool bind_int64(int index, sqlite3_int64 value, std::string& error) {
        if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
            error = "cannot bind integer parameter";
            return false;
        }
        return true;
    }

    bool bind_null(int index, std::string& error) {
        if (sqlite3_bind_null(statement_, index) != SQLITE_OK) {
            error = "cannot bind null parameter";
            return false;
        }
        return true;
    }

    int step() { return sqlite3_step(statement_); }

    bool step_done(Database& database, std::string& error) {
        const int result = step();
        if (result == SQLITE_DONE) return true;
        error = sqlite_message(database.get(), "execute prepared statement");
        return false;
    }

    sqlite3_int64 integer(int index) const {
        return sqlite3_column_int64(statement_, index);
    }

    std::string text(int index) const {
        const unsigned char* value = sqlite3_column_text(statement_, index);
        if (value == nullptr) return {};
        return reinterpret_cast<const char*>(value);
    }

    bool is_null(int index) const {
        return sqlite3_column_type(statement_, index) == SQLITE_NULL;
    }

    void reset() {
        sqlite3_reset(statement_);
        sqlite3_clear_bindings(statement_);
    }

private:
    sqlite3_stmt* statement_{};
};

class Transaction {
public:
    explicit Transaction(Database& database) : database_(database) {}
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction() {
        if (active_) {
            std::string ignored;
            database_.execute("ROLLBACK;", ignored);
        }
    }

    bool begin(std::string& error) {
        if (!database_.execute("BEGIN IMMEDIATE;", error)) return false;
        active_ = true;
        return true;
    }

    bool commit(std::string& error) {
        if (!database_.execute("COMMIT;", error)) return false;
        active_ = false;
        return true;
    }

private:
    Database& database_;
    bool active_{};
};

bool run_command(
    const std::vector<std::string>& arguments,
    std::chrono::steady_clock::duration timeout,
    std::size_t maximum_output,
    std::string& output,
    int& exit_status,
    std::string& error) {
    int pipe_fds[2]{};
    if (pipe(pipe_fds) != 0) {
        error = "cannot create process pipe";
        return false;
    }
    const pid_t child = fork();
    if (child < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        error = "cannot create child process";
        return false;
    }
    if (child == 0) {
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1U);
        for (const auto& argument : arguments)
            argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        execv(argv[0], argv.data());
        _exit(127);
    }
    close(pipe_fds[1]);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
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
        else if (waited < 0) {
            close(pipe_fds[0]);
            error = "cannot wait for child process";
            return false;
        } else if (std::chrono::steady_clock::now() >= deadline) {
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            close(pipe_fds[0]);
            error = "child process timed out";
            return false;
        } else {
            pollfd descriptor{pipe_fds[0], POLLIN, 0};
            poll(&descriptor, 1, 100);
        }
    }
    while (true) {
        std::array<char, 4096U> buffer{};
        const ssize_t count = read(pipe_fds[0], buffer.data(), buffer.size());
        if (count <= 0) break;
        if (output.size() + static_cast<std::size_t>(count) > maximum_output) {
            close(pipe_fds[0]);
            error = "child process output limit exceeded";
            return false;
        }
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    close(pipe_fds[0]);
    exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    return true;
}

bool sha256_hex(std::string_view bytes, std::string& digest, std::string& error) {
    const auto path = std::filesystem::temp_directory_path() /
        ("mcpo-hash-" + std::to_string(getpid()) + ".bin");
    {
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            error = "cannot create temporary hash input";
            return false;
        }
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            error = "cannot write temporary hash input";
            return false;
        }
    }
    std::string output;
    int status{};
    const bool ok = run_command(
        {"/usr/bin/openssl", "dgst", "-sha256", "-r", path.string()},
        std::chrono::seconds(30),
        4096U,
        output,
        status,
        error);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    if (!ok) return false;
    if (status != 0) {
        error = "OpenSSL SHA-256 failed with exit status " +
            std::to_string(status);
        if (!output.empty()) {
            constexpr std::size_t maximum_detail = 1024U;
            error += ": " +
                output.substr(0U, std::min(output.size(), maximum_detail));
        }
        return false;
    }
    if (output.size() < 64U) {
        error = "OpenSSL SHA-256 returned truncated output";
        return false;
    }
    digest = output.substr(0U, 64U);
    return std::all_of(digest.begin(), digest.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool sha512_raw(
    std::string_view bytes,
    std::string& digest,
    std::string& error) {
    const auto path = std::filesystem::temp_directory_path() /
        ("mcpo-hash512-" + std::to_string(getpid()) + ".bin");
    {
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            error = "cannot create temporary hash input";
            return false;
        }
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    std::string output;
    int status{};
    const bool ok = run_command(
        {"/usr/bin/openssl", "dgst", "-sha512", "-binary", path.string()},
        std::chrono::seconds(30),
        128U,
        output,
        status,
        error);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    if (!ok) return false;
    if (status != 0) {
        error = "OpenSSL SHA-512 failed with exit status " +
            std::to_string(status);
        if (!output.empty()) {
            constexpr std::size_t maximum_detail = 1024U;
            error += ": " +
                output.substr(0U, std::min(output.size(), maximum_detail));
        }
        return false;
    }
    if (output.size() != 64U) {
        error = "OpenSSL SHA-512 returned an unexpected digest length";
        return false;
    }
    digest = std::move(output);
    return true;
}

bool base64_decode(std::string_view input, std::string& out, std::string& error) {
    auto decode_char = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    if (input.size() % 4U != 0U) {
        error = "invalid base64 length";
        return false;
    }
    out.clear();
    out.reserve(input.size() / 4U * 3U);
    for (std::size_t index = 0U; index < input.size(); index += 4U) {
        const int a = decode_char(input[index]);
        const int b = decode_char(input[index + 1U]);
        const int c = input[index + 2U] == '=' ? 0 : decode_char(input[index + 2U]);
        const int d = input[index + 3U] == '=' ? 0 : decode_char(input[index + 3U]);
        if (a < 0 || b < 0 ||
            (input[index + 2U] != '=' && c < 0) ||
            (input[index + 3U] != '=' && d < 0)) {
            error = "invalid base64 character";
            return false;
        }
        const unsigned value = (static_cast<unsigned>(a) << 18U) |
            (static_cast<unsigned>(b) << 12U) |
            (static_cast<unsigned>(c) << 6U) |
            static_cast<unsigned>(d);
        out.push_back(static_cast<char>((value >> 16U) & 0xffU));
        if (input[index + 2U] != '=')
            out.push_back(static_cast<char>((value >> 8U) & 0xffU));
        if (input[index + 3U] != '=')
            out.push_back(static_cast<char>(value & 0xffU));
    }
    return true;
}

bool gunzip_bytes(
    std::string_view gzip_bytes,
    std::size_t maximum_bytes,
    std::string& plain,
    std::string& error) {
    const auto input_path = std::filesystem::temp_directory_path() /
        ("mcpo-gz-in-" + std::to_string(getpid()) + ".tgz");
    const auto output_path = std::filesystem::temp_directory_path() /
        ("mcpo-gz-out-" + std::to_string(getpid()) + ".tar");
    {
        std::ofstream out(input_path, std::ios::binary);
        if (!out) {
            error = "cannot stage gzip input";
            return false;
        }
        out.write(gzip_bytes.data(), static_cast<std::streamsize>(gzip_bytes.size()));
    }
    std::string command_output;
    int status{};
    const bool ok = run_command(
        {"/usr/bin/gzip", "-dc", input_path.string()},
        std::chrono::seconds(60),
        maximum_bytes + 1U,
        command_output,
        status,
        error);
    std::error_code ignored;
    std::filesystem::remove(input_path, ignored);
    std::filesystem::remove(output_path, ignored);
    if (!ok) return false;
    if (status != 0) {
        error = "gzip decompression failed";
        return false;
    }
    if (command_output.size() > maximum_bytes) {
        error = "uncompressed archive exceeds configured size limit";
        return false;
    }
    plain = std::move(command_output);
    return true;
}

bool read_file_bytes(
    const std::filesystem::path& path,
    std::size_t maximum_bytes,
    std::string& bytes,
    std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "cannot open " + path.string();
        return false;
    }
    bytes.assign(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    if (input.bad()) {
        error = "cannot read " + path.string();
        return false;
    }
    if (bytes.size() > maximum_bytes) {
        error = "file exceeds configured size limit: " + path.string();
        return false;
    }
    return true;
}

bool write_file_bytes(
    const std::filesystem::path& path,
    std::string_view bytes,
    std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "cannot create directory for " + path.string();
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "cannot write " + path.string();
        return false;
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        error = "cannot write " + path.string();
        return false;
    }
    return true;
}

bool replace_permissions(
    const std::filesystem::path& path,
    std::filesystem::perms permissions,
    std::string& error) {
    std::error_code ec;
    std::filesystem::permissions(
        path, permissions, std::filesystem::perm_options::replace, ec);
    if (ec) {
        error = "cannot set permissions on " + path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

struct RuleMetadata {
    std::string rule_id;
    std::string category;
    std::string severity;
    std::string confidence;
    std::string title;
    std::string explanation;
};

struct RuleDefaults {
    std::string rule_id_prefix;
    std::string category;
    std::string confidence;
    std::string title_prefix;
    std::string subject_path;
    std::string explanation;
};

struct NodeBuiltinRule {
    std::string symbol;
    std::vector<std::string> tokens;
    std::string severity;
};

struct RiskApiRule {
    RuleMetadata metadata;
    std::string detector;
    std::string pattern;
    std::string symbol;
    bool suppress_documentation_context{};
};

struct LifecycleRule {
    std::string name;
    std::string severity;
};

struct FileTypeRule {
    RuleMetadata metadata;
    std::vector<std::string> file_types;
    bool marks_native{};
};

struct AnalysisRules {
    std::string ruleset_version;
    std::vector<std::string> process_call_owners;
    std::vector<std::string> documentation_context_markers;
    RuleMetadata package_metadata;
    std::string package_metadata_subject_path;
    RuleDefaults node_builtin_defaults;
    std::vector<NodeBuiltinRule> node_builtins;
    RuleDefaults risk_api_defaults;
    std::vector<RiskApiRule> risk_apis;
    RuleDefaults lifecycle_defaults;
    std::vector<LifecycleRule> lifecycles;
    std::vector<FileTypeRule> file_types;
};

bool rule_string(
    const Json::Object& object,
    std::string_view key,
    std::string& value,
    std::string& error) {
    const auto it = object.find(std::string(key));
    if (it == object.end() || as_string(it->second) == nullptr ||
        as_string(it->second)->empty()) {
        error = "analysis rules field must be a non-empty string: " +
            std::string(key);
        return false;
    }
    value = *as_string(it->second);
    return true;
}

bool rule_string_array(
    const Json::Object& object,
    std::string_view key,
    std::vector<std::string>& values,
    std::string& error) {
    const auto it = object.find(std::string(key));
    const Json::Array* array =
        it == object.end() ? nullptr : as_array(it->second);
    if (array == nullptr || array->empty()) {
        error = "analysis rules field must be a non-empty string array: " +
            std::string(key);
        return false;
    }
    std::set<std::string> unique;
    for (const Json& item : *array) {
        const std::string* value = as_string(item);
        if (value == nullptr || value->empty() || !unique.insert(*value).second) {
            error = "analysis rules field contains an invalid or duplicate value: " +
                std::string(key);
            return false;
        }
        values.push_back(*value);
    }
    return true;
}

bool valid_rule_severity(std::string_view value) {
    return value == "info" || value == "low" || value == "medium" ||
        value == "high" || value == "critical";
}

bool valid_rule_confidence(std::string_view value) {
    return value == "low" || value == "medium" || value == "high";
}

bool parse_rule_metadata(
    const Json::Object& object,
    RuleMetadata& metadata,
    std::string& error) {
    if (!rule_string(object, "rule_id", metadata.rule_id, error) ||
        !rule_string(object, "category", metadata.category, error) ||
        !rule_string(object, "severity", metadata.severity, error) ||
        !rule_string(object, "confidence", metadata.confidence, error) ||
        !rule_string(object, "title", metadata.title, error) ||
        !rule_string(object, "explanation", metadata.explanation, error))
        return false;
    if (!valid_rule_severity(metadata.severity) ||
        !valid_rule_confidence(metadata.confidence)) {
        error = "analysis rule has invalid severity or confidence: " +
            metadata.rule_id;
        return false;
    }
    return true;
}

bool parse_rule_defaults(
    const Json::Object& root,
    std::string_view key,
    bool require_subject_path,
    RuleDefaults& defaults,
    std::string& error) {
    const auto it = root.find(std::string(key));
    const Json::Object* object =
        it == root.end() ? nullptr : as_object(it->second);
    if (object == nullptr) {
        error = "analysis rules field must be an object: " + std::string(key);
        return false;
    }
    if (!rule_string(*object, "category", defaults.category, error) ||
        !rule_string(*object, "confidence", defaults.confidence, error) ||
        !rule_string(*object, "explanation", defaults.explanation, error))
        return false;
    if (!valid_rule_confidence(defaults.confidence)) {
        error = "analysis rule defaults have invalid confidence: " +
            std::string(key);
        return false;
    }
    const auto prefix = object->find("rule_id_prefix");
    if (prefix != object->end()) {
        if (as_string(prefix->second) == nullptr ||
            as_string(prefix->second)->empty()) {
            error = "analysis rules rule_id_prefix must be a non-empty string";
            return false;
        }
        defaults.rule_id_prefix = *as_string(prefix->second);
    }
    const auto title = object->find("title_prefix");
    if (title != object->end()) {
        if (as_string(title->second) == nullptr ||
            as_string(title->second)->empty()) {
            error = "analysis rules title_prefix must be a non-empty string";
            return false;
        }
        defaults.title_prefix = *as_string(title->second);
    }
    if (require_subject_path &&
        !rule_string(*object, "subject_path", defaults.subject_path, error))
        return false;
    return true;
}

bool parse_analysis_rules_json(
    std::string_view json_text,
    AnalysisRules& rules,
    std::string& error) {
    Json root_value;
    JsonParser parser(json_text);
    if (!parser.parse(root_value, error)) return false;
    const Json::Object* root = as_object(root_value);
    if (root == nullptr) {
        error = "analysis rules document must be an object";
        return false;
    }
    const auto schema = root->find("schema_version");
    if (schema == root->end() || as_number_text(schema->second) == nullptr ||
        *as_number_text(schema->second) != "1") {
        error = "unsupported analysis rules schema_version";
        return false;
    }
    if (!rule_string(*root, "ruleset_version", rules.ruleset_version, error))
        return false;
    const auto engine_it = root->find("engine_options");
    const Json::Object* engine_options =
        engine_it == root->end() ? nullptr : as_object(engine_it->second);
    if (engine_options == nullptr ||
        !rule_string_array(
            *engine_options,
            "process_call_owners",
            rules.process_call_owners,
            error) ||
        !rule_string_array(
            *engine_options,
            "documentation_context_markers",
            rules.documentation_context_markers,
            error)) {
        if (error.empty())
            error = "analysis rules engine_options must be an object";
        return false;
    }

    const auto metadata_it = root->find("package_metadata_rule");
    const Json::Object* metadata_object =
        metadata_it == root->end() ? nullptr : as_object(metadata_it->second);
    if (metadata_object == nullptr ||
        !parse_rule_metadata(*metadata_object, rules.package_metadata, error) ||
        !rule_string(
            *metadata_object,
            "subject_path",
            rules.package_metadata_subject_path,
            error)) {
        if (error.empty())
            error = "analysis rules package_metadata_rule must be an object";
        return false;
    }
    if (!parse_rule_defaults(
            *root,
            "node_builtin_defaults",
            false,
            rules.node_builtin_defaults,
            error) ||
        rules.node_builtin_defaults.rule_id_prefix.empty() ||
        rules.node_builtin_defaults.title_prefix.empty() ||
        !parse_rule_defaults(
            *root, "risk_api_defaults", false, rules.risk_api_defaults, error) ||
        !parse_rule_defaults(
            *root,
            "lifecycle_defaults",
            true,
            rules.lifecycle_defaults,
            error) ||
        rules.lifecycle_defaults.rule_id_prefix.empty() ||
        rules.lifecycle_defaults.title_prefix.empty()) {
        if (error.empty()) error = "analysis rules defaults are incomplete";
        return false;
    }

    const auto builtins_it = root->find("node_builtin_rules");
    const Json::Array* builtins =
        builtins_it == root->end() ? nullptr : as_array(builtins_it->second);
    if (builtins == nullptr || builtins->empty()) {
        error = "analysis rules node_builtin_rules must be a non-empty array";
        return false;
    }
    std::set<std::string> builtin_symbols;
    for (const Json& item : *builtins) {
        const Json::Object* object = as_object(item);
        NodeBuiltinRule rule;
        if (object == nullptr ||
            !rule_string(*object, "symbol", rule.symbol, error) ||
            !rule_string_array(*object, "tokens", rule.tokens, error) ||
            !rule_string(*object, "severity", rule.severity, error))
            return false;
        if (!valid_rule_severity(rule.severity) ||
            !builtin_symbols.insert(rule.symbol).second) {
            error = "analysis rules contain invalid or duplicate node builtin: " +
                rule.symbol;
            return false;
        }
        rules.node_builtins.push_back(std::move(rule));
    }

    const auto risk_it = root->find("risk_api_rules");
    const Json::Array* risk_rules =
        risk_it == root->end() ? nullptr : as_array(risk_it->second);
    if (risk_rules == nullptr || risk_rules->empty()) {
        error = "analysis rules risk_api_rules must be a non-empty array";
        return false;
    }
    std::set<std::string> rule_ids{rules.package_metadata.rule_id};
    for (const Json& item : *risk_rules) {
        const Json::Object* object = as_object(item);
        RiskApiRule rule;
        if (object == nullptr ||
            !rule_string(*object, "rule_id", rule.metadata.rule_id, error) ||
            !rule_string(*object, "detector", rule.detector, error) ||
            !rule_string(*object, "pattern", rule.pattern, error) ||
            !rule_string(*object, "symbol", rule.symbol, error) ||
            !rule_string(*object, "severity", rule.metadata.severity, error) ||
            !rule_string(*object, "title", rule.metadata.title, error))
            return false;
        rule.metadata.category = rules.risk_api_defaults.category;
        rule.metadata.confidence = rules.risk_api_defaults.confidence;
        rule.metadata.explanation = rules.risk_api_defaults.explanation;
        if (!valid_rule_severity(rule.metadata.severity) ||
            !rule_ids.insert(rule.metadata.rule_id).second) {
            error = "analysis rules contain invalid or duplicate risk API rule: " +
                rule.metadata.rule_id;
            return false;
        }
        static const std::set<std::string> detectors{
            "process_call", "call", "substring", "websocket_call",
            "nonliteral_require"};
        if (!detectors.contains(rule.detector)) {
            error = "analysis rule has unsupported detector: " + rule.detector;
            return false;
        }
        const auto suppress = object->find("suppress_documentation_context");
        if (suppress != object->end()) {
            const bool* value = std::get_if<bool>(&suppress->second.value);
            if (value == nullptr) {
                error =
                    "suppress_documentation_context must be a JSON boolean";
                return false;
            }
            rule.suppress_documentation_context = *value;
        }
        rules.risk_apis.push_back(std::move(rule));
    }

    const auto lifecycle_it = root->find("lifecycle_rules");
    const Json::Array* lifecycle_rules =
        lifecycle_it == root->end() ? nullptr : as_array(lifecycle_it->second);
    if (lifecycle_rules == nullptr || lifecycle_rules->empty()) {
        error = "analysis rules lifecycle_rules must be a non-empty array";
        return false;
    }
    std::set<std::string> lifecycle_names;
    for (const Json& item : *lifecycle_rules) {
        const Json::Object* object = as_object(item);
        LifecycleRule rule;
        if (object == nullptr ||
            !rule_string(*object, "name", rule.name, error) ||
            !rule_string(*object, "severity", rule.severity, error))
            return false;
        if (!valid_rule_severity(rule.severity) ||
            !lifecycle_names.insert(rule.name).second) {
            error = "analysis rules contain invalid or duplicate lifecycle rule: " +
                rule.name;
            return false;
        }
        rules.lifecycles.push_back(std::move(rule));
    }

    const auto file_it = root->find("file_type_rules");
    const Json::Array* file_rules =
        file_it == root->end() ? nullptr : as_array(file_it->second);
    if (file_rules == nullptr || file_rules->empty()) {
        error = "analysis rules file_type_rules must be a non-empty array";
        return false;
    }
    std::set<std::string> classified_file_types;
    for (const Json& item : *file_rules) {
        const Json::Object* object = as_object(item);
        FileTypeRule rule;
        if (object == nullptr ||
            !parse_rule_metadata(*object, rule.metadata, error) ||
            !rule_string_array(*object, "file_types", rule.file_types, error))
            return false;
        const auto native = object->find("marks_native");
        const bool* marks_native =
            native == object->end() ? nullptr :
                std::get_if<bool>(&native->second.value);
        if (marks_native == nullptr) {
            error = "analysis file-type rule marks_native must be a JSON boolean";
            return false;
        }
        rule.marks_native = *marks_native;
        if (!rule_ids.insert(rule.metadata.rule_id).second) {
            error = "analysis rules contain duplicate rule_id: " +
                rule.metadata.rule_id;
            return false;
        }
        for (const std::string& file_type : rule.file_types) {
            if (!classified_file_types.insert(file_type).second) {
                error = "analysis file type appears in multiple rules: " +
                    file_type;
                return false;
            }
        }
        rules.file_types.push_back(std::move(rule));
    }
    return true;
}

bool load_analysis_rules(
    const std::filesystem::path& path,
    AnalysisRules& rules,
    std::string& error,
    std::string* source_json = nullptr) {
    std::string json_text;
    if (!read_file_bytes(path, 1024U * 1024U, json_text, error)) {
        error = "cannot load analysis rules " + path.string() + ": " + error;
        return false;
    }
    if (!parse_analysis_rules_json(json_text, rules, error)) {
        error = "invalid analysis rules " + path.string() + ": " + error;
        return false;
    }
    if (source_json != nullptr) *source_json = std::move(json_text);
    return true;
}

std::string lowercase(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool has_dotdot_segment(std::string_view path) {
    std::size_t start = 0U;
    while (start <= path.size()) {
        const std::size_t end = path.find('/', start);
        const std::string_view part =
            path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
        if (part == "..") return true;
        if (end == std::string_view::npos) break;
        start = end + 1U;
    }
    return false;
}

std::string normalize_archive_path(std::string_view path) {
    std::string out;
    out.reserve(path.size());
    for (char c : path) {
        if (c == '\\') out.push_back('/');
        else out.push_back(c);
    }
    while (!out.empty() && out.front() == '/') out.erase(out.begin());
    return out;
}

std::string file_extension(std::string_view path) {
    const auto slash = path.find_last_of('/');
    const std::string_view name =
        slash == std::string_view::npos ? path : path.substr(slash + 1U);
    const auto dot = name.find_last_of('.');
    if (dot == std::string_view::npos) return {};
    return lowercase(name.substr(dot));
}

std::string classify_file_type(std::string_view path, std::string_view bytes) {
    const std::string ext = file_extension(path);
    if (ext == ".js" || ext == ".mjs" || ext == ".cjs") return "javascript";
    if (path.ends_with(".d.ts")) return "typescript_declaration";
    if (ext == ".ts") return "typescript";
    if (ext == ".py" || ext == ".pyi") return "python";
    if (ext == ".map") return "source_map";
    if (ext == ".sh" || ext == ".bash") return "shell_script";
    if (ext == ".ps1" || ext == ".psm1") return "powershell_script";
    if (ext == ".node") return "native_node_module";
    if (ext == ".so" || ext == ".dylib") return "shared_library";
    if (ext == ".dll") return "shared_library";
    if (ext == ".wasm") return "wasm";
    if (ext == ".tgz" || ext == ".tar" || ext == ".gz" || ext == ".zip" ||
        ext == ".jar" || ext == ".war")
        return "nested_archive";
    if (bytes.size() >= 4U) {
        const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
        if (data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' && data[3] == 'F')
            return "elf";
        if (data[0] == 'M' && data[1] == 'Z') return "pe";
        if ((data[0] == 0xfe && data[1] == 0xed && data[2] == 0xfa &&
             (data[3] == 0xce || data[3] == 0xcf)) ||
            (data[0] == 0xcf && data[1] == 0xfa && data[2] == 0xed &&
             data[3] == 0xfe) ||
            (data[0] == 0xca && data[1] == 0xfe && data[2] == 0xba &&
             data[3] == 0xbe))
            return "mach_o";
        if (data[0] == 0x00 && data[1] == 0x61 && data[2] == 0x73 &&
            data[3] == 0x6d)
            return "wasm";
    }
    if (ext == ".json") return "json";
    if (ext == ".md") return "markdown";
    if (ext == ".txt") return "text";
    return "other";
}

bool looks_minified(std::string_view text) {
    if (text.size() < 2048U) return false;
    std::size_t newlines = 0U;
    for (char c : text)
        if (c == '\n') ++newlines;
    return newlines < 3U && text.size() > 4096U;
}

std::string bound_snippet(std::string_view text, std::size_t limit) {
    if (text.size() <= limit) return std::string(text);
    std::string out(text.substr(0U, limit));
    out += "...";
    return out;
}

void add_finding(
    AnalyzerWorkerResult& result,
    AnalysisFinding finding) {
    result.findings.push_back(std::move(finding));
}

bool is_identifier_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}

bool looks_like_documentation_url_context(
    std::string_view line,
    const AnalysisRules& rules) {
    const std::string lower = lowercase(line);
    return std::any_of(
        rules.documentation_context_markers.begin(),
        rules.documentation_context_markers.end(),
        [&](const std::string& marker) {
            return lower.find(lowercase(marker)) != std::string::npos;
        });
}

void scan_javascript_text(
    std::string_view archive_path,
    std::string_view text,
    const AnalysisRules& rules,
    const ArchiveLimits& limits,
    AnalyzerWorkerResult& result) {
    std::set<std::string> seen_rules;
    std::uint32_t line_number = 1U;
    std::size_t line_start = 0U;
    for (std::size_t index = 0U; index <= text.size(); ++index) {
        if (index == text.size() || text[index] == '\n') {
            const std::string_view line = text.substr(line_start, index - line_start);
            for (const NodeBuiltinRule& builtin : rules.node_builtins) {
                bool matched = false;
                for (const std::string& token : builtin.tokens) {
                    if (line.find("'" + token + "'") != std::string_view::npos ||
                        line.find("\"" + token + "\"") != std::string_view::npos) {
                        matched = true;
                        break;
                    }
                }
                if (!matched) continue;
                const std::string rule_id =
                    rules.node_builtin_defaults.rule_id_prefix + builtin.symbol;
                const std::string key = rule_id + ":" +
                    std::string(archive_path) + ":" + std::to_string(line_number);
                if (!seen_rules.insert(key).second) continue;
                AnalysisFinding finding;
                finding.rule_id = rule_id;
                finding.category = rules.node_builtin_defaults.category;
                finding.severity = builtin.severity;
                finding.confidence = rules.node_builtin_defaults.confidence;
                finding.title =
                    rules.node_builtin_defaults.title_prefix + builtin.symbol;
                finding.subject_path = std::string(archive_path);
                finding.line_number = line_number;
                finding.symbol = builtin.symbol;
                finding.evidence =
                    bound_snippet(line, limits.maximum_evidence_snippet_bytes);
                finding.explanation = rules.node_builtin_defaults.explanation;
                add_finding(result, std::move(finding));
            }

            const auto find_process_api = [&](std::string_view name) -> bool {
                std::size_t pos = 0U;
                while ((pos = line.find(name, pos)) != std::string_view::npos) {
                    const std::size_t after = pos + name.size();
                    if (after >= line.size() || line[after] != '(') {
                        pos = after;
                        continue;
                    }
                    if (pos > 0U && is_identifier_char(line[pos - 1U])) {
                        pos = after;
                        continue;
                    }
                    if (pos > 0U && line[pos - 1U] == '.') {
                        // Property calls are ignored unless the owner looks like
                        // a child_process binding. This avoids RegExp.exec and
                        // constant.exec false positives.
                        std::size_t start = pos - 1U;
                        while (start > 0U && is_identifier_char(line[start - 1U]))
                            --start;
                        const std::string_view owner =
                            line.substr(start, pos - 1U - start);
                        const std::string owner_lower = lowercase(owner);
                        if (std::find(
                                rules.process_call_owners.begin(),
                                rules.process_call_owners.end(),
                                owner_lower) ==
                            rules.process_call_owners.end()) {
                            pos = after;
                            continue;
                        }
                    }
                    return true;
                }
                return false;
            };

            const auto find_call = [&](std::string_view name) -> bool {
                std::size_t pos = 0U;
                while ((pos = line.find(name, pos)) != std::string_view::npos) {
                    const std::size_t after = pos + name.size();
                    if (after < line.size() && line[after] == '(') {
                        if (pos > 0U && is_identifier_char(line[pos - 1U])) {
                            pos = after;
                            continue;
                        }
                        return true;
                    }
                    pos = after;
                }
                return false;
            };

            const auto find_nonliteral_call = [&](std::string_view name) -> bool {
                std::size_t position = 0U;
                while ((position = line.find(name, position)) !=
                       std::string_view::npos) {
                    if (position > 0U && is_identifier_char(line[position - 1U])) {
                        position += name.size();
                        continue;
                    }
                    std::size_t argument = position + name.size();
                    while (argument < line.size() &&
                           std::isspace(
                               static_cast<unsigned char>(line[argument])))
                        ++argument;
                    if (argument >= line.size() || line[argument] != '(') {
                        position += name.size();
                        continue;
                    }
                    ++argument;
                    while (argument < line.size() &&
                           std::isspace(
                               static_cast<unsigned char>(line[argument])))
                        ++argument;
                    if (argument < line.size() && line[argument] != '\'' &&
                        line[argument] != '"')
                        return true;
                    position += name.size();
                }
                return false;
            };

            for (const RiskApiRule& rule : rules.risk_apis) {
                bool matched = false;
                if (rule.detector == "process_call")
                    matched = find_process_api(rule.pattern);
                else if (rule.detector == "call")
                    matched = find_call(rule.pattern);
                else if (rule.detector == "substring")
                    matched =
                        line.find(rule.pattern) != std::string_view::npos;
                else if (rule.detector == "websocket_call")
                    matched =
                        line.find("new " + rule.pattern) !=
                            std::string_view::npos ||
                        find_call(rule.pattern);
                else if (rule.detector == "nonliteral_require")
                    matched = find_nonliteral_call(rule.pattern);
                if (!matched) continue;
                if (rule.suppress_documentation_context &&
                    looks_like_documentation_url_context(line, rules) &&
                    rule.detector != "call" &&
                    rule.detector != "websocket_call")
                    continue;
                const std::string key = rule.metadata.rule_id + ":" +
                    std::string(archive_path) + ":" +
                    std::to_string(line_number);
                if (!seen_rules.insert(key).second) continue;
                AnalysisFinding finding;
                finding.rule_id = rule.metadata.rule_id;
                finding.category = rule.metadata.category;
                finding.severity = rule.metadata.severity;
                finding.confidence = rule.metadata.confidence;
                finding.title = rule.metadata.title;
                finding.subject_path = std::string(archive_path);
                finding.line_number = line_number;
                finding.symbol = rule.symbol;
                finding.evidence =
                    bound_snippet(line, limits.maximum_evidence_snippet_bytes);
                finding.explanation = rule.metadata.explanation;
                add_finding(result, std::move(finding));
            }

            if (index < text.size()) ++line_number;
            line_start = index + 1U;
        }
    }
}

struct TarMember {
    std::string path;
    char typeflag{'0'};
    std::uint64_t size{};
    std::string linkname;
    std::string data;
    unsigned mode{};
};

bool parse_octal(std::string_view text, std::uint64_t& value) {
    value = 0U;
    bool saw = false;
    for (char c : text) {
        if (c == '\0' || c == ' ') {
            if (saw) break;
            continue;
        }
        if (c < '0' || c > '7') return false;
        saw = true;
        value = (value << 3U) + static_cast<std::uint64_t>(c - '0');
    }
    return true;
}

bool parse_ustar(
    std::string_view tar_bytes,
    const ArchiveLimits& limits,
    std::vector<TarMember>& members,
    std::string& error) {
    std::size_t offset = 0U;
    std::uint64_t total = 0U;
    while (offset + 512U <= tar_bytes.size()) {
        const std::string_view header = tar_bytes.substr(offset, 512U);
        offset += 512U;
        bool all_zero = true;
        for (char c : header) {
            if (c != '\0') {
                all_zero = false;
                break;
            }
        }
        if (all_zero) break;
        if (members.size() >= limits.maximum_files) {
            error = "archive file count exceeds configured limit";
            return false;
        }
        std::string name(header.substr(0U, 100U));
        name.erase(std::find(name.begin(), name.end(), '\0'), name.end());
        std::string prefix(header.substr(345U, 155U));
        prefix.erase(std::find(prefix.begin(), prefix.end(), '\0'), prefix.end());
        std::string raw_path = prefix.empty() ? name : prefix + "/" + name;
        for (char& c : raw_path)
            if (c == '\\') c = '/';
        if (!raw_path.empty() &&
            (raw_path.front() == '/' ||
             (raw_path.size() >= 2U && std::isalpha(static_cast<unsigned char>(raw_path[0])) &&
              raw_path[1] == ':'))) {
            error = "archive contains absolute path entry";
            return false;
        }
        std::string path = normalize_archive_path(raw_path);
        if (path.empty()) {
            error = "malformed archive entry with empty path";
            return false;
        }
        if (has_dotdot_segment(path)) {
            error = "archive contains parent-traversal path entry";
            return false;
        }
        std::uint64_t size{};
        if (!parse_octal(header.substr(124U, 12U), size)) {
            error = "malformed archive size field";
            return false;
        }
        if (size > limits.maximum_individual_file_bytes) {
            error = "archive entry exceeds individual file size limit";
            return false;
        }
        if (total > limits.maximum_total_uncompressed_bytes ||
            size > limits.maximum_total_uncompressed_bytes - total) {
            error = "archive exceeds total uncompressed size limit";
            return false;
        }
        std::uint64_t mode{};
        parse_octal(header.substr(100U, 8U), mode);
        const char typeflag = header[156];
        std::string linkname(header.substr(157U, 100U));
        linkname.erase(
            std::find(linkname.begin(), linkname.end(), '\0'), linkname.end());
        for (char& c : linkname)
            if (c == '\\') c = '/';
        if (typeflag == '3' || typeflag == '4' || typeflag == '6') {
            error = "archive contains device or fifo entry";
            return false;
        }
        const std::size_t data_size = static_cast<std::size_t>(size);
        if (offset + data_size > tar_bytes.size()) {
            error = "truncated archive member data";
            return false;
        }
        TarMember member;
        member.path = std::move(path);
        member.typeflag = typeflag;
        member.size = size;
        member.linkname = linkname;
        member.mode = static_cast<unsigned>(mode);
        if (typeflag == '0' || typeflag == '\0') {
            member.data = std::string(tar_bytes.substr(offset, data_size));
            total += size;
        }
        members.push_back(std::move(member));
        const std::size_t padded = (data_size + 511U) & ~static_cast<std::size_t>(511U);
        offset += padded;
    }
    return true;
}

bool link_escapes_root(std::string_view member_path, std::string_view linkname) {
    if (linkname.empty()) return true;
    if (linkname.front() == '/' ||
        (linkname.size() >= 2U && std::isalpha(static_cast<unsigned char>(linkname[0])) &&
         linkname[1] == ':') ||
        has_dotdot_segment(linkname))
        return true;
    std::string base = normalize_archive_path(member_path);
    const auto slash = base.find_last_of('/');
    if (slash == std::string::npos) base.clear();
    else base.resize(slash);
    std::string resolved = base.empty() ? std::string(linkname)
                                        : base + "/" + std::string(linkname);
    resolved = normalize_archive_path(resolved);
    return has_dotdot_segment(resolved) || resolved.empty();
}

std::string object_string(const Json::Object& object, std::string_view key) {
    const auto it = object.find(std::string(key));
    if (it == object.end()) return {};
    const auto* value = as_string(it->second);
    return value == nullptr ? std::string{} : *value;
}

bool extract_dependencies(
    const Json::Object& manifest,
    std::string_view key,
    bool development,
    std::string_view dependency_type,
    AnalyzerWorkerResult& result) {
    const auto it = manifest.find(std::string(key));
    if (it == manifest.end()) return true;
    const auto* object = as_object(it->second);
    if (object == nullptr) return false;
    for (const auto& [name, version_json] : *object) {
        const auto* version = as_string(version_json);
        if (version == nullptr) continue;
        AnalysisDependency dependency;
        dependency.dependency_type = std::string(dependency_type);
        dependency.dependency_name = name;
        dependency.declared_version = *version;
        dependency.direct = true;
        dependency.development = development;
        result.dependencies.push_back(std::move(dependency));
    }
    return true;
}

}  // namespace

bool parse_npm_version_metadata(
    std::string_view json_text,
    std::string_view expected_name,
    std::string_view expected_version,
    NpmDistMetadata& metadata,
    std::string& error) {
    Json root;
    JsonParser parser(json_text);
    if (!parser.parse(root, error)) return false;
    const auto* object = as_object(root);
    if (object == nullptr) {
        error = "npm metadata must be an object";
        return false;
    }
    std::string name = object_string(*object, "name");
    std::string version = object_string(*object, "version");
    if (name.empty() || version.empty()) {
        error = "npm metadata missing name or version";
        return false;
    }
    if (name != expected_name || version != expected_version) {
        error = "npm metadata identity mismatch";
        return false;
    }
    std::string tarball = object_string(*object, "dist.tarball");
    std::string integrity = object_string(*object, "dist.integrity");
    std::string shasum = object_string(*object, "dist.shasum");
    if (tarball.empty() || integrity.empty()) {
        const auto dist_it = object->find("dist");
        if (dist_it == object->end()) {
            error = "npm metadata missing dist.tarball/dist.integrity";
            return false;
        }
        const auto* dist = as_object(dist_it->second);
        if (dist == nullptr) {
            error = "npm metadata dist must be an object";
            return false;
        }
        tarball = object_string(*dist, "tarball");
        integrity = object_string(*dist, "integrity");
        shasum = object_string(*dist, "shasum");
    }
    if (tarball.empty() || integrity.empty()) {
        error = "npm metadata missing tarball or integrity";
        return false;
    }
    metadata.name = std::move(name);
    metadata.version = std::move(version);
    metadata.tarball_url = std::move(tarball);
    metadata.integrity = std::move(integrity);
    if (!shasum.empty()) metadata.shasum = std::move(shasum);
    return true;
}

bool parse_pypi_release_metadata(
    std::string_view json_text,
    std::string_view expected_name,
    std::string_view expected_version,
    const AcquisitionLimits& limits,
    ArtifactDescriptor& descriptor,
    std::string& error) {
    Json root;
    JsonParser parser(json_text);
    if (!parser.parse(root, error)) return false;
    const auto* object = as_object(root);
    if (object == nullptr) {
        error = "PyPI release metadata must be an object";
        return false;
    }

    const auto info_it = object->find("info");
    const auto urls_it = object->find("urls");
    const auto* info =
        info_it == object->end() ? nullptr : as_object(info_it->second);
    const auto* urls =
        urls_it == object->end() ? nullptr : as_array(urls_it->second);
    if (info == nullptr || urls == nullptr) {
        error = "PyPI release metadata requires info and urls";
        return false;
    }
    if (urls->size() > limits.maximum_release_files) {
        error = "PyPI release contains too many files";
        return false;
    }

    const std::string name = object_string(*info, "name");
    const std::string version = object_string(*info, "version");
    if (name.empty() || version.empty()) {
        error = "PyPI release metadata missing name or version";
        return false;
    }
    std::string normalized_name;
    std::string normalized_expected;
    if (!normalize_pypi_project_name(name, normalized_name, error) ||
        !normalize_pypi_project_name(expected_name, normalized_expected, error)) {
        return false;
    }
    if (normalized_name != normalized_expected || version != expected_version) {
        error = "PyPI release metadata identity mismatch";
        return false;
    }

    std::vector<PypiReleaseFile> files;
    files.reserve(urls->size());
    for (const Json& item : *urls) {
        const auto* file = as_object(item);
        if (file == nullptr) {
            error = "PyPI release file must be an object";
            return false;
        }
        const std::string filename = object_string(*file, "filename");
        const std::string package_type = object_string(*file, "packagetype");
        const std::string url = object_string(*file, "url");
        const auto size_it = file->find("size");
        const std::string* size_text =
            size_it == file->end() ? nullptr : as_number_text(size_it->second);
        const auto digests_it = file->find("digests");
        const auto* digests =
            digests_it == file->end() ? nullptr : as_object(digests_it->second);
        const std::string sha256 =
            digests == nullptr ? std::string{} : object_string(*digests, "sha256");
        const auto yanked_it = file->find("yanked");
        const bool* yanked = yanked_it == file->end()
            ? nullptr
            : std::get_if<bool>(&yanked_it->second.value);
        std::uint64_t size{};
        if (filename.empty() || package_type.empty() || url.empty() ||
            size_text == nullptr || size_text->empty() || digests == nullptr ||
            sha256.empty() || yanked == nullptr) {
            error = "PyPI release file is missing a required field";
            return false;
        }
        const auto converted = std::from_chars(
            size_text->data(), size_text->data() + size_text->size(), size);
        if (converted.ec != std::errc{} ||
            converted.ptr != size_text->data() + size_text->size()) {
            error = "PyPI release file size must be a non-negative integer";
            return false;
        }
        files.push_back({
            filename,
            package_type,
            url,
            sha256,
            size,
            *yanked,
        });
    }
    return select_pypi_sdist_artifact(
        normalized_expected, expected_version, files, limits, descriptor, error);
}

bool verify_npm_integrity(
    std::string_view artifact_bytes,
    std::string_view published_integrity,
    std::string& error) {
    const auto dash = published_integrity.find('-');
    if (dash == std::string_view::npos) {
        error = "invalid npm integrity format";
        return false;
    }
    const std::string_view algo = published_integrity.substr(0U, dash);
    const std::string_view encoded = published_integrity.substr(dash + 1U);
    std::string decoded;
    if (!base64_decode(encoded, decoded, error)) return false;
    if (algo == "sha512") {
        std::string digest;
        if (!sha512_raw(artifact_bytes, digest, error)) return false;
        if (decoded.size() != digest.size() || digest != decoded) {
            error = "npm integrity mismatch";
            return false;
        }
        return true;
    }
    if (algo == "sha256") {
        std::string digest;
        if (!sha256_hex(artifact_bytes, digest, error)) return false;
        std::string expected_hex;
        expected_hex.resize(decoded.size() * 2U);
        static constexpr char hex[] = "0123456789abcdef";
        for (std::size_t i = 0U; i < decoded.size(); ++i) {
            const auto value = static_cast<unsigned char>(decoded[i]);
            expected_hex[i * 2U] = hex[value >> 4U];
            expected_hex[i * 2U + 1U] = hex[value & 0x0fU];
        }
        if (digest != expected_hex) {
            error = "npm integrity mismatch";
            return false;
        }
        return true;
    }
    error = "unsupported npm integrity algorithm";
    return false;
}

bool verify_pypi_integrity(
    std::string_view artifact_bytes,
    std::string_view published_sha256,
    std::uint64_t published_size,
    std::string& error) {
    if (artifact_bytes.size() != published_size) {
        error = "PyPI artifact size does not match release metadata";
        return false;
    }
    if (published_sha256.size() != 64U ||
        !std::all_of(
            published_sha256.begin(),
            published_sha256.end(),
            [](char character) {
                return (character >= '0' && character <= '9') ||
                    (character >= 'a' && character <= 'f') ||
                    (character >= 'A' && character <= 'F');
            })) {
        error = "invalid PyPI SHA-256 digest";
        return false;
    }
    std::string actual;
    if (!sha256_hex(artifact_bytes, actual, error)) return false;
    if (!std::equal(
            actual.begin(),
            actual.end(),
            published_sha256.begin(),
            [](char left, char right) {
                if (right >= 'A' && right <= 'F')
                    right = static_cast<char>(right - 'A' + 'a');
                return left == right;
            })) {
        error = "PyPI SHA-256 integrity mismatch";
        return false;
    }
    return true;
}

bool analyze_package_tarball_bytes(
    std::string_view registry_type,
    std::string_view tarball_bytes,
    const std::filesystem::path& rules_path,
    const ArchiveLimits& limits,
    AnalyzerWorkerResult& result,
    std::string& error) {
    result = AnalyzerWorkerResult{};
    AnalysisRules rules;
    if (!load_analysis_rules(rules_path, rules, error)) return false;
    result.ruleset_version = rules.ruleset_version;
    if (tarball_bytes.size() > limits.maximum_tarball_bytes) {
        error = "tarball exceeds configured size limit";
        return false;
    }
    std::string tar_bytes;
    if (!gunzip_bytes(
            tarball_bytes, limits.maximum_total_uncompressed_bytes, tar_bytes, error))
        return false;
    std::vector<TarMember> members;
    if (!parse_ustar(tar_bytes, limits, members, error)) return false;

    std::ostringstream inventory;
    inventory << "{\"files\":[";
    bool first = true;
    std::optional<std::string> package_json_text;
    std::optional<std::string> pypi_metadata_text;
    std::string pypi_metadata_path;
    for (const TarMember& member : members) {
        if (member.typeflag == '2' || member.typeflag == '1') {
            if (link_escapes_root(member.path, member.linkname)) {
                error = "archive link target escapes package root";
                return false;
            }
        }
        if (!first) inventory << ',';
        first = false;
        inventory << "{\"path\":" << json_escape(member.path)
                  << ",\"type\":" << json_escape(std::string(1, member.typeflag))
                  << ",\"size\":" << member.size << '}';
        if ((member.typeflag == '0' || member.typeflag == '\0') &&
            (member.path == "package/package.json" ||
             member.path.ends_with("/package.json")) &&
            !package_json_text)
            package_json_text = member.data;
        if ((member.typeflag == '0' || member.typeflag == '\0') &&
            member.path.ends_with("/PKG-INFO") &&
            std::count(member.path.begin(), member.path.end(), '/') == 1 &&
            !pypi_metadata_text) {
            pypi_metadata_text = member.data;
            pypi_metadata_path = member.path;
        }
    }
    inventory << "]}";
    result.archive_inventory_json = inventory.str();

    std::optional<std::string> manifest_text;
    std::string manifest_subject;
    std::vector<std::pair<std::string, std::string>> pypi_dependencies;
    if (registry_type == "npm") {
        if (!package_json_text) {
            error = "package.json missing from npm tarball";
            return false;
        }
        manifest_text = std::move(package_json_text);
        manifest_subject = rules.package_metadata_subject_path;
    } else if (registry_type == "pypi") {
        if (!pypi_metadata_text) {
            error = "root PKG-INFO missing from PyPI source distribution";
            return false;
        }
        std::string name;
        std::string version;
        std::string license;
        std::string repository;
        std::istringstream metadata_stream(*pypi_metadata_text);
        std::string line;
        while (std::getline(metadata_stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            const std::string key = line.substr(0U, colon);
            std::string value = line.substr(colon + 1U);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                value.erase(value.begin());
            if (key == "Name" && name.empty()) name = value;
            else if (key == "Version" && version.empty()) version = value;
            else if (key == "License" && license.empty()) license = value;
            else if ((key == "Home-page" || key == "Project-URL") &&
                     repository.empty())
                repository = value;
            else if (key == "Requires-Dist") {
                const auto separator = value.find_first_of("[ (;<>=!~");
                const std::string dependency_name = value.substr(0U, separator);
                if (!dependency_name.empty())
                    pypi_dependencies.emplace_back(dependency_name, value);
            }
        }
        if (name.empty() || version.empty()) {
            error = "PyPI PKG-INFO missing Name or Version";
            return false;
        }
        std::ostringstream synthesized;
        synthesized << "{\"name\":" << json_escape(name)
                    << ",\"version\":" << json_escape(version)
                    << ",\"license\":" << json_escape(license)
                    << ",\"repository\":" << json_escape(repository) << '}';
        manifest_text = synthesized.str();
        manifest_subject = pypi_metadata_path;
    } else {
        error = "unsupported package registry for analyzer worker";
        return false;
    }
    result.package_manifest_json = *manifest_text;
    Json manifest_json;
    JsonParser parser(*manifest_text);
    if (!parser.parse(manifest_json, error)) return false;
    const auto* manifest = as_object(manifest_json);
    if (manifest == nullptr) {
        error = "package.json must be an object";
        return false;
    }
    result.package_name = object_string(*manifest, "name");
    result.package_version = object_string(*manifest, "version");
    result.license = object_string(*manifest, "license");
    if (result.license->empty()) result.license.reset();
    const auto repo_it = manifest->find("repository");
    if (repo_it != manifest->end()) {
        if (const auto* text = as_string(repo_it->second))
            result.repository = *text;
        else if (const auto* object = as_object(repo_it->second))
            result.repository = object_string(*object, "url");
    }
    result.main_entry = object_string(*manifest, "main");
    if (result.main_entry->empty()) result.main_entry.reset();
    result.module_entry = object_string(*manifest, "module");
    if (result.module_entry->empty()) result.module_entry.reset();
    const auto engines_it = manifest->find("engines");
    if (engines_it != manifest->end()) {
        if (const auto* engines = as_object(engines_it->second)) {
            const std::string node = object_string(*engines, "node");
            if (!node.empty()) result.engines_node = node;
        }
    }
    const auto bin_it = manifest->find("bin");
    if (bin_it != manifest->end()) {
        if (const auto* text = as_string(bin_it->second)) {
            result.bin_entries.emplace_back(result.package_name, *text);
        } else if (const auto* object = as_object(bin_it->second)) {
            for (const auto& [name, value] : *object) {
                if (const auto* path = as_string(value))
                    result.bin_entries.emplace_back(name, *path);
            }
        }
    }
    const auto scripts_it = manifest->find("scripts");
    if (scripts_it != manifest->end()) {
        if (const auto* scripts = as_object(scripts_it->second)) {
            for (const LifecycleRule& rule : rules.lifecycles) {
                const auto it = scripts->find(rule.name);
                if (it == scripts->end()) continue;
                const auto* command = as_string(it->second);
                if (command == nullptr) continue;
                result.lifecycle_scripts.emplace_back(rule.name, *command);
                AnalysisFinding finding;
                finding.rule_id =
                    rules.lifecycle_defaults.rule_id_prefix + rule.name;
                finding.category = rules.lifecycle_defaults.category;
                finding.severity = rule.severity;
                finding.confidence = rules.lifecycle_defaults.confidence;
                finding.title =
                    rules.lifecycle_defaults.title_prefix + rule.name;
                finding.subject_path = rules.lifecycle_defaults.subject_path;
                finding.symbol = rule.name;
                finding.evidence =
                    bound_snippet(*command, limits.maximum_evidence_snippet_bytes);
                finding.explanation = rules.lifecycle_defaults.explanation;
                add_finding(result, std::move(finding));
            }
        }
    }
    if (!extract_dependencies(*manifest, "dependencies", false, "runtime", result) ||
        !extract_dependencies(
            *manifest, "devDependencies", true, "development", result) ||
        !extract_dependencies(*manifest, "peerDependencies", false, "peer", result) ||
        !extract_dependencies(
            *manifest, "optionalDependencies", false, "optional", result)) {
        error = "invalid dependency object in package.json";
        return false;
    }
    for (const auto& [name, declaration] : pypi_dependencies) {
        AnalysisDependency dependency;
        dependency.dependency_type = "runtime";
        dependency.dependency_name = name;
        dependency.declared_version = declaration;
        dependency.direct = true;
        result.dependencies.push_back(std::move(dependency));
    }

    {
        AnalysisFinding meta;
        meta.rule_id = rules.package_metadata.rule_id;
        meta.category = rules.package_metadata.category;
        meta.severity = rules.package_metadata.severity;
        meta.confidence = rules.package_metadata.confidence;
        meta.title = rules.package_metadata.title;
        meta.subject_path = manifest_subject;
        meta.evidence = bound_snippet(
            result.package_name + "@" + result.package_version,
            limits.maximum_evidence_snippet_bytes);
        meta.explanation = rules.package_metadata.explanation;
        add_finding(result, std::move(meta));
    }

    for (const TarMember& member : members) {
        if (member.typeflag != '0' && member.typeflag != '\0') continue;
        AnalysisFileRecord file;
        file.archive_path = member.path;
        file.byte_size = member.size;
        if (!sha256_hex(member.data, file.sha256, error)) return false;
        file.file_type = classify_file_type(member.path, member.data);
        file.executable = (member.mode & 0111U) != 0U;
        const FileTypeRule* matched_file_rule = nullptr;
        for (const FileTypeRule& rule : rules.file_types) {
            if (std::find(
                    rule.file_types.begin(),
                    rule.file_types.end(),
                    file.file_type) != rule.file_types.end()) {
                matched_file_rule = &rule;
                break;
            }
        }
        file.native_binary =
            matched_file_rule != nullptr && matched_file_rule->marks_native;
        file.generated = file.file_type == "source_map" ||
            member.path.find("/dist/") != std::string::npos;
        file.minified =
            (file.file_type == "javascript" || file.file_type == "typescript") &&
            looks_minified(member.data);
        if (file.native_binary) result.has_native_code = true;
        if (matched_file_rule != nullptr) {
            AnalysisFinding finding;
            finding.rule_id = matched_file_rule->metadata.rule_id;
            finding.category = matched_file_rule->metadata.category;
            finding.severity = matched_file_rule->metadata.severity;
            finding.confidence = matched_file_rule->metadata.confidence;
            finding.title = matched_file_rule->metadata.title;
            finding.subject_path = member.path;
            finding.evidence = file.file_type;
            finding.explanation = matched_file_rule->metadata.explanation;
            add_finding(result, std::move(finding));
        }
        if (file.file_type == "javascript" || file.file_type == "typescript" ||
            file.file_type == "typescript_declaration")
            scan_javascript_text(member.path, member.data, rules, limits, result);
        result.files.push_back(std::move(file));
    }
    result.analyzed_file_count = result.files.size();
    result.status = "ok";

    std::ostringstream summary;
    summary << "{\"ruleset_version\":" << json_escape(result.ruleset_version)
            << ",\"package_name\":" << json_escape(result.package_name)
            << ",\"package_version\":" << json_escape(result.package_version)
            << ",\"analyzed_file_count\":" << result.analyzed_file_count
            << ",\"dependency_count\":" << result.dependencies.size()
            << ",\"finding_count\":" << result.findings.size()
            << ",\"has_native_code\":" << (result.has_native_code ? "true" : "false")
            << ",\"lifecycle_scripts\":[";
    for (std::size_t i = 0U; i < result.lifecycle_scripts.size(); ++i) {
        if (i != 0U) summary << ',';
        summary << json_escape(result.lifecycle_scripts[i].first);
    }
    summary << "]}";
    result.summary_json = summary.str();
    return true;
}

bool analyze_npm_tarball_bytes(
    std::string_view tarball_bytes,
    const std::filesystem::path& rules_path,
    const ArchiveLimits& limits,
    AnalyzerWorkerResult& result,
    std::string& error) {
    return analyze_package_tarball_bytes(
        "npm", tarball_bytes, rules_path, limits, result, error);
}

namespace {

std::string finding_to_json(const AnalysisFinding& finding) {
    std::ostringstream out;
    out << "{\"rule_id\":" << json_escape(finding.rule_id)
        << ",\"category\":" << json_escape(finding.category)
        << ",\"severity\":" << json_escape(finding.severity)
        << ",\"confidence\":" << json_escape(finding.confidence)
        << ",\"disposition\":" << json_escape(finding.disposition)
        << ",\"title\":" << json_escape(finding.title)
        << ",\"subject_path\":" << json_escape(finding.subject_path)
        << ",\"line_number\":";
    if (finding.line_number) out << *finding.line_number;
    else out << "null";
    out << ",\"symbol\":";
    if (finding.symbol) out << json_escape(*finding.symbol);
    else out << "null";
    out << ",\"evidence\":" << json_escape(finding.evidence)
        << ",\"explanation\":" << json_escape(finding.explanation) << '}';
    return out.str();
}

std::string worker_result_to_json(const AnalyzerWorkerResult& result) {
    std::ostringstream out;
    out << "{\"status\":" << json_escape(result.status)
        << ",\"ruleset_version\":" << json_escape(result.ruleset_version)
        << ",\"package_name\":" << json_escape(result.package_name)
        << ",\"package_version\":" << json_escape(result.package_version)
        << ",\"license\":";
    if (result.license) out << json_escape(*result.license);
    else out << "null";
    out << ",\"repository\":";
    if (result.repository) out << json_escape(*result.repository);
    else out << "null";
    out << ",\"main_entry\":";
    if (result.main_entry) out << json_escape(*result.main_entry);
    else out << "null";
    out << ",\"module_entry\":";
    if (result.module_entry) out << json_escape(*result.module_entry);
    else out << "null";
    out << ",\"engines_node\":";
    if (result.engines_node) out << json_escape(*result.engines_node);
    else out << "null";
    out << ",\"bin\":[";
    for (std::size_t i = 0U; i < result.bin_entries.size(); ++i) {
        if (i) out << ',';
        out << "{\"name\":" << json_escape(result.bin_entries[i].first)
            << ",\"path\":" << json_escape(result.bin_entries[i].second) << '}';
    }
    out << "],\"lifecycle_scripts\":[";
    for (std::size_t i = 0U; i < result.lifecycle_scripts.size(); ++i) {
        if (i) out << ',';
        out << "{\"name\":" << json_escape(result.lifecycle_scripts[i].first)
            << ",\"command\":" << json_escape(result.lifecycle_scripts[i].second)
            << '}';
    }
    out << "],\"dependencies\":[";
    for (std::size_t i = 0U; i < result.dependencies.size(); ++i) {
        const auto& dep = result.dependencies[i];
        if (i) out << ',';
        out << "{\"dependency_type\":" << json_escape(dep.dependency_type)
            << ",\"dependency_name\":" << json_escape(dep.dependency_name)
            << ",\"declared_version\":" << json_escape(dep.declared_version)
            << ",\"resolved_version\":";
        if (dep.resolved_version) out << json_escape(*dep.resolved_version);
        else out << "null";
        out << ",\"direct\":" << (dep.direct ? "true" : "false")
            << ",\"development\":" << (dep.development ? "true" : "false") << '}';
    }
    out << "],\"files\":[";
    for (std::size_t i = 0U; i < result.files.size(); ++i) {
        const auto& file = result.files[i];
        if (i) out << ',';
        out << "{\"archive_path\":" << json_escape(file.archive_path)
            << ",\"file_type\":" << json_escape(file.file_type)
            << ",\"byte_size\":" << file.byte_size
            << ",\"sha256\":" << json_escape(file.sha256)
            << ",\"executable\":" << (file.executable ? "true" : "false")
            << ",\"native_binary\":" << (file.native_binary ? "true" : "false")
            << ",\"generated\":" << (file.generated ? "true" : "false")
            << ",\"minified\":" << (file.minified ? "true" : "false") << '}';
    }
    out << "],\"findings\":[";
    for (std::size_t i = 0U; i < result.findings.size(); ++i) {
        if (i) out << ',';
        out << finding_to_json(result.findings[i]);
    }
    out << "],\"archive_inventory_json\":" << json_escape(result.archive_inventory_json)
        << ",\"package_manifest_json\":" << json_escape(result.package_manifest_json)
        << ",\"summary_json\":" << json_escape(result.summary_json)
        << ",\"has_native_code\":" << (result.has_native_code ? "true" : "false")
        << ",\"analyzed_file_count\":" << result.analyzed_file_count << '}';
    return out.str();
}

bool require_string_field(
    const Json::Object& object,
    std::string_view key,
    std::string& out,
    std::string& error) {
    const auto it = object.find(std::string(key));
    if (it == object.end()) {
        error = "missing field " + std::string(key);
        return false;
    }
    const auto* value = as_string(it->second);
    if (value == nullptr) {
        error = "field " + std::string(key) + " must be a string";
        return false;
    }
    out = *value;
    return true;
}

bool enum_allowed(std::string_view value, std::initializer_list<std::string_view> allowed) {
    for (std::string_view item : allowed)
        if (item == value) return true;
    return false;
}

}  // namespace

bool parse_analyzer_worker_json(
    std::string_view json_text,
    const ArchiveLimits& limits,
    AnalyzerWorkerResult& result,
    std::string& error) {
    if (json_text.size() > limits.maximum_analyzer_output_bytes) {
        error = "analyzer output exceeds configured limit";
        return false;
    }
    Json root;
    JsonParser parser(json_text);
    if (!parser.parse(root, error)) return false;
    const auto* object = as_object(root);
    if (object == nullptr) {
        error = "analyzer output must be an object";
        return false;
    }
    AnalyzerWorkerResult parsed;
    if (!require_string_field(*object, "status", parsed.status, error) ||
        !require_string_field(
            *object, "ruleset_version", parsed.ruleset_version, error) ||
        !require_string_field(*object, "package_name", parsed.package_name, error) ||
        !require_string_field(
            *object, "package_version", parsed.package_version, error))
        return false;
    if (parsed.status != "ok") {
        error = "analyzer status is not ok";
        return false;
    }
    auto optional_string = [&](std::string_view key, std::optional<std::string>& target) {
        const auto it = object->find(std::string(key));
        if (it == object->end() || std::holds_alternative<std::nullptr_t>(it->second.value))
            return true;
        const auto* value = as_string(it->second);
        if (value == nullptr) {
            error = std::string(key) + " must be string or null";
            return false;
        }
        target = *value;
        return true;
    };
    if (!optional_string("license", parsed.license) ||
        !optional_string("repository", parsed.repository) ||
        !optional_string("main_entry", parsed.main_entry) ||
        !optional_string("module_entry", parsed.module_entry) ||
        !optional_string("engines_node", parsed.engines_node))
        return false;
    const auto findings_it = object->find("findings");
    if (findings_it == object->end() || as_array(findings_it->second) == nullptr) {
        error = "findings must be an array";
        return false;
    }
    for (const Json& item : *as_array(findings_it->second)) {
        const auto* finding_object = as_object(item);
        if (finding_object == nullptr) {
            error = "finding must be an object";
            return false;
        }
        AnalysisFinding finding;
        if (!require_string_field(*finding_object, "rule_id", finding.rule_id, error) ||
            !require_string_field(*finding_object, "category", finding.category, error) ||
            !require_string_field(*finding_object, "severity", finding.severity, error) ||
            !require_string_field(
                *finding_object, "confidence", finding.confidence, error) ||
            !require_string_field(
                *finding_object, "disposition", finding.disposition, error) ||
            !require_string_field(*finding_object, "title", finding.title, error) ||
            !require_string_field(
                *finding_object, "subject_path", finding.subject_path, error) ||
            !require_string_field(
                *finding_object, "explanation", finding.explanation, error))
            return false;
        if (!enum_allowed(
                finding.severity, {"info", "low", "medium", "high", "critical"}) ||
            !enum_allowed(finding.confidence, {"low", "medium", "high"}) ||
            !enum_allowed(
                finding.disposition,
                {"unreviewed",
                 "expected",
                 "reviewed-benign",
                 "mitigated",
                 "suspicious",
                 "confirmed-risk",
                 "false-positive"})) {
            error = "finding enum value rejected";
            return false;
        }
        const auto evidence_it = finding_object->find("evidence");
        if (evidence_it != finding_object->end()) {
            if (const auto* evidence = as_string(evidence_it->second)) {
                if (evidence->size() > limits.maximum_evidence_snippet_bytes + 16U) {
                    error = "finding evidence snippet too large";
                    return false;
                }
                finding.evidence = *evidence;
            }
        }
        const auto line_it = finding_object->find("line_number");
        if (line_it != finding_object->end() &&
            !std::holds_alternative<std::nullptr_t>(line_it->second.value)) {
            const auto* text = as_number_text(line_it->second);
            if (text == nullptr) {
                error = "line_number must be a number or null";
                return false;
            }
            unsigned value{};
            const auto parsed_number =
                std::from_chars(text->data(), text->data() + text->size(), value);
            if (parsed_number.ec != std::errc{} || value == 0U) {
                error = "invalid line_number";
                return false;
            }
            finding.line_number = value;
        }
        const auto symbol_it = finding_object->find("symbol");
        if (symbol_it != finding_object->end() &&
            !std::holds_alternative<std::nullptr_t>(symbol_it->second.value)) {
            const auto* symbol = as_string(symbol_it->second);
            if (symbol == nullptr) {
                error = "symbol must be string or null";
                return false;
            }
            finding.symbol = *symbol;
        }
        parsed.findings.push_back(std::move(finding));
    }
    const auto files_it = object->find("files");
    if (files_it == object->end() || as_array(files_it->second) == nullptr) {
        error = "files must be an array";
        return false;
    }
    for (const Json& item : *as_array(files_it->second)) {
        const auto* file_object = as_object(item);
        if (file_object == nullptr) {
            error = "file must be an object";
            return false;
        }
        AnalysisFileRecord file;
        if (!require_string_field(
                *file_object, "archive_path", file.archive_path, error) ||
            !require_string_field(*file_object, "file_type", file.file_type, error) ||
            !require_string_field(*file_object, "sha256", file.sha256, error))
            return false;
        const auto size_it = file_object->find("byte_size");
        if (size_it == file_object->end()) {
            error = "byte_size must be a number";
            return false;
        }
        const auto* size_text = as_number_text(size_it->second);
        if (size_text == nullptr) {
            error = "byte_size must be a number";
            return false;
        }
        std::uint64_t size{};
        const auto parsed_size = std::from_chars(
            size_text->data(), size_text->data() + size_text->size(), size);
        if (parsed_size.ec != std::errc{}) {
            error = "invalid byte_size";
            return false;
        }
        file.byte_size = size;
        auto bool_field = [&](std::string_view key, bool& target) {
            const auto it = file_object->find(std::string(key));
            if (it == file_object->end() || !std::holds_alternative<bool>(it->second.value)) {
                error = std::string(key) + " must be boolean";
                return false;
            }
            target = std::get<bool>(it->second.value);
            return true;
        };
        if (!bool_field("executable", file.executable) ||
            !bool_field("native_binary", file.native_binary) ||
            !bool_field("generated", file.generated) ||
            !bool_field("minified", file.minified))
            return false;
        if (file.native_binary) parsed.has_native_code = true;
        parsed.files.push_back(std::move(file));
    }
    const auto deps_it = object->find("dependencies");
    if (deps_it != object->end()) {
        const auto* deps = as_array(deps_it->second);
        if (deps == nullptr) {
            error = "dependencies must be an array";
            return false;
        }
        for (const Json& item : *deps) {
            const auto* dep_object = as_object(item);
            if (dep_object == nullptr) {
                error = "dependency must be an object";
                return false;
            }
            AnalysisDependency dependency;
            if (!require_string_field(
                    *dep_object, "dependency_type", dependency.dependency_type, error) ||
                !require_string_field(
                    *dep_object, "dependency_name", dependency.dependency_name, error) ||
                !require_string_field(
                    *dep_object, "declared_version", dependency.declared_version, error))
                return false;
            const auto resolved_it = dep_object->find("resolved_version");
            if (resolved_it != dep_object->end() &&
                !std::holds_alternative<std::nullptr_t>(resolved_it->second.value)) {
                const auto* resolved = as_string(resolved_it->second);
                if (resolved == nullptr) {
                    error = "resolved_version must be string or null";
                    return false;
                }
                dependency.resolved_version = *resolved;
            }
            const auto direct_it = dep_object->find("direct");
            const auto dev_it = dep_object->find("development");
            if (direct_it == dep_object->end() ||
                !std::holds_alternative<bool>(direct_it->second.value) ||
                dev_it == dep_object->end() ||
                !std::holds_alternative<bool>(dev_it->second.value)) {
                error = "dependency direct/development must be boolean";
                return false;
            }
            dependency.direct = std::get<bool>(direct_it->second.value);
            dependency.development = std::get<bool>(dev_it->second.value);
            parsed.dependencies.push_back(std::move(dependency));
        }
    }
    if (!require_string_field(
            *object, "archive_inventory_json", parsed.archive_inventory_json, error) ||
        !require_string_field(
            *object, "package_manifest_json", parsed.package_manifest_json, error) ||
        !require_string_field(*object, "summary_json", parsed.summary_json, error))
        return false;
    const auto native_it = object->find("has_native_code");
    if (native_it != object->end() && std::holds_alternative<bool>(native_it->second.value))
        parsed.has_native_code = std::get<bool>(native_it->second.value);
    parsed.analyzed_file_count = parsed.files.size();
    const auto life_it = object->find("lifecycle_scripts");
    if (life_it != object->end()) {
        const auto* life = as_array(life_it->second);
        if (life == nullptr) {
            error = "lifecycle_scripts must be an array";
            return false;
        }
        for (const Json& item : *life) {
            const auto* life_object = as_object(item);
            if (life_object == nullptr) continue;
            std::string name;
            std::string command;
            if (require_string_field(*life_object, "name", name, error) &&
                require_string_field(*life_object, "command", command, error))
                parsed.lifecycle_scripts.emplace_back(std::move(name), std::move(command));
            else
                return false;
        }
    }
    result = std::move(parsed);
    return true;
}

AnalyzeError resolve_exact_package(
    const std::filesystem::path& database_path,
    std::string_view server_identifier,
    std::string_view server_version,
    std::string_view package_identifier,
    ResolvedPackage& resolved,
    std::string& error) {
    Database database;
    if (!database.open(
            database_path, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, error))
        return AnalyzeError::database;
    Statement query;
    if (!query.prepare(
            database,
            "SELECT p.id,p.server_version_id,sv.server_identifier,sv.server_version,"
            "p.registry_type,p.identifier,p.version,p.transport "
            "FROM packages p "
            "JOIN server_versions sv ON sv.id=p.server_version_id "
            "WHERE sv.server_identifier=?1 AND sv.server_version=?2 "
            "AND p.identifier=?3 "
            "ORDER BY p.position,p.id;",
            error) ||
        !query.bind_text(1, server_identifier, error) ||
        !query.bind_text(2, server_version, error) ||
        !query.bind_text(3, package_identifier, error))
        return AnalyzeError::database;
    std::vector<ResolvedPackage> matches;
    while (true) {
        const int step = query.step();
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            error = sqlite_message(database.get(), "resolve package");
            return AnalyzeError::database;
        }
        ResolvedPackage row;
        row.package_id = query.integer(0);
        row.server_version_id = query.integer(1);
        row.server_identifier = query.text(2);
        row.server_version = query.text(3);
        row.registry_type = query.text(4);
        row.package_identifier = query.text(5);
        if (!query.is_null(6)) row.package_version = query.text(6);
        row.transport = query.text(7);
        matches.push_back(std::move(row));
    }
    if (matches.empty()) {
        error = "exact package record not found for server/version/package";
        return AnalyzeError::package_not_found;
    }
    if (matches.size() > 1U) {
        const ResolvedPackage& canonical = matches.front();
        const bool same_artifact = std::all_of(
            matches.begin() + 1,
            matches.end(),
            [&canonical](const ResolvedPackage& candidate) {
                return candidate.registry_type == canonical.registry_type &&
                    candidate.package_identifier ==
                        canonical.package_identifier &&
                    candidate.package_version == canonical.package_version;
            });
        if (!same_artifact) {
            error = "ambiguous package selection for server/version/package";
            return AnalyzeError::ambiguous_package;
        }
    }
    if (matches.front().registry_type != "npm" &&
        matches.front().registry_type != "pypi") {
        error = "unsupported registry type for package analysis v1: " +
            matches.front().registry_type;
        return AnalyzeError::unsupported_registry;
    }
    if (!matches.front().package_version || matches.front().package_version->empty()) {
        error = "exact package version is required; refusing to select latest";
        return AnalyzeError::missing_version;
    }
    resolved = std::move(matches.front());
    return AnalyzeError::none;
}

int analyze_worker_main(
    std::string_view registry_type,
    const std::filesystem::path& tarball,
    const std::filesystem::path& rules_path,
    const ArchiveLimits& limits,
    std::ostream& out,
    std::ostream& err) {
    std::string bytes;
    std::string error;
    if (!read_file_bytes(tarball, limits.maximum_tarball_bytes, bytes, error)) {
        err << error << '\n';
        return 2;
    }
    AnalyzerWorkerResult result;
    if (!analyze_package_tarball_bytes(
            registry_type, bytes, rules_path, limits, result, error)) {
        err << error << '\n';
        return 3;
    }
    out << worker_result_to_json(result) << '\n';
    return 0;
}

namespace {

AnalyzePackageResult failure_result(AnalyzeError error, std::string message) {
    return AnalyzePackageResult{error, std::move(message), std::nullopt, std::nullopt,
        std::nullopt, false};
}

bool default_download(
    const std::string& url,
    std::chrono::steady_clock::duration timeout,
    std::size_t maximum_bytes,
    std::string& body,
    std::string& error) {
    const auto path = std::filesystem::temp_directory_path() /
        ("mcpo-dl-" + std::to_string(getpid()) + ".bin");
    std::string output;
    int status{};
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(timeout).count();
    const bool ok = run_command(
        {"/usr/bin/curl",
         "--silent",
         "--show-error",
         "--fail",
         "--location",
         "--max-redirs",
         "3",
         "--max-time",
         std::to_string(seconds > 0 ? seconds : 60),
         "--output",
         path.string(),
         url},
        timeout + std::chrono::seconds(5),
        1U * 1024U * 1024U,
        output,
        status,
        error);
    if (!ok) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return false;
    }
    if (status != 0) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        error = output.empty() ? "curl download failed" : output;
        return false;
    }
    const bool read_ok =
        read_file_bytes(path, maximum_bytes, body, error);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return read_ok;
}

bool in_process_worker(
    std::string_view registry_type,
    const std::filesystem::path& tarball,
    const std::filesystem::path& rules_path,
    const ArchiveLimits& limits,
    AnalyzerWorkerResult& result,
    std::string& raw_json,
    std::string& error) {
    std::string bytes;
    if (!read_file_bytes(tarball, limits.maximum_tarball_bytes, bytes, error))
        return false;
    if (!analyze_package_tarball_bytes(
            registry_type, bytes, rules_path, limits, result, error))
        return false;
    raw_json = worker_result_to_json(result);
    return true;
}

bool docker_worker(
    const AnalyzePackageOptions& options,
    std::string_view registry_type,
    const std::filesystem::path& tarball,
    const std::filesystem::path& rules_path,
    AnalyzerWorkerResult& result,
    std::string& raw_json,
    std::string& error) {
    if (options.self_executable.empty()) {
        error = "self executable path required for container analysis";
        return false;
    }
    const std::string tarball_abs =
        std::filesystem::absolute(tarball).lexically_normal().string();
    const std::string binary_abs =
        std::filesystem::absolute(options.self_executable).lexically_normal().string();
    const std::string rules_abs =
        std::filesystem::absolute(rules_path).lexically_normal().string();
    std::vector<std::string> args{
        options.docker_binary,
        "run",
        "--rm",
        "--network",
        "none",
        "--read-only",
        "--tmpfs",
        "/tmp:rw,nosuid,nodev,size=67108864",
        "--tmpfs",
        "/work:rw,nosuid,nodev,size=268435456",
        "--user",
        "65532:65532",
        "--cap-drop",
        "ALL",
        "--security-opt",
        "no-new-privileges",
        "--memory",
        "512m",
        "--cpus",
        "1.0",
        "--pids-limit",
        "128",
        "--ulimit",
        "nofile=256:256",
        "--mount",
        "type=bind,src=" + binary_abs + ",dst=/opt/mcp-observatory,ro=true",
        "--mount",
        "type=bind,src=" + tarball_abs + ",dst=/in/artifact.tgz,ro=true",
        "--mount",
        "type=bind,src=" + rules_abs + ",dst=/rules/analysis.json,ro=true",
        "--mount",
        "type=bind,src=/usr/bin/gzip,dst=/usr/bin/gzip,ro=true",
        "--mount",
        "type=bind,src=/usr/bin/openssl,dst=/usr/bin/openssl,ro=true",
        "--mount",
        "type=bind,src=/lib/x86_64-linux-gnu,dst=/lib/x86_64-linux-gnu,ro=true",
        "--mount",
        "type=bind,src=/lib64,dst=/lib64,ro=true",
        "debian:bookworm-slim",
        "/opt/mcp-observatory",
        "analyze-worker",
        "--registry",
        std::string(registry_type),
        "--tarball",
        "/in/artifact.tgz",
        "--rules",
        "/rules/analysis.json",
        "--maximum-files",
        std::to_string(options.limits.maximum_files),
        "--maximum-total-uncompressed-bytes",
        std::to_string(options.limits.maximum_total_uncompressed_bytes),
        "--maximum-individual-file-bytes",
        std::to_string(options.limits.maximum_individual_file_bytes),
        "--maximum-tarball-bytes",
        std::to_string(options.limits.maximum_tarball_bytes)};
    int status{};
    if (!run_command(
            args,
            options.container_timeout,
            options.limits.maximum_analyzer_output_bytes,
            raw_json,
            status,
            error))
        return false;
    if (status != 0) {
        error = raw_json.empty() ? "analyzer container failed" : raw_json;
        return false;
    }
    // The worker emits one JSON object on one physical line. Docker may prepend
    // diagnostics, so select the last complete object-shaped line without
    // searching for braces inside the nested JSON document.
    std::string json_text;
    std::size_t line_start = 0U;
    while (line_start <= raw_json.size()) {
        const std::size_t newline = raw_json.find('\n', line_start);
        const std::size_t line_end =
            newline == std::string::npos ? raw_json.size() : newline;
        std::string_view line(raw_json.data() + line_start, line_end - line_start);
        while (!line.empty() &&
               (line.front() == ' ' || line.front() == '\t' || line.front() == '\r'))
            line.remove_prefix(1U);
        while (!line.empty() &&
               (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
            line.remove_suffix(1U);
        if (line.size() >= 2U && line.front() == '{' && line.back() == '}')
            json_text.assign(line);
        if (newline == std::string::npos) break;
        line_start = newline + 1U;
    }
    if (json_text.empty()) {
        error = "analyzer container produced no complete JSON object line";
        return false;
    }
    return parse_analyzer_worker_json(json_text, options.limits, result, error);
}

std::filesystem::path evidence_dir_for(
    const std::filesystem::path& evidence_root,
    std::string_view sha256) {
    if (sha256.size() < 4U) return evidence_root / "artifacts" / "sha256" / "invalid";
    return evidence_root / "artifacts" / "sha256" /
        std::string(sha256.substr(0U, 2U)) / std::string(sha256);
}

std::string relative_posix(const std::filesystem::path& path) {
    std::string text = path.generic_string();
    while (text.starts_with("./")) text.erase(0, 2);
    return text;
}

bool record_evidence_file(
    Database& database,
    sqlite3_int64 run_id,
    std::string_view evidence_type,
    const std::filesystem::path& absolute_path,
    const std::filesystem::path& relative_path,
    std::string_view media_type,
    std::vector<AnalysisEvidenceFile>& collected,
    std::string& error) {
    std::string bytes;
    if (!read_file_bytes(absolute_path, 32U * 1024U * 1024U, bytes, error))
        return false;
    std::string digest;
    if (!sha256_hex(bytes, digest, error)) return false;
    Statement insert;
    if (!insert.prepare(
            database,
            "INSERT INTO analysis_evidence("
            "analysis_run_id,evidence_type,relative_path,sha256,byte_size,media_type)"
            " VALUES(?1,?2,?3,?4,?5,?6);",
            error) ||
        !insert.bind_int64(1, run_id, error) ||
        !insert.bind_text(2, evidence_type, error) ||
        !insert.bind_text(3, relative_posix(relative_path), error) ||
        !insert.bind_text(4, digest, error) ||
        !insert.bind_int64(5, static_cast<sqlite3_int64>(bytes.size()), error) ||
        !insert.bind_text(6, media_type, error) ||
        !insert.step_done(database, error))
        return false;
    AnalysisEvidenceFile row;
    row.evidence_type = std::string(evidence_type);
    row.relative_path = relative_posix(relative_path);
    row.sha256 = digest;
    row.byte_size = bytes.size();
    row.media_type = std::string(media_type);
    collected.push_back(std::move(row));
    return true;
}

std::map<std::string, std::size_t> count_severities(
    const std::vector<AnalysisFinding>& findings) {
    std::map<std::string, std::size_t> counts{
        {"info", 0}, {"low", 0}, {"medium", 0}, {"high", 0}, {"critical", 0}};
    for (const auto& finding : findings) ++counts[finding.severity];
    return counts;
}

std::string format_analyze_text(
    std::int64_t run_id,
    const ResolvedPackage& package,
    std::string_view artifact_sha256,
    bool integrity_verified,
    const AnalyzerWorkerResult& worker,
    const std::filesystem::path& evidence_directory,
    std::string_view status,
    bool reused) {
    const auto severities = count_severities(worker.findings);
    std::ostringstream out;
    out << "analysis_run_id=" << run_id << '\n'
        << "status=" << status << '\n'
        << "reused_existing=" << (reused ? "true" : "false") << '\n'
        << "server_identifier=" << package.server_identifier << '\n'
        << "server_version=" << package.server_version << '\n'
        << "registry_type=" << package.registry_type << '\n'
        << "package_identifier=" << package.package_identifier << '\n'
        << "package_version=" << package.package_version.value_or("") << '\n'
        << "artifact_sha256=" << artifact_sha256 << '\n'
        << "integrity_verified=" << (integrity_verified ? "true" : "false") << '\n'
        << "analyzed_file_count=" << worker.analyzed_file_count << '\n'
        << "dependency_count=" << worker.dependencies.size() << '\n'
        << "findings_info=" << severities.at("info") << '\n'
        << "findings_low=" << severities.at("low") << '\n'
        << "findings_medium=" << severities.at("medium") << '\n'
        << "findings_high=" << severities.at("high") << '\n'
        << "findings_critical=" << severities.at("critical") << '\n'
        << "lifecycle_scripts=";
    for (std::size_t i = 0U; i < worker.lifecycle_scripts.size(); ++i) {
        if (i) out << ',';
        out << worker.lifecycle_scripts[i].first;
    }
    out << '\n'
        << "native_code=" << (worker.has_native_code ? "true" : "false") << '\n'
        << "evidence_directory=" << evidence_directory.generic_string() << '\n';
    return out.str();
}

std::string format_analyze_json(
    std::int64_t run_id,
    const ResolvedPackage& package,
    std::string_view artifact_sha256,
    bool integrity_verified,
    const AnalyzerWorkerResult& worker,
    const std::filesystem::path& evidence_directory,
    std::string_view status,
    bool reused) {
    const auto severities = count_severities(worker.findings);
    std::ostringstream out;
    out << "{\"analysis_run_id\":" << run_id
        << ",\"status\":" << json_escape(status)
        << ",\"reused_existing\":" << (reused ? "true" : "false")
        << ",\"server_identifier\":" << json_escape(package.server_identifier)
        << ",\"server_version\":" << json_escape(package.server_version)
        << ",\"registry_type\":" << json_escape(package.registry_type)
        << ",\"package_identifier\":" << json_escape(package.package_identifier)
        << ",\"package_version\":"
        << json_escape(package.package_version.value_or(""))
        << ",\"artifact_sha256\":" << json_escape(artifact_sha256)
        << ",\"integrity_verified\":" << (integrity_verified ? "true" : "false")
        << ",\"analyzed_file_count\":" << worker.analyzed_file_count
        << ",\"dependency_count\":" << worker.dependencies.size()
        << ",\"findings_by_severity\":{"
        << "\"info\":" << severities.at("info")
        << ",\"low\":" << severities.at("low")
        << ",\"medium\":" << severities.at("medium")
        << ",\"high\":" << severities.at("high")
        << ",\"critical\":" << severities.at("critical")
        << "},\"lifecycle_scripts\":[";
    for (std::size_t i = 0U; i < worker.lifecycle_scripts.size(); ++i) {
        if (i) out << ',';
        out << json_escape(worker.lifecycle_scripts[i].first);
    }
    out << "],\"native_code\":" << (worker.has_native_code ? "true" : "false")
        << ",\"evidence_directory\":"
        << json_escape(evidence_directory.generic_string())
        << ",\"analyzer_name\":" << json_escape(package_analyzer_name)
        << ",\"analyzer_version\":" << json_escape(package_analyzer_version)
        << ",\"ruleset_version\":" << json_escape(worker.ruleset_version)
        << "}\n";
    return out.str();
}

bool ensure_schema_migrated(Database& database, std::string& error) {
    Statement schema;
    if (!schema.prepare(
            database, "SELECT schema_version FROM schema_info WHERE singleton=1;", error))
        return false;
    if (schema.step() != SQLITE_ROW) {
        error = "schema_info missing";
        return false;
    }
    const sqlite3_int64 version = schema.integer(0);
    if (version == 3) return true;
    if (version != 1 && version != 2) {
        error = "unsupported schema version";
        return false;
    }
    if (version == 1) {
        // Re-open through explorer index path is heavy; duplicate migration SQL here.
        static constexpr std::string_view analysis_sql = R"SQL(
CREATE TABLE IF NOT EXISTS analysis_runs(
    id INTEGER PRIMARY KEY,
    server_version_id INTEGER NOT NULL REFERENCES server_versions(id) ON DELETE RESTRICT,
    package_id INTEGER NOT NULL REFERENCES packages(id) ON DELETE RESTRICT,
    analysis_type TEXT NOT NULL CHECK(analysis_type IN ('npm_package_static_v1')),
    status TEXT NOT NULL CHECK(status IN ('running','completed','failed')),
    analyzer_name TEXT NOT NULL,
    analyzer_version TEXT NOT NULL,
    ruleset_version TEXT NOT NULL,
    started_at TEXT NOT NULL,
    completed_at TEXT,
    artifact_sha256 TEXT,
    published_integrity TEXT,
    integrity_verified INTEGER CHECK(integrity_verified IN (0, 1)),
    base_image_ref TEXT,
    base_image_digest TEXT,
    network_mode TEXT,
    container_read_only INTEGER CHECK(container_read_only IN (0, 1)),
    container_user TEXT,
    summary_json TEXT,
    error_stage TEXT,
    error_message TEXT
);
CREATE TABLE IF NOT EXISTS analysis_artifacts(
    id INTEGER PRIMARY KEY,
    analysis_run_id INTEGER NOT NULL UNIQUE REFERENCES analysis_runs(id) ON DELETE CASCADE,
    registry_type TEXT NOT NULL,
    package_identifier TEXT NOT NULL,
    package_version TEXT NOT NULL,
    source_url TEXT NOT NULL,
    local_relative_path TEXT NOT NULL,
    byte_size INTEGER NOT NULL CHECK(byte_size >= 0),
    sha256 TEXT NOT NULL,
    published_integrity TEXT NOT NULL,
    integrity_verified INTEGER NOT NULL CHECK(integrity_verified IN (0, 1)),
    downloaded_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS analysis_findings(
    id INTEGER PRIMARY KEY,
    analysis_run_id INTEGER NOT NULL REFERENCES analysis_runs(id) ON DELETE CASCADE,
    rule_id TEXT NOT NULL,
    category TEXT NOT NULL,
    severity TEXT NOT NULL CHECK(severity IN ('info','low','medium','high','critical')),
    confidence TEXT NOT NULL CHECK(confidence IN ('low','medium','high')),
    disposition TEXT NOT NULL CHECK(disposition IN (
        'unreviewed','expected','reviewed-benign','mitigated',
        'suspicious','confirmed-risk','false-positive')),
    subject_path TEXT NOT NULL,
    line_number INTEGER CHECK(line_number IS NULL OR line_number > 0),
    symbol TEXT,
    title TEXT NOT NULL,
    evidence TEXT,
    explanation TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS analysis_files(
    id INTEGER PRIMARY KEY,
    analysis_run_id INTEGER NOT NULL REFERENCES analysis_runs(id) ON DELETE CASCADE,
    archive_path TEXT NOT NULL,
    file_type TEXT NOT NULL,
    byte_size INTEGER NOT NULL CHECK(byte_size >= 0),
    sha256 TEXT NOT NULL,
    executable INTEGER NOT NULL CHECK(executable IN (0, 1)),
    native_binary INTEGER NOT NULL CHECK(native_binary IN (0, 1)),
    generated INTEGER NOT NULL CHECK(generated IN (0, 1)),
    minified INTEGER NOT NULL CHECK(minified IN (0, 1)),
    UNIQUE(analysis_run_id, archive_path)
);
CREATE TABLE IF NOT EXISTS analysis_dependencies(
    id INTEGER PRIMARY KEY,
    analysis_run_id INTEGER NOT NULL REFERENCES analysis_runs(id) ON DELETE CASCADE,
    dependency_type TEXT NOT NULL CHECK(dependency_type IN ('runtime','development','peer','optional')),
    dependency_name TEXT NOT NULL,
    declared_version TEXT NOT NULL,
    resolved_version TEXT,
    direct INTEGER NOT NULL CHECK(direct IN (0, 1)),
    development INTEGER NOT NULL CHECK(development IN (0, 1))
);
CREATE TABLE IF NOT EXISTS analysis_evidence(
    id INTEGER PRIMARY KEY,
    analysis_run_id INTEGER NOT NULL REFERENCES analysis_runs(id) ON DELETE CASCADE,
    evidence_type TEXT NOT NULL,
    relative_path TEXT NOT NULL,
    sha256 TEXT NOT NULL,
    byte_size INTEGER NOT NULL CHECK(byte_size >= 0),
    media_type TEXT NOT NULL,
    UNIQUE(analysis_run_id, relative_path)
);
CREATE INDEX IF NOT EXISTS analysis_runs_package ON analysis_runs(package_id, status);
CREATE INDEX IF NOT EXISTS analysis_runs_artifact ON analysis_runs(
    artifact_sha256, analyzer_version, ruleset_version, status);
CREATE INDEX IF NOT EXISTS analysis_findings_run ON analysis_findings(analysis_run_id);
CREATE INDEX IF NOT EXISTS analysis_findings_rule ON analysis_findings(rule_id);
CREATE INDEX IF NOT EXISTS analysis_files_run ON analysis_files(analysis_run_id);
CREATE INDEX IF NOT EXISTS analysis_dependencies_run ON analysis_dependencies(analysis_run_id);
CREATE INDEX IF NOT EXISTS analysis_evidence_run ON analysis_evidence(analysis_run_id);
)SQL";
        if (!database.execute(analysis_sql, error)) return false;
    }
    static constexpr std::string_view review_sql = R"SQL(
CREATE TABLE IF NOT EXISTS analysis_finding_reviews(
    id INTEGER PRIMARY KEY,
    finding_id INTEGER NOT NULL REFERENCES analysis_findings(id) ON DELETE CASCADE,
    previous_disposition TEXT NOT NULL CHECK(previous_disposition IN (
        'unreviewed','expected','reviewed-benign','mitigated',
        'suspicious','confirmed-risk','false-positive')),
    disposition TEXT NOT NULL CHECK(disposition IN (
        'expected','reviewed-benign','mitigated',
        'suspicious','confirmed-risk','false-positive')),
    reviewer TEXT NOT NULL CHECK(length(reviewer) BETWEEN 1 AND 200),
    reviewed_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS analysis_finding_reviews_finding
ON analysis_finding_reviews(finding_id, id);
)SQL";
    if (!database.execute(review_sql, error)) return false;
    Statement bump;
    return bump.prepare(
               database,
               "UPDATE schema_info SET schema_version=3 WHERE singleton=1;",
               error) &&
        bump.step_done(database, error);
}

bool insert_failed_run(
    Database& database,
    const ResolvedPackage& package,
    std::string_view ruleset_version,
    std::string_view stage,
    std::string_view message,
    std::optional<std::string_view> artifact_sha256,
    std::optional<std::string_view> published_integrity,
    std::int64_t& run_id,
    std::string& error) {
    Statement insert;
    if (!insert.prepare(
            database,
            "INSERT INTO analysis_runs("
            "server_version_id,package_id,analysis_type,status,analyzer_name,"
            "analyzer_version,ruleset_version,started_at,completed_at,"
            "artifact_sha256,published_integrity,integrity_verified,"
            "error_stage,error_message)"
            " VALUES(?1,?2,?3,'failed',?4,?5,?6,?7,?8,?9,?10,0,?11,?12);",
            error) ||
        !insert.bind_int64(1, package.server_version_id, error) ||
        !insert.bind_int64(2, package.package_id, error) ||
        !insert.bind_text(3, analysis_type_v1, error) ||
        !insert.bind_text(4, package_analyzer_name, error) ||
        !insert.bind_text(5, package_analyzer_version, error) ||
        !insert.bind_text(6, ruleset_version, error))
        return false;
    const std::string now = utc_now();
    if (!insert.bind_text(7, now, error) || !insert.bind_text(8, now, error))
        return false;
    if (artifact_sha256) {
        if (!insert.bind_text(9, *artifact_sha256, error)) return false;
    } else if (!insert.bind_null(9, error))
        return false;
    if (published_integrity) {
        if (!insert.bind_text(10, *published_integrity, error)) return false;
    } else if (!insert.bind_null(10, error))
        return false;
    const std::string bounded = bound_snippet(message, 1000U);
    if (!insert.bind_text(11, stage, error) ||
        !insert.bind_text(12, bounded, error) ||
        !insert.step_done(database, error))
        return false;
    run_id = sqlite3_last_insert_rowid(database.get());
    return true;
}

}  // namespace

AnalyzePackageResult analyze_package(
    const AnalyzePackageOptions& options,
    AnalyzeDownloadTransport download,
    AnalyzeWorkerRunner worker) {
    if (options.database.empty() || options.server_identifier.empty() ||
        options.server_version.empty() || options.package_identifier.empty())
        return failure_result(
            AnalyzeError::invalid_arguments,
            "analyze package requires --database --server --version --package");

    ResolvedPackage package;
    std::string error;
    const AnalyzeError resolved = resolve_exact_package(
        options.database,
        options.server_identifier,
        options.server_version,
        options.package_identifier,
        package,
        error);
    if (resolved != AnalyzeError::none) return failure_result(resolved, error);

    AnalysisRules rules;
    std::string rules_json;
    if (!load_analysis_rules(options.rules_path, rules, error, &rules_json))
        return failure_result(AnalyzeError::validation, error);
    const std::string& ruleset_version = rules.ruleset_version;

    AnalyzeDownloadTransport download_fn =
        download ? download : AnalyzeDownloadTransport(default_download);
    AnalyzeWorkerRunner worker_fn = worker;
    if (!worker_fn) {
        if (options.allow_in_process_worker) {
            worker_fn = [&options](
                            std::string_view registry_type,
                            const std::filesystem::path& tarball,
                            const std::filesystem::path& rules_path,
                            const ArchiveLimits& limits,
                            AnalyzerWorkerResult& result,
                            std::string& raw_json,
                            std::string& error_text) {
                return in_process_worker(
                    registry_type,
                    tarball,
                    rules_path,
                    limits,
                    result,
                    raw_json,
                    error_text);
            };
        }
        else
            worker_fn = [&options](
                            std::string_view registry_type,
                            const std::filesystem::path& tarball,
                            const std::filesystem::path& rules_path,
                            const ArchiveLimits&,
                            AnalyzerWorkerResult& result,
                            std::string& raw_json,
                            std::string& error_text) {
                return docker_worker(
                    options,
                    registry_type,
                    tarball,
                    rules_path,
                    result,
                    raw_json,
                    error_text);
            };
    }

    Database database;
    if (!database.open(
            options.database,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX,
            error))
        return failure_result(AnalyzeError::database, error);
    Transaction migration(database);
    if (!migration.begin(error) || !ensure_schema_migrated(database, error) ||
        !migration.commit(error))
        return failure_result(AnalyzeError::incompatible_schema, error);

    const std::string version = *package.package_version;
    std::string metadata_url =
        package.registry_type == "pypi"
            ? options.pypi_registry_url
            : options.npm_registry_url;
    while (!metadata_url.empty() && metadata_url.back() == '/')
        metadata_url.pop_back();
    metadata_url += "/";
    if (package.registry_type == "pypi") {
        std::string normalized_name;
        if (!normalize_pypi_project_name(
                package.package_identifier, normalized_name, error))
            return failure_result(AnalyzeError::validation, error);
        metadata_url += normalized_name + "/" + version + "/json";
    } else {
        for (char c : package.package_identifier) {
            if (c == '/') metadata_url += "%2F";
            else metadata_url.push_back(c);
        }
        metadata_url += "/" + version;
    }

    std::string metadata_body;
    if (!download_fn(
            metadata_url,
            options.download_timeout,
            2U * 1024U * 1024U,
            metadata_body,
            error)) {
        std::int64_t run_id{};
        Transaction tx(database);
        if (tx.begin(error) &&
            insert_failed_run(
                database, package, ruleset_version, "download_metadata", error,
                std::nullopt, std::nullopt, run_id, error))
            tx.commit(error);
        return failure_result(AnalyzeError::download, error);
    }

    std::string artifact_url;
    std::string published_integrity;
    std::uint64_t published_size{};
    if (package.registry_type == "pypi") {
        ArtifactDescriptor descriptor;
        AcquisitionLimits acquisition_limits;
        acquisition_limits.maximum_artifact_bytes =
            static_cast<std::uint64_t>(options.limits.maximum_tarball_bytes);
        if (parse_pypi_release_metadata(
                metadata_body,
                package.package_identifier,
                version,
                acquisition_limits,
                descriptor,
                error)) {
            artifact_url = descriptor.download_url;
            published_integrity = "sha256:" + descriptor.sha256;
            published_size = descriptor.published_size;
        }
    } else {
        NpmDistMetadata metadata;
        if (parse_npm_version_metadata(
                metadata_body,
                package.package_identifier,
                version,
                metadata,
                error)) {
            artifact_url = metadata.tarball_url;
            published_integrity = metadata.integrity;
        }
    }
    if (artifact_url.empty()) {
        std::int64_t run_id{};
        Transaction tx(database);
        if (tx.begin(error) &&
            insert_failed_run(
                database, package, ruleset_version, "parse_metadata", error,
                std::nullopt, std::nullopt, run_id, error))
            tx.commit(error);
        return failure_result(AnalyzeError::validation, error);
    }
    if (package.registry_type == "pypi" &&
        (options.pypi_registry_url.starts_with("http://127.0.0.1:") ||
         options.pypi_registry_url.starts_with("http://localhost:"))) {
        const auto origin = [](std::string_view url) {
            const auto scheme = url.find("://");
            const auto slash = scheme == std::string_view::npos
                ? std::string_view::npos
                : url.find('/', scheme + 3U);
            return slash == std::string_view::npos ? url : url.substr(0U, slash);
        };
        if (origin(options.pypi_registry_url) != origin(artifact_url)) {
            return failure_result(
                AnalyzeError::validation,
                "insecure loopback PyPI artifact URL must match registry origin");
        }
    }

    std::string artifact_bytes;
    if (!download_fn(
            artifact_url,
            options.download_timeout,
            options.limits.maximum_tarball_bytes,
            artifact_bytes,
            error)) {
        std::int64_t run_id{};
        Transaction tx(database);
        if (tx.begin(error) &&
            insert_failed_run(
                database,
                package,
                ruleset_version,
                "download_tarball",
                error,
                std::nullopt,
                published_integrity,
                run_id,
                error))
            tx.commit(error);
        return failure_result(AnalyzeError::download, error);
    }

    std::string artifact_sha256;
    if (!sha256_hex(artifact_bytes, artifact_sha256, error))
        return failure_result(AnalyzeError::io, error);

    if (!options.force) {
        Statement existing;
        if (existing.prepare(
                database,
                "SELECT id,summary_json FROM analysis_runs "
                "WHERE artifact_sha256=?1 AND analyzer_version=?2 "
                "AND ruleset_version=?3 AND status='completed' "
                "AND analysis_type=?4 ORDER BY id DESC LIMIT 1;",
                error) &&
            existing.bind_text(1, artifact_sha256, error) &&
            existing.bind_text(2, package_analyzer_version, error) &&
            existing.bind_text(3, ruleset_version, error) &&
            existing.bind_text(4, analysis_type_v1, error) &&
            existing.step() == SQLITE_ROW) {
            const sqlite3_int64 run_id = existing.integer(0);
            AnalyzerWorkerResult worker_result;
            worker_result.ruleset_version = ruleset_version;
            worker_result.package_name = package.package_identifier;
            worker_result.package_version = version;
            Statement findings;
            if (findings.prepare(
                    database,
                    "SELECT rule_id,category,severity,confidence,disposition,"
                    "subject_path,line_number,symbol,title,evidence,explanation "
                    "FROM analysis_findings WHERE analysis_run_id=?1;",
                    error) &&
                findings.bind_int64(1, run_id, error)) {
                while (findings.step() == SQLITE_ROW) {
                    AnalysisFinding finding;
                    finding.rule_id = findings.text(0);
                    finding.category = findings.text(1);
                    finding.severity = findings.text(2);
                    finding.confidence = findings.text(3);
                    finding.disposition = findings.text(4);
                    finding.subject_path = findings.text(5);
                    if (!findings.is_null(6))
                        finding.line_number =
                            static_cast<std::uint32_t>(findings.integer(6));
                    if (!findings.is_null(7)) finding.symbol = findings.text(7);
                    finding.title = findings.text(8);
                    finding.evidence = findings.text(9);
                    finding.explanation = findings.text(10);
                    worker_result.findings.push_back(std::move(finding));
                }
            }
            Statement files;
            if (files.prepare(
                    database,
                    "SELECT archive_path,file_type,byte_size,sha256,executable,"
                    "native_binary,generated,minified "
                    "FROM analysis_files WHERE analysis_run_id=?1;",
                    error) &&
                files.bind_int64(1, run_id, error)) {
                while (files.step() == SQLITE_ROW) {
                    AnalysisFileRecord file;
                    file.archive_path = files.text(0);
                    file.file_type = files.text(1);
                    file.byte_size = static_cast<std::uint64_t>(files.integer(2));
                    file.sha256 = files.text(3);
                    file.executable = files.integer(4) != 0;
                    file.native_binary = files.integer(5) != 0;
                    file.generated = files.integer(6) != 0;
                    file.minified = files.integer(7) != 0;
                    if (file.native_binary) worker_result.has_native_code = true;
                    worker_result.files.push_back(std::move(file));
                }
            }
            worker_result.analyzed_file_count = worker_result.files.size();
            Statement deps;
            if (deps.prepare(
                    database,
                    "SELECT dependency_type,dependency_name,declared_version,"
                    "resolved_version,direct,development "
                    "FROM analysis_dependencies WHERE analysis_run_id=?1;",
                    error) &&
                deps.bind_int64(1, run_id, error)) {
                while (deps.step() == SQLITE_ROW) {
                    AnalysisDependency dependency;
                    dependency.dependency_type = deps.text(0);
                    dependency.dependency_name = deps.text(1);
                    dependency.declared_version = deps.text(2);
                    if (!deps.is_null(3)) dependency.resolved_version = deps.text(3);
                    dependency.direct = deps.integer(4) != 0;
                    dependency.development = deps.integer(5) != 0;
                    if (dependency.dependency_type == "development" ||
                        dependency.development) {
                        // keep
                    }
                    worker_result.dependencies.push_back(std::move(dependency));
                }
            }
            for (const auto& finding : worker_result.findings) {
                for (const LifecycleRule& lifecycle : rules.lifecycles) {
                    if (finding.rule_id ==
                        rules.lifecycle_defaults.rule_id_prefix +
                            lifecycle.name) {
                        worker_result.lifecycle_scripts.emplace_back(
                            lifecycle.name, finding.evidence);
                        break;
                    }
                }
            }
            const auto evidence_directory =
                evidence_dir_for(options.evidence_root, artifact_sha256);
            AnalyzePackageResult reused;
            reused.analysis_run_id = run_id;
            reused.artifact_sha256 = artifact_sha256;
            reused.evidence_directory = evidence_directory;
            reused.reused_existing = true;
            reused.output =
                options.format == AnalyzeOutputFormat::json
                    ? format_analyze_json(
                          run_id,
                          package,
                          artifact_sha256,
                          true,
                          worker_result,
                          evidence_directory,
                          "completed",
                          true)
                    : format_analyze_text(
                          run_id,
                          package,
                          artifact_sha256,
                          true,
                          worker_result,
                          evidence_directory,
                          "completed",
                          true);
            return reused;
        }
    }

    const bool integrity_ok =
        package.registry_type == "pypi"
            ? verify_pypi_integrity(
                  artifact_bytes,
                  published_integrity.substr(std::string_view("sha256:").size()),
                  published_size,
                  error)
            : verify_npm_integrity(artifact_bytes, published_integrity, error);
    if (!integrity_ok) {
        std::int64_t run_id{};
        Transaction tx(database);
        if (tx.begin(error) &&
            insert_failed_run(
                database,
                package,
                ruleset_version,
                "integrity",
                error,
                artifact_sha256,
                published_integrity,
                run_id,
                error))
            tx.commit(error);
        return failure_result(AnalyzeError::integrity, error);
    }

    const auto staging = std::filesystem::temp_directory_path() /
        ("mcpo-analyze-" + std::to_string(getpid()));
    std::error_code ec;
    std::filesystem::create_directories(staging, ec);
    if (ec || !replace_permissions(
                  staging,
                  std::filesystem::perms::owner_all,
                  error)) {
        if (ec)
            error =
                "cannot create analysis staging directory " + staging.string() +
                ": " + ec.message();
        std::filesystem::remove_all(staging, ec);
        return failure_result(AnalyzeError::io, error);
    }
    const auto staged_tarball = staging / "artifact.tgz";
    const auto staged_rules = staging / "analysis-rules.json";
    if (!write_file_bytes(staged_tarball, artifact_bytes, error) ||
        !write_file_bytes(staged_rules, rules_json, error)) {
        std::filesystem::remove_all(staging, ec);
        return failure_result(AnalyzeError::io, error);
    }
    constexpr auto container_input_permissions =
        std::filesystem::perms::owner_read |
        std::filesystem::perms::group_read |
        std::filesystem::perms::others_read;
    if (!replace_permissions(
            staged_tarball, container_input_permissions, error) ||
        !replace_permissions(
            staged_rules, container_input_permissions, error)) {
        std::filesystem::remove_all(staging, ec);
        return failure_result(AnalyzeError::io, error);
    }

    AnalyzerWorkerResult worker_result;
    std::string raw_json;
    if (!worker_fn(
            package.registry_type,
            staged_tarball,
            staged_rules,
            options.limits,
            worker_result,
            raw_json,
            error)) {
        std::int64_t run_id{};
        Transaction tx(database);
        if (tx.begin(error) &&
            insert_failed_run(
                database,
                package,
                ruleset_version,
                "analyze_container",
                error,
                artifact_sha256,
                published_integrity,
                run_id,
                error))
            tx.commit(error);
        std::filesystem::remove_all(staging, ec);
        return failure_result(AnalyzeError::container, error);
    }
    if (worker_result.package_name.empty()) {
        if (!parse_analyzer_worker_json(raw_json, options.limits, worker_result, error)) {
            std::filesystem::remove_all(staging, ec);
            return failure_result(AnalyzeError::validation, error);
        }
    }
    if (worker_result.ruleset_version != ruleset_version) {
        std::filesystem::remove_all(staging, ec);
        return failure_result(
            AnalyzeError::validation,
            "analyzer worker ruleset_version does not match host ruleset");
    }
    bool worker_identity_matches =
        worker_result.package_version == version;
    if (package.registry_type == "pypi") {
        std::string expected_name;
        std::string worker_name;
        worker_identity_matches =
            worker_identity_matches &&
            normalize_pypi_project_name(
                package.package_identifier, expected_name, error) &&
            normalize_pypi_project_name(
                worker_result.package_name, worker_name, error) &&
            expected_name == worker_name;
    } else {
        worker_identity_matches =
            worker_identity_matches &&
            worker_result.package_name == package.package_identifier;
    }
    if (!worker_identity_matches) {
        std::filesystem::remove_all(staging, ec);
        return failure_result(
            AnalyzeError::validation,
            error.empty() ? "analyzed package identity mismatch" : error);
    }

    const auto evidence_directory =
        evidence_dir_for(options.evidence_root, artifact_sha256);
    std::filesystem::create_directories(evidence_directory, ec);
    if (!write_file_bytes(evidence_directory / "artifact.tgz", artifact_bytes, error) ||
        !write_file_bytes(
            evidence_directory / "analysis-rules.json", rules_json, error) ||
        !write_file_bytes(
            evidence_directory / "registry-metadata.json", metadata_body, error) ||
        !write_file_bytes(
            evidence_directory / "archive-inventory.json",
            worker_result.archive_inventory_json,
            error) ||
        !write_file_bytes(
            evidence_directory / "package-manifest.json",
            worker_result.package_manifest_json,
            error) ||
        !write_file_bytes(
            evidence_directory / "analysis-summary.json",
            worker_result.summary_json,
            error)) {
        std::filesystem::remove_all(staging, ec);
        return failure_result(AnalyzeError::io, error);
    }
    {
        std::ostringstream files_jsonl;
        for (const auto& file : worker_result.files) {
            files_jsonl << "{\"archive_path\":" << json_escape(file.archive_path)
                        << ",\"file_type\":" << json_escape(file.file_type)
                        << ",\"byte_size\":" << file.byte_size
                        << ",\"sha256\":" << json_escape(file.sha256) << "}\n";
        }
        if (!write_file_bytes(
                evidence_directory / "files.jsonl", files_jsonl.str(), error)) {
            std::filesystem::remove_all(staging, ec);
            return failure_result(AnalyzeError::io, error);
        }
    }
    {
        std::ostringstream deps;
        deps << '[';
        for (std::size_t i = 0U; i < worker_result.dependencies.size(); ++i) {
            const auto& dep = worker_result.dependencies[i];
            if (i) deps << ',';
            deps << "{\"dependency_type\":" << json_escape(dep.dependency_type)
                 << ",\"dependency_name\":" << json_escape(dep.dependency_name)
                 << ",\"declared_version\":" << json_escape(dep.declared_version)
                 << '}';
        }
        deps << ']';
        if (!write_file_bytes(
                evidence_directory / "dependencies.json", deps.str(), error)) {
            std::filesystem::remove_all(staging, ec);
            return failure_result(AnalyzeError::io, error);
        }
    }
    {
        std::ostringstream findings_jsonl;
        for (const auto& finding : worker_result.findings)
            findings_jsonl << finding_to_json(finding) << '\n';
        if (!write_file_bytes(
                evidence_directory / "findings.jsonl", findings_jsonl.str(), error) ||
            !write_file_bytes(
                evidence_directory / "analyzer.log",
                bound_snippet(raw_json, 64U * 1024U),
                error)) {
            std::filesystem::remove_all(staging, ec);
            return failure_result(AnalyzeError::io, error);
        }
    }

    Transaction tx(database);
    if (!tx.begin(error)) {
        std::filesystem::remove_all(staging, ec);
        return failure_result(AnalyzeError::database, error);
    }
    Statement insert_run;
    const std::string started = utc_now();
    if (!insert_run.prepare(
            database,
            "INSERT INTO analysis_runs("
            "server_version_id,package_id,analysis_type,status,analyzer_name,"
            "analyzer_version,ruleset_version,started_at,completed_at,"
            "artifact_sha256,published_integrity,integrity_verified,"
            "base_image_ref,base_image_digest,network_mode,container_read_only,"
            "container_user,summary_json)"
            " VALUES(?1,?2,?3,'completed',?4,?5,?6,?7,?8,?9,?10,1,?11,?12,?13,1,?14,?15);",
            error) ||
        !insert_run.bind_int64(1, package.server_version_id, error) ||
        !insert_run.bind_int64(2, package.package_id, error) ||
        !insert_run.bind_text(3, analysis_type_v1, error) ||
        !insert_run.bind_text(4, package_analyzer_name, error) ||
        !insert_run.bind_text(5, package_analyzer_version, error) ||
        !insert_run.bind_text(6, ruleset_version, error) ||
        !insert_run.bind_text(7, started, error) ||
        !insert_run.bind_text(8, started, error) ||
        !insert_run.bind_text(9, artifact_sha256, error) ||
        !insert_run.bind_text(10, published_integrity, error) ||
        !insert_run.bind_text(11, "debian:bookworm-slim", error) ||
        !insert_run.bind_null(12, error) ||
        !insert_run.bind_text(13, "none", error) ||
        !insert_run.bind_text(14, "65532:65532", error) ||
        !insert_run.bind_text(15, worker_result.summary_json, error) ||
        !insert_run.step_done(database, error)) {
        std::filesystem::remove_all(staging, ec);
        return failure_result(AnalyzeError::database, error);
    }
    const sqlite3_int64 run_id = sqlite3_last_insert_rowid(database.get());
    const std::filesystem::path relative_artifact =
        std::filesystem::path("artifacts") / "sha256" /
        artifact_sha256.substr(0U, 2U) / artifact_sha256 / "artifact.tgz";
    Statement insert_artifact;
    if (!insert_artifact.prepare(
            database,
            "INSERT INTO analysis_artifacts("
            "analysis_run_id,registry_type,package_identifier,package_version,"
            "source_url,local_relative_path,byte_size,sha256,published_integrity,"
            "integrity_verified,downloaded_at)"
            " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,1,?10);",
            error) ||
        !insert_artifact.bind_int64(1, run_id, error) ||
        !insert_artifact.bind_text(2, package.registry_type, error) ||
        !insert_artifact.bind_text(3, package.package_identifier, error) ||
        !insert_artifact.bind_text(4, version, error) ||
        !insert_artifact.bind_text(5, artifact_url, error) ||
        !insert_artifact.bind_text(6, relative_posix(relative_artifact), error) ||
        !insert_artifact.bind_int64(
            7, static_cast<sqlite3_int64>(artifact_bytes.size()), error) ||
        !insert_artifact.bind_text(8, artifact_sha256, error) ||
        !insert_artifact.bind_text(9, published_integrity, error) ||
        !insert_artifact.bind_text(10, started, error) ||
        !insert_artifact.step_done(database, error)) {
        std::filesystem::remove_all(staging, ec);
        return failure_result(AnalyzeError::database, error);
    }

    Statement insert_finding;
    if (!insert_finding.prepare(
            database,
            "INSERT INTO analysis_findings("
            "analysis_run_id,rule_id,category,severity,confidence,disposition,"
            "subject_path,line_number,symbol,title,evidence,explanation)"
            " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12);",
            error)) {
        std::filesystem::remove_all(staging, ec);
        return failure_result(AnalyzeError::database, error);
    }
    for (const auto& finding : worker_result.findings) {
        insert_finding.reset();
        if (!insert_finding.bind_int64(1, run_id, error) ||
            !insert_finding.bind_text(2, finding.rule_id, error) ||
            !insert_finding.bind_text(3, finding.category, error) ||
            !insert_finding.bind_text(4, finding.severity, error) ||
            !insert_finding.bind_text(5, finding.confidence, error) ||
            !insert_finding.bind_text(6, finding.disposition, error) ||
            !insert_finding.bind_text(7, finding.subject_path, error)) {
            std::filesystem::remove_all(staging, ec);
            return failure_result(AnalyzeError::database, error);
        }
        if (finding.line_number) {
            if (!insert_finding.bind_int64(
                    8, static_cast<sqlite3_int64>(*finding.line_number), error)) {
                std::filesystem::remove_all(staging, ec);
                return failure_result(AnalyzeError::database, error);
            }
        } else if (!insert_finding.bind_null(8, error)) {
            std::filesystem::remove_all(staging, ec);
            return failure_result(AnalyzeError::database, error);
        }
        if (finding.symbol) {
            if (!insert_finding.bind_text(9, *finding.symbol, error)) {
                std::filesystem::remove_all(staging, ec);
                return failure_result(AnalyzeError::database, error);
            }
        } else if (!insert_finding.bind_null(9, error)) {
            std::filesystem::remove_all(staging, ec);
            return failure_result(AnalyzeError::database, error);
        }
        if (!insert_finding.bind_text(10, finding.title, error) ||
            !insert_finding.bind_text(11, finding.evidence, error) ||
            !insert_finding.bind_text(12, finding.explanation, error) ||
            !insert_finding.step_done(database, error)) {
            std::filesystem::remove_all(staging, ec);
            return failure_result(AnalyzeError::database, error);
        }
    }

    Statement insert_file;
    if (!insert_file.prepare(
            database,
            "INSERT INTO analysis_files("
            "analysis_run_id,archive_path,file_type,byte_size,sha256,"
            "executable,native_binary,generated,minified)"
            " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9);",
            error)) {
        std::filesystem::remove_all(staging, ec);
        return failure_result(AnalyzeError::database, error);
    }
    for (const auto& file : worker_result.files) {
        insert_file.reset();
        if (!insert_file.bind_int64(1, run_id, error) ||
            !insert_file.bind_text(2, file.archive_path, error) ||
            !insert_file.bind_text(3, file.file_type, error) ||
            !insert_file.bind_int64(
                4, static_cast<sqlite3_int64>(file.byte_size), error) ||
            !insert_file.bind_text(5, file.sha256, error) ||
            !insert_file.bind_int64(6, file.executable ? 1 : 0, error) ||
            !insert_file.bind_int64(7, file.native_binary ? 1 : 0, error) ||
            !insert_file.bind_int64(8, file.generated ? 1 : 0, error) ||
            !insert_file.bind_int64(9, file.minified ? 1 : 0, error) ||
            !insert_file.step_done(database, error)) {
            std::filesystem::remove_all(staging, ec);
            return failure_result(AnalyzeError::database, error);
        }
    }

    Statement insert_dep;
    if (!insert_dep.prepare(
            database,
            "INSERT INTO analysis_dependencies("
            "analysis_run_id,dependency_type,dependency_name,declared_version,"
            "resolved_version,direct,development)"
            " VALUES(?1,?2,?3,?4,?5,?6,?7);",
            error)) {
        std::filesystem::remove_all(staging, ec);
        return failure_result(AnalyzeError::database, error);
    }
    for (const auto& dep : worker_result.dependencies) {
        insert_dep.reset();
        if (!insert_dep.bind_int64(1, run_id, error) ||
            !insert_dep.bind_text(2, dep.dependency_type, error) ||
            !insert_dep.bind_text(3, dep.dependency_name, error) ||
            !insert_dep.bind_text(4, dep.declared_version, error)) {
            std::filesystem::remove_all(staging, ec);
            return failure_result(AnalyzeError::database, error);
        }
        if (dep.resolved_version) {
            if (!insert_dep.bind_text(5, *dep.resolved_version, error)) {
                std::filesystem::remove_all(staging, ec);
                return failure_result(AnalyzeError::database, error);
            }
        } else if (!insert_dep.bind_null(5, error)) {
            std::filesystem::remove_all(staging, ec);
            return failure_result(AnalyzeError::database, error);
        }
        if (!insert_dep.bind_int64(6, dep.direct ? 1 : 0, error) ||
            !insert_dep.bind_int64(7, dep.development ? 1 : 0, error) ||
            !insert_dep.step_done(database, error)) {
            std::filesystem::remove_all(staging, ec);
            return failure_result(AnalyzeError::database, error);
        }
    }

    std::vector<AnalysisEvidenceFile> evidence_rows;
    const std::vector<std::tuple<std::string, std::string, std::string>> evidence_specs{
        {"artifact", "artifact.tgz", "application/gzip"},
        {"analysis_rules", "analysis-rules.json", "application/json"},
        {"registry_metadata", "registry-metadata.json", "application/json"},
        {"archive_inventory", "archive-inventory.json", "application/json"},
        {"package_manifest", "package-manifest.json", "application/json"},
        {"files", "files.jsonl", "application/x-ndjson"},
        {"dependencies", "dependencies.json", "application/json"},
        {"findings", "findings.jsonl", "application/x-ndjson"},
        {"summary", "analysis-summary.json", "application/json"},
        {"analyzer_log", "analyzer.log", "text/plain"}};
    for (const auto& [type, name, media] : evidence_specs) {
        if (!record_evidence_file(
                database,
                run_id,
                type,
                evidence_directory / name,
                std::filesystem::path("artifacts") / "sha256" /
                    artifact_sha256.substr(0U, 2U) / artifact_sha256 / name,
                media,
                evidence_rows,
                error)) {
            std::filesystem::remove_all(staging, ec);
            return failure_result(AnalyzeError::database, error);
        }
    }
    if (!tx.commit(error)) {
        std::filesystem::remove_all(staging, ec);
        return failure_result(AnalyzeError::database, error);
    }
    std::filesystem::remove_all(staging, ec);

    AnalyzePackageResult ok;
    ok.analysis_run_id = run_id;
    ok.artifact_sha256 = artifact_sha256;
    ok.evidence_directory = evidence_directory;
    ok.output =
        options.format == AnalyzeOutputFormat::json
            ? format_analyze_json(
                  run_id,
                  package,
                  artifact_sha256,
                  true,
                  worker_result,
                  evidence_directory,
                  "completed",
                  false)
            : format_analyze_text(
                  run_id,
                  package,
                  artifact_sha256,
                  true,
                  worker_result,
                  evidence_directory,
                  "completed",
                  false);
    return ok;
}

FindingOperationResult read_finding_source(
    const FindingSourceOptions& options) {
    const auto failure = [](FindingOperationError code, std::string message) {
        FindingOperationResult result;
        result.error = code;
        result.output = std::move(message);
        return result;
    };
    if (options.database.empty() || options.evidence_root.empty() ||
        options.finding_id <= 0 || options.maximum_source_bytes == 0U) {
        return failure(
            FindingOperationError::invalid_arguments,
            "database, evidence root, and a positive finding id are required");
    }

    Database database;
    std::string error;
    if (!database.open(
            options.database, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, error) ||
        !database.execute("PRAGMA query_only = ON;", error)) {
        return failure(FindingOperationError::database, error);
    }
    Statement schema;
    if (!schema.prepare(
            database,
            "SELECT schema_version FROM schema_info WHERE singleton=1;",
            error) ||
        schema.step() != SQLITE_ROW) {
        return failure(FindingOperationError::incompatible_schema, error);
    }
    const sqlite3_int64 schema_version = schema.integer(0);
    if (schema_version != 2 && schema_version != 3) {
        return failure(
            FindingOperationError::incompatible_schema,
            "finding source requires Observatory schema version 2 or 3");
    }

    Statement select;
    if (!select.prepare(
            database,
            "SELECT af.analysis_run_id,af.subject_path,af.line_number,"
            "ar.artifact_sha256,f.byte_size,f.sha256,"
            "ae.relative_path,ae.sha256,ae.byte_size,af.evidence,af.symbol "
            "FROM analysis_findings af "
            "JOIN analysis_runs ar ON ar.id=af.analysis_run_id "
            "LEFT JOIN analysis_files f "
            "ON f.analysis_run_id=af.analysis_run_id "
            "AND f.archive_path=af.subject_path "
            "LEFT JOIN analysis_evidence ae "
            "ON ae.analysis_run_id=af.analysis_run_id "
            "AND ae.evidence_type='artifact' "
            "WHERE af.id=?1 ORDER BY ae.id LIMIT 1;",
            error) ||
        !select.bind_int64(1, options.finding_id, error)) {
        return failure(FindingOperationError::database, error);
    }
    const int selected = select.step();
    if (selected == SQLITE_DONE) {
        return failure(
            FindingOperationError::finding_not_found, "finding not found");
    }
    if (selected != SQLITE_ROW) {
        return failure(
            FindingOperationError::database,
            sqlite_message(database.get(), "read finding source metadata"));
    }
    if (select.is_null(3) || select.is_null(4) || select.is_null(5) ||
        select.is_null(6) || select.is_null(7) || select.is_null(8)) {
        return failure(
            FindingOperationError::evidence,
            "finding source is not backed by a finalized artifact and file record");
    }

    const sqlite3_int64 run_id = select.integer(0);
    const std::string subject_path = select.text(1);
    const std::optional<sqlite3_int64> line_number =
        select.is_null(2) ? std::nullopt :
                            std::optional<sqlite3_int64>(select.integer(2));
    const std::string artifact_sha256 = select.text(3);
    const sqlite3_int64 recorded_file_size = select.integer(4);
    const std::string recorded_file_sha256 = select.text(5);
    const std::string evidence_relative_path = select.text(6);
    const std::string evidence_sha256 = select.text(7);
    const sqlite3_int64 evidence_size = select.integer(8);
    const std::string finding_evidence =
        select.is_null(9) ? std::string{} : select.text(9);
    const std::string finding_symbol =
        select.is_null(10) ? std::string{} : select.text(10);

    const auto valid_digest = [](std::string_view digest) {
        return digest.size() == 64U &&
            std::all_of(digest.begin(), digest.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            });
    };
    if (!valid_digest(artifact_sha256) ||
        !valid_digest(recorded_file_sha256) ||
        !valid_digest(evidence_sha256)) {
        return failure(
            FindingOperationError::evidence,
            "finding source metadata contains an invalid SHA-256 digest");
    }
    const std::string expected_relative =
        "artifacts/sha256/" + artifact_sha256.substr(0U, 2U) + "/" +
        artifact_sha256 + "/artifact.tgz";
    if (evidence_relative_path != expected_relative ||
        evidence_sha256 != artifact_sha256 || evidence_size < 0 ||
        static_cast<std::uint64_t>(evidence_size) >
            options.limits.maximum_tarball_bytes) {
        return failure(
            FindingOperationError::evidence,
            "artifact evidence metadata does not match its digest path");
    }

    std::error_code path_error;
    const std::filesystem::path evidence_root =
        std::filesystem::canonical(options.evidence_root, path_error);
    if (path_error || !std::filesystem::is_directory(evidence_root)) {
        return failure(
            FindingOperationError::io,
            "evidence root is not an accessible directory");
    }
    std::filesystem::path artifact_path = evidence_root;
    const std::filesystem::path relative_artifact(expected_relative);
    for (const auto& component : relative_artifact) {
        artifact_path /= component;
        const auto status =
            std::filesystem::symlink_status(artifact_path, path_error);
        if (path_error || std::filesystem::is_symlink(status)) {
            return failure(
                FindingOperationError::evidence,
                "artifact evidence path contains a missing or symbolic-link component");
        }
    }
    if (!std::filesystem::is_regular_file(artifact_path)) {
        return failure(
            FindingOperationError::evidence,
            "artifact evidence is not a regular file");
    }

    std::string artifact_bytes;
    if (!read_file_bytes(
            artifact_path,
            options.limits.maximum_tarball_bytes,
            artifact_bytes,
            error)) {
        return failure(FindingOperationError::io, error);
    }
    if (artifact_bytes.size() != static_cast<std::size_t>(evidence_size)) {
        return failure(
            FindingOperationError::evidence,
            "artifact evidence size does not match the catalog");
    }
    std::string actual_artifact_sha256;
    if (!sha256_hex(artifact_bytes, actual_artifact_sha256, error)) {
        return failure(FindingOperationError::io, error);
    }
    if (actual_artifact_sha256 != artifact_sha256) {
        return failure(
            FindingOperationError::evidence,
            "artifact evidence digest does not match the catalog");
    }

    constexpr std::size_t tar_overhead_allowance = 8U * 1024U * 1024U;
    if (options.limits.maximum_total_uncompressed_bytes >
        std::numeric_limits<std::size_t>::max() - tar_overhead_allowance) {
        return failure(
            FindingOperationError::limit_exceeded,
            "configured archive limit is too large");
    }
    std::string tar_bytes;
    if (!gunzip_bytes(
            artifact_bytes,
            options.limits.maximum_total_uncompressed_bytes +
                tar_overhead_allowance,
            tar_bytes,
            error)) {
        return failure(FindingOperationError::evidence, error);
    }
    std::vector<TarMember> members;
    if (!parse_ustar(tar_bytes, options.limits, members, error)) {
        return failure(FindingOperationError::evidence, error);
    }
    const TarMember* source = nullptr;
    for (const TarMember& member : members) {
        if (member.path != subject_path) continue;
        if (source != nullptr) {
            return failure(
                FindingOperationError::evidence,
                "archive contains duplicate finding source members");
        }
        source = &member;
    }
    if (source == nullptr || (source->typeflag != '0' && source->typeflag != '\0')) {
        return failure(
            FindingOperationError::evidence,
            "finding source is not a regular archive member");
    }
    if (recorded_file_size < 0 ||
        source->data.size() != static_cast<std::size_t>(recorded_file_size)) {
        return failure(
            FindingOperationError::evidence,
            "finding source size does not match the catalog");
    }
    std::string actual_file_sha256;
    if (!sha256_hex(source->data, actual_file_sha256, error)) {
        return failure(FindingOperationError::io, error);
    }
    if (actual_file_sha256 != recorded_file_sha256) {
        return failure(
            FindingOperationError::evidence,
            "finding source digest does not match the catalog");
    }
    if (!valid_utf8(source->data) ||
        source->data.find('\0') != std::string::npos) {
        return failure(
            FindingOperationError::evidence,
            "finding source is not displayable UTF-8 text");
    }

    if (options.raw_output) {
        FindingOperationResult result;
        result.output = source->data;
        return result;
    }

    std::size_t display_start = 0U;
    std::size_t display_end = source->data.size();
    if (source->data.size() > options.maximum_source_bytes) {
        std::size_t anchor = 0U;
        if (line_number) {
            std::size_t current_line = 1U;
            std::size_t line_start = 0U;
            while (current_line < static_cast<std::size_t>(*line_number) &&
                   line_start < source->data.size()) {
                const std::size_t newline =
                    source->data.find('\n', line_start);
                if (newline == std::string::npos) {
                    line_start = source->data.size();
                    break;
                }
                line_start = newline + 1U;
                ++current_line;
            }
            const std::size_t newline =
                source->data.find('\n', line_start);
            const std::size_t line_end =
                newline == std::string::npos ? source->data.size() : newline;
            anchor = line_start + (line_end - line_start) / 2U;
            std::string needle =
                finding_symbol.empty() ? finding_evidence : finding_symbol;
            if (needle.ends_with("...")) needle.resize(needle.size() - 3U);
            if (!needle.empty()) {
                const std::size_t match =
                    source->data.find(needle, line_start);
                if (match != std::string::npos && match < line_end)
                    anchor = match + needle.size() / 2U;
            }
        }
        const std::size_t half = options.maximum_source_bytes / 2U;
        display_start = anchor > half ? anchor - half : 0U;
        display_end = std::min(
            source->data.size(),
            display_start + options.maximum_source_bytes);
        if (display_end == source->data.size() &&
            display_end > options.maximum_source_bytes) {
            display_start = display_end - options.maximum_source_bytes;
        }
        const auto continuation = [](unsigned char byte) {
            return (byte & 0xc0U) == 0x80U;
        };
        while (display_start < display_end &&
               continuation(static_cast<unsigned char>(
                   source->data[display_start])))
            ++display_start;
        while (display_end > display_start &&
               display_end < source->data.size() &&
               continuation(static_cast<unsigned char>(
                   source->data[display_end])))
            --display_end;
    }
    const bool truncated_before = display_start > 0U;
    const bool truncated_after = display_end < source->data.size();
    const bool starts_mid_line =
        truncated_before && source->data[display_start - 1U] != '\n';
    const bool ends_mid_line =
        truncated_after && display_end > 0U &&
        source->data[display_end - 1U] != '\n';
    const std::size_t start_line =
        1U + static_cast<std::size_t>(std::count(
                 source->data.begin(),
                 source->data.begin() +
                     static_cast<std::ptrdiff_t>(display_start),
                 '\n'));
    const std::string_view displayed(
        source->data.data() + display_start,
        display_end - display_start);

    std::ostringstream output;
    if (options.format == AnalyzeOutputFormat::json) {
        output << "{\"status\":\"completed\",\"finding_id\":"
               << options.finding_id << ",\"analysis_run_id\":" << run_id
               << ",\"subject_path\":" << json_escape(subject_path)
               << ",\"line_number\":";
        if (line_number) output << *line_number;
        else output << "null";
        output << ",\"sha256\":" << json_escape(actual_file_sha256)
               << ",\"byte_size\":" << source->data.size()
               << ",\"displayed_byte_size\":" << displayed.size()
               << ",\"start_line\":" << start_line
               << ",\"truncated_before\":"
               << (truncated_before ? "true" : "false")
               << ",\"truncated_after\":"
               << (truncated_after ? "true" : "false")
               << ",\"starts_mid_line\":"
               << (starts_mid_line ? "true" : "false")
               << ",\"ends_mid_line\":"
               << (ends_mid_line ? "true" : "false")
               << ",\"content\":" << json_escape(displayed) << "}\n";
    } else {
        output << "finding_id=" << options.finding_id << '\n'
               << "analysis_run_id=" << run_id << '\n'
               << "subject_path=" << subject_path << '\n'
               << "line_number=";
        if (line_number) output << *line_number;
        output << "\nsha256=" << actual_file_sha256
               << "\ndisplayed_byte_size=" << displayed.size()
               << "\nstart_line=" << start_line
               << "\ntruncated_before="
               << (truncated_before ? "true" : "false")
               << "\ntruncated_after="
               << (truncated_after ? "true" : "false") << "\n\n"
               << displayed;
    }
    FindingOperationResult result;
    result.output = output.str();
    return result;
}

FindingOperationResult review_finding(
    const ReviewFindingOptions& options) {
    const auto failure = [](FindingOperationError code, std::string message) {
        FindingOperationResult result;
        result.error = code;
        result.output = std::move(message);
        return result;
    };
    static const std::set<std::string_view> all_dispositions{
        "unreviewed", "expected", "reviewed-benign", "mitigated",
        "suspicious", "confirmed-risk", "false-positive"};
    static const std::set<std::string_view> review_dispositions{
        "expected", "reviewed-benign", "mitigated",
        "suspicious", "confirmed-risk", "false-positive"};
    if (options.database.empty() || options.finding_id <= 0 ||
        !all_dispositions.contains(options.expected_disposition) ||
        !review_dispositions.contains(options.disposition) ||
        options.reviewer.empty() || options.reviewer.size() > 200U ||
        !valid_utf8(options.reviewer)) {
        return failure(
            FindingOperationError::invalid_arguments,
            "review requires a positive finding id, allowed dispositions, "
            "and a reviewer of at most 200 UTF-8 bytes");
    }

    Database database;
    std::string error;
    if (!database.open(
            options.database,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI,
            error)) {
        return failure(FindingOperationError::database, error);
    }
    Transaction transaction(database);
    if (!transaction.begin(error)) {
        return failure(FindingOperationError::database, error);
    }
    if (!ensure_schema_migrated(database, error)) {
        return failure(FindingOperationError::incompatible_schema, error);
    }

    Statement current;
    if (!current.prepare(
            database,
            "SELECT disposition FROM analysis_findings WHERE id=?1;",
            error) ||
        !current.bind_int64(1, options.finding_id, error)) {
        return failure(FindingOperationError::database, error);
    }
    const int selected = current.step();
    if (selected == SQLITE_DONE) {
        return failure(
            FindingOperationError::finding_not_found, "finding not found");
    }
    if (selected != SQLITE_ROW) {
        return failure(
            FindingOperationError::database,
            sqlite_message(database.get(), "read finding disposition"));
    }
    const std::string previous = current.text(0);
    if (previous != options.expected_disposition ||
        previous == options.disposition) {
        return failure(
            FindingOperationError::conflict,
            "finding disposition changed or already has the requested disposition");
    }

    const std::string reviewed_at = utc_now();
    Statement insert;
    if (!insert.prepare(
            database,
            "INSERT INTO analysis_finding_reviews("
            "finding_id,previous_disposition,disposition,reviewer,reviewed_at)"
            " VALUES(?1,?2,?3,?4,?5);",
            error) ||
        !insert.bind_int64(1, options.finding_id, error) ||
        !insert.bind_text(2, previous, error) ||
        !insert.bind_text(3, options.disposition, error) ||
        !insert.bind_text(4, options.reviewer, error) ||
        !insert.bind_text(5, reviewed_at, error) ||
        !insert.step_done(database, error)) {
        return failure(FindingOperationError::database, error);
    }
    const sqlite3_int64 review_id = sqlite3_last_insert_rowid(database.get());
    Statement update;
    if (!update.prepare(
            database,
            "UPDATE analysis_findings SET disposition=?1 "
            "WHERE id=?2 AND disposition=?3;",
            error) ||
        !update.bind_text(1, options.disposition, error) ||
        !update.bind_int64(2, options.finding_id, error) ||
        !update.bind_text(3, previous, error) ||
        !update.step_done(database, error)) {
        return failure(FindingOperationError::database, error);
    }
    if (sqlite3_changes(database.get()) != 1) {
        return failure(
            FindingOperationError::conflict,
            "finding disposition changed during review");
    }
    if (!transaction.commit(error)) {
        return failure(FindingOperationError::database, error);
    }

    std::ostringstream output;
    if (options.format == AnalyzeOutputFormat::json) {
        output << "{\"status\":\"completed\",\"review_id\":" << review_id
               << ",\"finding_id\":" << options.finding_id
               << ",\"previous_disposition\":" << json_escape(previous)
               << ",\"disposition\":" << json_escape(options.disposition)
               << ",\"reviewer\":" << json_escape(options.reviewer)
               << ",\"reviewed_at\":" << json_escape(reviewed_at) << "}\n";
    } else {
        output << "status=completed\nreview_id=" << review_id
               << "\nfinding_id=" << options.finding_id
               << "\nprevious_disposition=" << previous
               << "\ndisposition=" << options.disposition
               << "\nreviewer=" << options.reviewer
               << "\nreviewed_at=" << reviewed_at << '\n';
    }
    FindingOperationResult result;
    result.output = output.str();
    return result;
}

}  // namespace mcpo
