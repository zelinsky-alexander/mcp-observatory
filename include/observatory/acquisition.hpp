#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mcpo {

enum class ArtifactRegistry {
    npm,
    pypi,
};

enum class ArtifactArchiveFormat {
    tar_gzip,
    zip,
};

struct ArtifactDescriptor {
    ArtifactRegistry registry{ArtifactRegistry::npm};
    ArtifactArchiveFormat archive_format{ArtifactArchiveFormat::tar_gzip};
    std::string package_name;
    std::string package_version;
    std::string filename;
    std::string download_url;
    std::string sha256;
    std::uint64_t published_size{};
};

struct NpmArtifactInput {
    std::string package_name;
    std::string package_version;
    std::string tarball_url;
    std::string sha256;
    std::uint64_t published_size{};
};

struct PypiReleaseFile {
    std::string filename;
    std::string package_type;
    std::string url;
    std::string sha256;
    std::uint64_t size{};
    bool yanked{};
};

struct AcquisitionLimits {
    std::size_t maximum_release_files{256U};
    std::uint64_t maximum_artifact_bytes{32U * 1024U * 1024U};
};

[[nodiscard]] std::string_view artifact_registry_name(ArtifactRegistry registry) noexcept;
[[nodiscard]] std::string_view archive_format_name(ArtifactArchiveFormat format) noexcept;

[[nodiscard]] bool normalize_pypi_project_name(
    std::string_view package_name,
    std::string& normalized_name,
    std::string& error);

[[nodiscard]] bool make_npm_artifact_descriptor(
    const NpmArtifactInput& input,
    const AcquisitionLimits& limits,
    ArtifactDescriptor& descriptor,
    std::string& error);

[[nodiscard]] bool make_pypi_artifact_descriptors(
    std::string_view package_name,
    std::string_view package_version,
    const std::vector<PypiReleaseFile>& files,
    const AcquisitionLimits& limits,
    std::vector<ArtifactDescriptor>& descriptors,
    std::string& error);

[[nodiscard]] bool select_pypi_sdist_artifact(
    std::string_view package_name,
    std::string_view package_version,
    const std::vector<PypiReleaseFile>& files,
    const AcquisitionLimits& limits,
    ArtifactDescriptor& descriptor,
    std::string& error);

}  // namespace mcpo
