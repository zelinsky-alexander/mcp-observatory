#include "observatory/acquisition.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <tuple>

namespace mcpo {
namespace {

bool valid_https_url(std::string_view value) {
    if (!value.starts_with("https://") || value.size() <= 8U) return false;
    if (value.find('#') != std::string_view::npos) return false;
    const auto authority_end = value.find('/', 8U);
    const auto authority = value.substr(8U, authority_end == std::string_view::npos
        ? value.size() - 8U
        : authority_end - 8U);
    return !authority.empty() && authority.find('@') == std::string_view::npos;
}

bool valid_sha256(std::string_view value) {
    return value.size() == 64U && std::all_of(value.begin(), value.end(), [](char character) {
        return std::isxdigit(static_cast<unsigned char>(character)) != 0;
    });
}

bool valid_identity(std::string_view value) {
    return !value.empty() && value.size() <= 512U &&
        std::all_of(value.begin(), value.end(), [](char character) {
            const auto byte = static_cast<unsigned char>(character);
            return byte >= 0x21U && byte != 0x7fU;
        });
}

bool validate_common(
    std::string_view package_name,
    std::string_view package_version,
    std::string_view filename,
    std::string_view url,
    std::string_view sha256,
    std::uint64_t size,
    const AcquisitionLimits& limits,
    std::string& error) {
    if (!valid_identity(package_name) || !valid_identity(package_version)) {
        error = "package name and version must be non-empty bounded visible strings";
        return false;
    }
    if (!valid_identity(filename) || filename.find('/') != std::string_view::npos ||
        filename.find('\\') != std::string_view::npos) {
        error = "artifact filename must be a bounded basename";
        return false;
    }
    if (!valid_https_url(url)) {
        error = "artifact URL must be HTTPS without credentials or fragment";
        return false;
    }
    if (!valid_sha256(sha256)) {
        error = "artifact SHA-256 must contain exactly 64 hexadecimal characters";
        return false;
    }
    if (size == 0U || size > limits.maximum_artifact_bytes) {
        error = "artifact size is zero or exceeds the configured acquisition limit";
        return false;
    }
    return true;
}

bool pypi_format(const PypiReleaseFile& file, ArtifactArchiveFormat& format) {
    if (file.package_type == "bdist_wheel" && file.filename.ends_with(".whl")) {
        format = ArtifactArchiveFormat::zip;
        return true;
    }
    if (file.package_type != "sdist") return false;
    if (file.filename.ends_with(".tar.gz") || file.filename.ends_with(".tgz")) {
        format = ArtifactArchiveFormat::tar_gzip;
        return true;
    }
    if (file.filename.ends_with(".zip")) {
        format = ArtifactArchiveFormat::zip;
        return true;
    }
    return false;
}

}  // namespace

std::string_view artifact_registry_name(ArtifactRegistry registry) noexcept {
    switch (registry) {
        case ArtifactRegistry::npm: return "npm";
        case ArtifactRegistry::pypi: return "pypi";
    }
    return "unknown";
}

std::string_view archive_format_name(ArtifactArchiveFormat format) noexcept {
    switch (format) {
        case ArtifactArchiveFormat::tar_gzip: return "tar_gzip";
        case ArtifactArchiveFormat::zip: return "zip";
    }
    return "unknown";
}

bool make_npm_artifact_descriptor(
    const NpmArtifactInput& input,
    const AcquisitionLimits& limits,
    ArtifactDescriptor& descriptor,
    std::string& error) {
    const auto slash = input.tarball_url.find_last_of('/');
    const std::string filename = slash == std::string::npos
        ? std::string{}
        : input.tarball_url.substr(slash + 1U);
    if (!filename.ends_with(".tgz")) {
        error = "npm artifact URL must name a .tgz archive";
        return false;
    }
    if (!validate_common(input.package_name, input.package_version, filename,
                         input.tarball_url, input.sha256, input.published_size,
                         limits, error)) {
        return false;
    }
    descriptor = {
        ArtifactRegistry::npm,
        ArtifactArchiveFormat::tar_gzip,
        input.package_name,
        input.package_version,
        filename,
        input.tarball_url,
        input.sha256,
        input.published_size,
    };
    return true;
}

bool make_pypi_artifact_descriptors(
    std::string_view package_name,
    std::string_view package_version,
    const std::vector<PypiReleaseFile>& files,
    const AcquisitionLimits& limits,
    std::vector<ArtifactDescriptor>& descriptors,
    std::string& error) {
    descriptors.clear();
    if (files.size() > limits.maximum_release_files) {
        error = "PyPI release contains too many files";
        return false;
    }
    std::set<std::tuple<std::string, std::string>> identities;
    for (const auto& file : files) {
        if (file.yanked) continue;
        ArtifactArchiveFormat format{};
        if (!pypi_format(file, format)) continue;
        if (!validate_common(package_name, package_version, file.filename, file.url,
                             file.sha256, file.size, limits, error)) {
            descriptors.clear();
            return false;
        }
        if (!identities.emplace(file.filename, file.sha256).second) {
            error = "duplicate PyPI release artifact identity";
            descriptors.clear();
            return false;
        }
        descriptors.push_back({
            ArtifactRegistry::pypi,
            format,
            std::string(package_name),
            std::string(package_version),
            file.filename,
            file.url,
            file.sha256,
            file.size,
        });
    }
    std::sort(descriptors.begin(), descriptors.end(), [](const auto& left, const auto& right) {
        return std::tie(left.archive_format, left.filename, left.sha256) <
            std::tie(right.archive_format, right.filename, right.sha256);
    });
    if (descriptors.empty()) {
        error = "PyPI release has no supported non-yanked wheel or source distribution";
        return false;
    }
    return true;
}

}  // namespace mcpo
