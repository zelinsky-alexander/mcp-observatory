#include "observatory/target_manifest.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}

mcpo::ManifestResult parse(std::string_view text, mcpo::ReadLimits limits = {}) {
    std::istringstream input{std::string(text)};
    return mcpo::read_target_manifest(input, limits);
}

}  // namespace

int main() {
    {
        const auto result = parse(
            "{\"schema_version\":1,\"target_id\":\"registry:a:1\","
            "\"source_type\":\"official_registry\",\"kind\":\"local_package\"}\n"
            "{\"kind\":\"remote_endpoint\",\"source_type\":\"operator_submission\","
            "\"target_id\":\"remote:b\",\"schema_version\":1,\"metadata\":{\"x\":[1,true,null]}}\n");
        require(result.ok(), "valid records should pass");
        require(result.summary.records == 2U, "record count");
        require(result.summary.local_packages == 1U, "local package count");
        require(result.summary.remote_endpoints == 1U, "remote endpoint count");
    }

    require(!parse("{}\n").ok(), "missing fields should fail");
    require(!parse("{\"schema_version\":2,\"target_id\":\"x\",\"source_type\":\"y\",\"kind\":\"local_package\"}\n").ok(),
            "unsupported schema should fail");
    require(!parse("{\"schema_version\":1,\"schema_version\":1,\"target_id\":\"x\",\"source_type\":\"y\",\"kind\":\"local_package\"}\n").ok(),
            "duplicate security field should fail");
    require(!parse("{\"schema_version\":1,\"target_id\":\"x\\u0020y\",\"source_type\":\"y\",\"kind\":\"local_package\"}\n").ok(),
            "escaped security string should fail in version 1");

    {
        mcpo::ReadLimits limits;
        limits.max_line_bytes = 8U;
        require(!parse("{\"schema_version\":1}\n", limits).ok(), "oversized line should fail");
    }

    {
        mcpo::ReadLimits limits;
        limits.max_records = 1U;
        const auto result = parse(
            "{\"schema_version\":1,\"target_id\":\"a\",\"source_type\":\"s\",\"kind\":\"local_package\"}\n"
            "{\"schema_version\":1,\"target_id\":\"b\",\"source_type\":\"s\",\"kind\":\"remote_endpoint\"}\n",
            limits);
        require(!result.ok(), "record limit should be enforced");
    }

    return 0;
}
