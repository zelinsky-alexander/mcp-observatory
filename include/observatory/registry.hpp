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
    unsigned request_timeout_seconds{30U};
    unsigned run_timeout_seconds{900U};
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
    bool retain_raw{true};
    bool verbose{};
};

struct HttpResponse {
    unsigned status{};
    std::string content_type;
    std::string body;
    std::string effective_url;
    std::optional<std::string> location;
};

using HttpTransport = std::function<bool(
    const std::string& url,
    unsigned timeout_seconds,
    std::size_t maximum_bytes,
    HttpResponse& response,
    std::string& error)>;

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
