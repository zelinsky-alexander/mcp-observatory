#include "observatory/acquisition.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <tuple>

namespace mcpo {
namespace {

bool ascii_alphanumeric(char character) {
    return (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9');
}

bool valid_https_url(std::string_view value) {
    const bool secure = value.starts_with("https://") && value.size() > 8U;
    const bool loopback =
        value.starts_with("http://127.0.0.1:") ||
        value.starts_with("http://localhost:");
    if (!secure && !loopback) return false;
    if (value.find('#') != std::string_view::npos) return false;
    const std::size_t scheme_length = secure ? 8U : 7U;
    const auto authority_end = value.find('/', scheme_length);
    const auto authority = value.substr(
        scheme_length,
        authority_end == std::string_view::npos
            ? value.size() - scheme_length
            : authority_end - scheme_length);
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

bool pypi_tar_gzip_sdist(const PypiReleaseFile& file) {
    return file.package_type == "sdist" &&
        (file.filename.ends_with(".tar.gz") || file.filename.ends_with(".tgz"));
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

bool normalize_pypi_project_name(
    std::string_view package_name,
    std::string& normalized_name,
    std::string& error) {
    normalized_name.clear();
    if (package_name.empty() || package_name.size() > 512U ||
        !ascii_alphanumeric(package_name.front()) ||
        !ascii_alphanumeric(package_name.back())) {
        error = "PyPI project name must be a bounded name beginning and ending with a letter or digit";
        return false;
    }

    bool separator = false;
    for (const char character : package_name) {
        if (ascii_alphanumeric(character)) {
            normalized_name.push_back(character >= 'A' && character <= 'Z'
                ? static_cast<char>(character - 'A' + 'a')
                : character);
            separator = false;
            continue;
        }
        if (character != '.' && character != '_' && character != '-') {
            normalized_name.clear();
            error = "PyPI project name contains an unsupported character";
            return false;
        }
        if (!separator) {
            normalized_name.push_back('-');
            separator = true;
        }
    }
    return true;
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

bool select_pypi_sdist_artifact(
    std::string_view package_name,
    std::string_view package_version,
    const std::vector<PypiReleaseFile>& files,
    const AcquisitionLimits& limits,
    ArtifactDescriptor& descriptor,
    std::string& error) {
    if (files.size() > limits.maximum_release_files) {
        error = "PyPI release contains too many files";
        return false;
    }

    std::string normalized_name;
    if (!normalize_pypi_project_name(package_name, normalized_name, error)) {
        return false;
    }

    const PypiReleaseFile* selected = nullptr;
    for (const auto& file : files) {
        if (file.yanked || !pypi_tar_gzip_sdist(file)) continue;
        if (!validate_common(normalized_name, package_version, file.filename, file.url,
                             file.sha256, file.size, limits, error)) {
            return false;
        }
        if (selected != nullptr) {
            error = "PyPI release has multiple supported non-yanked tar-gzip source distributions";
            return false;
        }
        selected = &file;
    }
    if (selected == nullptr) {
        error = "PyPI release has no supported non-yanked tar-gzip source distribution";
        return false;
    }

    descriptor = {
        ArtifactRegistry::pypi,
        ArtifactArchiveFormat::tar_gzip,
        std::move(normalized_name),
        std::string(package_version),
        selected->filename,
        selected->url,
        selected->sha256,
        selected->size,
    };
    return true;
}

}  // namespace mcpo
