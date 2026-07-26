#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcpo {

inline constexpr std::string_view official_registry_base_url =
    "https://registry.modelcontextprotocol.io";

struct RegistryLimits {
    std::size_t maximum_pages{1'000U};
    std::size_t maximum_page_bytes{8U * 1024U * 1024U};
    std::size_t maximum_records{100'000U};
    std::size_t maximum_redirects{5U};
};

struct RegistryRuntimePolicy {
    std::chrono::seconds request_timeout{60};
    std::chrono::seconds stall_timeout{300};
    std::optional<std::chrono::seconds> run_timeout;
    std::size_t maximum_attempts_per_page{8U};
    std::chrono::seconds retry_initial{2};
    std::chrono::seconds retry_maximum{120};
};

struct RegistryUrl {
    std::string scheme;
    std::string host;
    unsigned port{};
    std::string base_path;
    std::string normalized;
};

struct RegistryCollectOptions {
    std::filesystem::path output;
    std::optional<std::filesystem::path> resume;
    std::string registry_base_url{official_registry_base_url};
    RegistryLimits limits{};
    RegistryRuntimePolicy runtime{};
    bool retain_raw{true};
    bool verbose{};
    std::chrono::milliseconds heartbeat_interval{std::chrono::seconds(5)};
    std::function<std::chrono::steady_clock::time_point()> now;
    std::function<void(std::chrono::steady_clock::duration)> wait;
};

enum class HttpFailureKind {
    none,
    timeout,
    temporary_network,
    tls,
    protocol,
    cancelled,
    response_too_large,
    local_io,
    other,
};

struct HttpResponse {
    unsigned status{};
    std::string content_type;
    std::string body;
    std::string effective_url;
    std::optional<std::string> location;
    std::optional<std::string> retry_after;
    HttpFailureKind failure_kind{HttpFailureKind::none};
};

struct HttpWaitDecision {
    bool continue_waiting{};
    std::chrono::steady_clock::duration next_wake{};
};

using HttpHeartbeat =
    std::function<HttpWaitDecision(std::chrono::milliseconds waiting)>;

using HttpTransport = std::function<bool(
    const std::string& url,
    std::chrono::steady_clock::duration timeout,
    std::size_t maximum_bytes,
    HttpResponse& response,
    std::string& error,
    const HttpHeartbeat& heartbeat,
    std::chrono::steady_clock::duration initial_wake)>;

[[nodiscard]] bool parse_registry_url(
    std::string_view text,
    RegistryUrl& result,
    std::string& error);

[[nodiscard]] bool registry_same_origin(
    const RegistryUrl& left,
    const RegistryUrl& right) noexcept;

[[nodiscard]] std::string registry_api_url(
    const RegistryUrl& base,
    std::optional<std::string_view> cursor);

[[nodiscard]] bool collect_registry(
    const RegistryCollectOptions& options,
    std::string& message,
    HttpTransport transport = {});

[[nodiscard]] bool reconstruct_registry_checkpoint(
    const std::filesystem::path& partial_bundle,
    std::string_view registry_base_url,
    RegistryLimits limits,
    std::string& message);

[[nodiscard]] bool validate_bundle(
    const std::filesystem::path& bundle,
    std::string& message);

[[nodiscard]] bool valid_utc_timestamp(std::string_view value) noexcept;

}  // namespace mcpo
