#pragma once

#include <cstddef>
#include <cstdint>
#include <istream>
#include <optional>
#include <string>
#include <string_view>

namespace mcpo {

enum class TargetKind {
    local_package,
    remote_endpoint
};

struct TargetRecord {
    std::uint32_t schema_version{};
    std::string target_id;
    std::string source_type;
    TargetKind kind{};
};

struct ReadLimits {
    std::size_t max_line_bytes{64U * 1024U};
    std::size_t max_records{10'000U};
};

struct ManifestSummary {
    std::size_t records{};
    std::size_t local_packages{};
    std::size_t remote_endpoints{};
};

struct ManifestError {
    std::size_t line{};
    std::string message;
};

struct ManifestResult {
    ManifestSummary summary;
    std::optional<ManifestError> error;

    [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

[[nodiscard]] ManifestResult read_target_manifest(
    std::istream& input,
    ReadLimits limits = {});

[[nodiscard]] std::string_view to_string(TargetKind kind) noexcept;

}  // namespace mcpo
