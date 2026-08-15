#include "observatory/analyze.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}

std::string metadata(std::string_view version) {
    return std::string(
        "{\"info\":{\"name\":\"innoday\",\"version\":\"") +
        std::string(version) +
        "\"},\"urls\":[{\"filename\":\"innoday-0.1.tar.gz\","
        "\"packagetype\":\"sdist\",\"url\":\"https://example.invalid/innoday-0.1.tar.gz\","
        "\"size\":123,\"yanked\":false,\"digests\":{\"sha256\":"
        "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}}]}";
}

bool accepts(std::string_view expected, std::string_view returned) {
    mcpo::ArtifactDescriptor descriptor;
    mcpo::AcquisitionLimits limits;
    std::string error;
    return mcpo::parse_pypi_release_metadata(
        metadata(returned), "innoday", expected, limits, descriptor, error);
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string_view, std::string_view>> equivalent{
        {"0.1.227-beta", "0.1.227b0"},
        {"1.0-beta", "1.0b0"},
        {"1.0-beta1", "1.0b1"},
        {"1.0-alpha", "1.0a0"},
        {"1.0-rc1", "1.0rc1"},
        {"1.0-preview2", "1.0rc2"},
        {"v1.0-beta", "1.0b0"},
        {"1.0.0-beta", "1.0b0"},
    };
    for (const auto& [expected, returned] : equivalent)
        require(accepts(expected, returned), "equivalent PyPI version rejected");

    require(!accepts("1.0", "1.0b0"), "stable and beta must differ");
    require(!accepts("1.0b1", "1.0b2"), "beta serials must differ");
    require(!accepts("1.0rc1", "1.0"), "release candidate and stable must differ");
    require(!accepts("1.0.post1", "1.0post1"),
            "unsupported normalization must fail closed");
    return 0;
}
