#include "observatory/acquisition.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}

constexpr std::string_view digest =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

}  // namespace

int main() {
    mcpo::AcquisitionLimits limits;
    std::string error;

    {
        mcpo::ArtifactDescriptor descriptor;
        mcpo::NpmArtifactInput input{
            "example-package",
            "1.2.3",
            "https://registry.npmjs.org/example-package/-/example-package-1.2.3.tgz",
            std::string(digest),
            1024U,
        };
        require(mcpo::make_npm_artifact_descriptor(input, limits, descriptor, error),
                "valid npm descriptor should succeed");
        require(descriptor.registry == mcpo::ArtifactRegistry::npm,
                "npm registry retained");
        require(descriptor.archive_format == mcpo::ArtifactArchiveFormat::tar_gzip,
                "npm tgz format retained");
    }

    {
        std::vector<mcpo::PypiReleaseFile> files{
            {"demo-2.0.0.tar.gz", "sdist", "https://files.pythonhosted.org/demo.tar.gz",
             std::string(digest), 2048U, false},
            {"demo-2.0.0-py3-none-any.whl", "bdist_wheel",
             "https://files.pythonhosted.org/demo.whl", std::string(digest), 1024U, false},
            {"demo-2.0.0-old.whl", "bdist_wheel",
             "https://files.pythonhosted.org/old.whl", std::string(digest), 1024U, true},
        };
        std::vector<mcpo::ArtifactDescriptor> descriptors;
        require(mcpo::make_pypi_artifact_descriptors(
                    "demo", "2.0.0", files, limits, descriptors, error),
                "wheel and sdist should be accepted");
        require(descriptors.size() == 2U, "yanked PyPI file should be skipped");
        require(descriptors[0].archive_format == mcpo::ArtifactArchiveFormat::tar_gzip,
                "descriptors should be deterministic");
        require(descriptors[1].archive_format == mcpo::ArtifactArchiveFormat::zip,
                "wheel should use zip reader");
    }

    {
        std::vector<mcpo::PypiReleaseFile> files{
            {"demo.exe", "bdist_wininst", "https://files.pythonhosted.org/demo.exe",
             std::string(digest), 1024U, false},
        };
        std::vector<mcpo::ArtifactDescriptor> descriptors;
        require(!mcpo::make_pypi_artifact_descriptors(
                    "demo", "2.0.0", files, limits, descriptors, error),
                "unsupported PyPI artifact should fail closed");
    }

    {
        std::vector<mcpo::PypiReleaseFile> files{
            {"demo.whl", "bdist_wheel", "http://files.pythonhosted.org/demo.whl",
             std::string(digest), 1024U, false},
        };
        std::vector<mcpo::ArtifactDescriptor> descriptors;
        require(!mcpo::make_pypi_artifact_descriptors(
                    "demo", "2.0.0", files, limits, descriptors, error),
                "non-HTTPS PyPI URL should fail closed");
    }

    return 0;
}
