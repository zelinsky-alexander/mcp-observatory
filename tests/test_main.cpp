#include "observatory/history.hpp"
#include "observatory/inventory.hpp"
#include "observatory/observation.hpp"
#include "observatory/registry.hpp"
#include "observatory/target_manifest.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <unistd.h>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}

mcpo::ManifestResult parse_manifest(std::string_view text, mcpo::ReadLimits limits = {}) {
    std::istringstream input{std::string(text)};
    return mcpo::read_target_manifest(input, limits);
}

mcpo::InventoryResult parse_inventory(std::string_view text, mcpo::InventoryLimits limits = {}) {
    std::istringstream input{std::string(text)};
    return mcpo::read_inventory(input, limits);
}

mcpo::ObservationResult parse_observation(std::string_view text, mcpo::ObservationLimits limits = {}) {
    std::istringstream input{std::string(text)};
    return mcpo::read_observation(input, limits);
}

mcpo::HistoryResult parse_history(
    std::string_view text,
    std::optional<std::string> target_id = std::nullopt,
    mcpo::HistoryLimits limits = {}) {
    std::istringstream input{std::string(text)};
    return mcpo::analyze_history(input, std::move(target_id), limits);
}

constexpr std::string_view base_inventory =
    "{\"inventory_version\":1,\"server\":{\"downstream_executable\":\"node\"},"
    "\"tools\":[{\"name\":\"read_file\",\"inputSchema\":{\"type\":\"object\"}},"
    "{\"name\":\"search\",\"description\":\"Find records\"}]}";

constexpr std::string_view changed_inventory =
    "{\"inventory_version\":1,\"server\":{\"downstream_executable\":\"node\"},"
    "\"tools\":[{\"name\":\"execute\",\"inputSchema\":{}},"
    "{\"name\":\"read_file\",\"inputSchema\":{\"properties\":{},\"type\":\"object\"}}]}";

std::string observation(
    std::string_view observed_at = "2026-07-24T20:15:30Z",
    std::string_view target_id = "local:filesystem:2026.7.10",
    std::string_view inventory = base_inventory) {
    return std::string("{\"observation_version\":1,\"observed_at\":\"") +
        std::string(observed_at) + "\",\"target_id\":\"" + std::string(target_id) +
        "\",\"source_type\":\"controlled_corpus\","
        "\"sensor\":{\"name\":\"mcp-native-guard\",\"version\":\"0.1.0\"},"
        "\"configuration_profile\":\"default-no-network\",\"inventory\":" +
        std::string(inventory) + "}";
}

std::string registry_entry(
    std::string_view name,
    std::string_view version,
    std::string_view extra = "") {
    return std::string("{\"server\":{\"name\":\"") + std::string(name) +
        "\",\"version\":\"" + std::string(version) +
        "\",\"description\":\"fixture\"" + std::string(extra) +
        "},\"_meta\":{\"io.modelcontextprotocol.registry/official\":{"
        "\"publishedAt\":\"2026-07-25T10:00:00Z\"}}}";
}

std::string registry_page(
    std::string records,
    std::optional<std::string_view> cursor = std::nullopt) {
    std::string result = "{\"metadata\":{\"nextCursor\":";
    if (cursor) result += "\"" + std::string(*cursor) + "\"";
    else result += "null";
    return result + "},\"servers\":[" + records + "]}";
}

mcpo::HttpTransport fixture_transport(std::vector<std::string> pages) {
    return [pages = std::move(pages), index = std::size_t{}](
               const std::string& url,
               unsigned,
               std::size_t maximum,
               mcpo::HttpResponse& response,
               std::string& error,
               const mcpo::HttpHeartbeat&,
               std::chrono::milliseconds) mutable {
        if (index >= pages.size()) {
            error = "unexpected fixture request";
            return false;
        }
        response.status = 200U;
        response.content_type = "application/json";
        response.body = pages[index++];
        if (response.body.size() > maximum) {
            error = "fixture page too large";
            return false;
        }
        response.effective_url = url;
        return true;
    };
}

}  // namespace

int main() {
    {
        const auto result = parse_manifest(
            "{\"schema_version\":1,\"target_id\":\"registry:a:1\","
            "\"source_type\":\"official_registry\",\"kind\":\"local_package\"}\n"
            "{\"kind\":\"remote_endpoint\",\"source_type\":\"operator_submission\","
            "\"target_id\":\"remote:b\",\"schema_version\":1,\"metadata\":{\"x\":[1,true,null]}}\n");
        require(result.ok(), "valid target records should pass");
        require(result.summary.records == 2U, "target record count");
    }

    require(!parse_manifest("{}\n").ok(), "missing target fields should fail");
    require(!parse_manifest("{\"schema_version\":1,\"schema_version\":1,\"target_id\":\"x\",\"source_type\":\"y\",\"kind\":\"local_package\"}\n").ok(),
            "duplicate target security field should fail");

    {
        const auto result = parse_inventory(base_inventory);
        require(result.ok(), "valid inventory should pass");
        require(result.inventory.version == 1U, "inventory version");
        require(result.inventory.tools.size() == 2U, "inventory tool count");
    }

    require(!parse_inventory("{}").ok(), "missing inventory fields should fail");
    require(!parse_inventory(
        "{\"inventory_version\":1,\"inventory_version\":1,\"server\":{\"downstream_executable\":\"x\"},\"tools\":[]}").ok(),
        "duplicate inventory version should fail");
    require(!parse_inventory(
        "{\"inventory_version\":1,\"server\":{\"downstream_executable\":\"x\"},\"tools\":[{\"name\":\"z\"},{\"name\":\"a\"}]}").ok(),
        "unsorted tools should fail");

    {
        const auto before = parse_inventory(base_inventory);
        const auto after = parse_inventory(changed_inventory);
        require(before.ok() && after.ok(), "drift fixtures should parse");
        const auto diff = mcpo::compare_inventories(before.inventory, after.inventory);
        require(diff.added.size() == 1U && diff.added[0] == "execute", "added tool");
        require(diff.removed.size() == 1U && diff.removed[0] == "search", "removed tool");
        require(diff.modified.size() == 1U && diff.modified[0].name == "read_file", "modified tool");
    }

    {
        const auto result = parse_observation(observation());
        require(result.ok(), "valid observation should pass");
        require(result.observation.target_id == "local:filesystem:2026.7.10", "observation target");
        std::ostringstream history;
        std::string error;
        require(mcpo::append_observation_jsonl(history, result.observation, error), "append observation");
        require(history.str().find('\n') == history.str().size() - 1U, "history record is one JSONL line");
    }

    require(!parse_observation("{}").ok(), "missing observation fields should fail");
    require(!parse_observation(
        "{\"observation_version\":1,\"observed_at\":\"24-07-2026\","
        "\"target_id\":\"x\",\"source_type\":\"s\",\"sensor\":{\"name\":\"n\",\"version\":\"v\"},"
        "\"configuration_profile\":\"p\",\"inventory\":" + std::string(base_inventory) + "}").ok(),
        "invalid observation timestamp should fail");

    {
        const std::string history_text =
            observation("2026-07-24T20:15:30Z", "target:a", base_inventory) + "\n" +
            observation("2026-07-24T18:00:00Z", "target:b", base_inventory) + "\n" +
            observation("2026-07-25T09:00:00Z", "target:a", changed_inventory) + "\n";
        const auto result = parse_history(history_text, std::string("target:a"));
        require(result.ok(), "valid history should pass");
        require(result.summary.records == 3U, "history record count");
        require(result.summary.targets == 2U, "history target count");
        require(result.summary.earliest_observed_at == "2026-07-24T18:00:00Z", "earliest timestamp");
        require(result.summary.latest_observed_at == "2026-07-25T09:00:00Z", "latest timestamp");
        require(result.target.records == 2U, "selected target count");
        require(result.target.previous.has_value() && result.target.latest.has_value(), "two latest observations retained");
        require(result.target.previous->observed_at == "2026-07-24T20:15:30Z", "previous target observation");
        require(result.target.latest->observed_at == "2026-07-25T09:00:00Z", "latest target observation");
        require(!mcpo::compare_inventories(
            result.target.previous->inventory,
            result.target.latest->inventory).identical(), "latest target drift detected");
    }

    {
        const std::string out_of_order =
            observation("2026-07-25T09:00:00Z", "target:a", changed_inventory) + "\n" +
            observation("2026-07-23T09:00:00Z", "target:a", base_inventory) + "\n" +
            observation("2026-07-24T09:00:00Z", "target:a", base_inventory) + "\n";
        const auto result = parse_history(out_of_order, std::string("target:a"));
        require(result.ok(), "out-of-order history should pass");
        require(result.target.previous->observed_at == "2026-07-24T09:00:00Z", "chronological previous selected");
        require(result.target.latest->observed_at == "2026-07-25T09:00:00Z", "chronological latest selected");
    }

    require(!parse_history(observation() + "\nnot-json\n").ok(), "malformed history line should fail");

    {
        mcpo::HistoryLimits limits;
        limits.max_records = 1U;
        const auto result = parse_history(observation() + "\n" + observation() + "\n", std::nullopt, limits);
        require(!result.ok(), "history record limit should be enforced");
    }

    {
        mcpo::HistoryLimits limits;
        limits.max_targets = 1U;
        const auto result = parse_history(
            observation("2026-07-24T20:15:30Z", "target:a") + "\n" +
            observation("2026-07-24T20:15:31Z", "target:b") + "\n",
            std::nullopt,
            limits);
        require(!result.ok(), "history target limit should be enforced");
    }

    {
        mcpo::RegistryUrl url;
        std::string error;
        require(mcpo::parse_registry_url("https://EXAMPLE.com:443/base/", url, error),
                "HTTPS registry URL should parse");
        require(url.normalized == "https://example.com/base", "registry URL normalized");
        require(mcpo::registry_api_url(url, std::string_view("a b")) ==
                    "https://example.com/base/v0.1/servers?cursor=a%20b",
                "base path and encoded cursor preserved");
        require(mcpo::parse_registry_url("http://localhost:8080", url, error),
                "localhost HTTP accepted");
        require(mcpo::parse_registry_url("http://127.0.0.1:8080", url, error),
                "IPv4 loopback HTTP accepted");
        require(mcpo::parse_registry_url("http://[::1]:8080", url, error),
                "IPv6 loopback HTTP accepted");
        require(!mcpo::parse_registry_url("http://example.com", url, error),
                "non-local HTTP rejected");
        require(!mcpo::parse_registry_url("https://user:pass@example.com", url, error),
                "embedded credentials rejected");
        require(!mcpo::parse_registry_url("https://example.com/#x", url, error),
                "URL fragment rejected");
        require(!mcpo::parse_registry_url("https://example.com:70000", url, error),
                "invalid URL port rejected");
        require(!mcpo::parse_registry_url("ftp://example.com", url, error),
                "unsupported URL scheme rejected");
    }

    require(mcpo::valid_utc_timestamp("2026-07-25T10:00:00Z"), "UTC timestamp accepted");
    require(!mcpo::valid_utc_timestamp("2026-13-25T10:00:00Z"), "invalid month rejected");
    require(mcpo::official_registry_base_url ==
                "https://registry.modelcontextprotocol.io",
            "compiled registry default");

    {
        const std::filesystem::path parent =
            std::filesystem::temp_directory_path() /
            ("mcpo-registry-tests-" + std::to_string(static_cast<long>(getpid())));
        std::error_code ec;
        std::filesystem::remove_all(parent, ec);
        std::filesystem::create_directory(parent);

        mcpo::RegistryCollectOptions options;
        options.output = parent / "one";
        options.registry_base_url = "http://127.0.0.1:8080/base";
        std::string message;
        require(mcpo::collect_registry(
                    options, message,
                    fixture_transport({registry_page(registry_entry("z/server", "1.0.0") + "," +
                        registry_entry("a/server", "2.0.0", ",\"unknown\":{\"kept\":true}"))})),
                "one-page collection should succeed");
        require(std::filesystem::is_regular_file(options.output / "_SUCCESS"),
                "successful collection marker exists");
        require(mcpo::validate_bundle(options.output, message), "created bundle validates");
        std::ifstream canonical(options.output / "canonical/servers.jsonl");
        std::string first;
        std::getline(canonical, first);
        require(first.find("\"server_identifier\":\"a/server\"") != std::string::npos,
                "canonical records sorted");
        require(first.find("\"unknown\":{\"kept\":true}") != std::string::npos,
                "unknown fields preserved");

        options.output = parent / "multi";
        require(mcpo::collect_registry(
                    options, message,
                    fixture_transport({
                        registry_page(registry_entry("a/server", "1"), "next"),
                        registry_page(registry_entry("b/server", "1"))})),
                "multi-page collection should succeed");

        options.output = parent / "empty";
        require(mcpo::collect_registry(
                    options, message, fixture_transport({registry_page("")})),
                "empty registry should succeed");

        options.output = parent / "limits-boundary";
        const std::string boundary_page = registry_page(registry_entry("boundary/server", "1"));
        options.limits.maximum_pages = 1U;
        options.limits.maximum_page_bytes = boundary_page.size();
        options.limits.maximum_records = 1U;
        options.limits.maximum_redirects = 0U;
        options.limits.request_timeout_seconds = 1U;
        options.limits.run_timeout_seconds = 1U;
        require(mcpo::collect_registry(
                    options, message, fixture_transport({boundary_page})),
                "inclusive configured limit boundaries should succeed");
        options.limits = {};

        options.output = parent / "identical-duplicate";
        const std::string duplicate = registry_entry("same/server", "1");
        require(mcpo::collect_registry(
                    options, message,
                    fixture_transport({registry_page(duplicate + "," + duplicate)})),
                "byte-equivalent duplicate should collapse");

        options.output = parent / "conflicting-duplicate";
        require(!mcpo::collect_registry(
                    options, message,
                    fixture_transport({registry_page(
                        registry_entry("same/server", "1") + "," +
                        registry_entry("same/server", "1", ",\"title\":\"different\""))})),
                "conflicting duplicate should fail");
        require(!std::filesystem::exists(options.output / "_SUCCESS"),
                "failed destination has no success marker");

        options.output = parent / "malformed";
        require(!mcpo::collect_registry(options, message, fixture_transport({"{bad"})),
                "malformed JSON should fail");

        options.output = parent / "excessive-depth";
        std::string deep = "{\"servers\":[{\"server\":{\"name\":\"deep/server\","
            "\"version\":\"1\",\"unknown\":";
        deep.append(66U, '[');
        deep += "null";
        deep.append(66U, ']');
        deep += "}}]}";
        require(!mcpo::collect_registry(options, message, fixture_transport({deep})),
                "excessive JSON nesting should fail");

        options.output = parent / "cursor-loop";
        require(!mcpo::collect_registry(
                    options, message,
                    fixture_transport({registry_page("", "again"), registry_page("", "again")})),
                "repeated cursor should fail");

        options.output = parent / "page-limit";
        options.limits.maximum_pages = 1U;
        require(!mcpo::collect_registry(
                    options, message,
                    fixture_transport({registry_page("", "next")})),
                "page limit enforced");
        options.limits.maximum_pages = 1'000U;

        options.output = parent / "record-limit";
        options.limits.maximum_records = 1U;
        require(!mcpo::collect_registry(
                    options, message,
                    fixture_transport({registry_page(
                        registry_entry("a/server", "1") + "," +
                        registry_entry("b/server", "1"))})),
                "record limit enforced");
        options.limits.maximum_records = 100'000U;

        options.output = parent / "existing";
        std::filesystem::create_directory(options.output);
        require(!mcpo::collect_registry(
                    options, message, fixture_transport({registry_page("")})),
                "existing destination rejected");

        std::ofstream tamper(parent / "one/canonical/servers.jsonl", std::ios::app);
        tamper << "{}\n";
        tamper.close();
        require(!mcpo::validate_bundle(parent / "one", message),
                "modified artifact is detected");

        const auto make_legacy = [&](std::string_view name) {
            const auto path = parent / std::string(name);
            std::filesystem::create_directories(path / "raw");
            return path;
        };
        const auto write_legacy_page = [](
                                           const std::filesystem::path& path,
                                           std::string_view body) {
            std::ofstream output(path, std::ios::binary);
            output << body;
            output.close();
        };

        const auto legacy = make_legacy("legacy-valid");
        write_legacy_page(
            legacy / "raw/page-000001.json",
            registry_page(registry_entry("legacy/one", "1"), "cursor-two"));
        write_legacy_page(
            legacy / "raw/page-000002.json",
            "{\"metadata\":{\"cursor\":\"cursor-two\","
            "\"nextCursor\":\"cursor-three\"},\"servers\":[" +
                registry_entry("legacy/two", "1") + "]}");
        require(mcpo::reconstruct_registry_checkpoint(
                    legacy, "http://127.0.0.1:8080/base", {}, message),
                "multi-page legacy checkpoint should reconstruct");
        require(std::filesystem::is_regular_file(legacy / "checkpoint.json"),
                "legacy checkpoint created");
        require(std::filesystem::is_regular_file(legacy / "raw/pages.jsonl"),
                "legacy page metadata rebuilt");
        require(!std::filesystem::exists(legacy / "_SUCCESS"),
                "checkpoint reconstruction must not create _SUCCESS");

        options = {};
        options.output = parent / "legacy-resumed";
        options.resume = legacy;
        options.registry_base_url = "http://127.0.0.1:8080/base";
        bool saw_resume_cursor = false;
        mcpo::HttpTransport resume_transport =
            [&](const std::string& url, unsigned, std::size_t,
                mcpo::HttpResponse& response, std::string&,
                const mcpo::HttpHeartbeat&, std::chrono::milliseconds) {
                saw_resume_cursor =
                    url.find("cursor=cursor-three") != std::string::npos;
                response.status = 200U;
                response.content_type = "application/json";
                response.body = registry_page(registry_entry("legacy/three", "1"));
                response.effective_url = url;
                return true;
            };
        require(mcpo::collect_registry(
                    options, message, std::move(resume_transport)),
                "collection should resume from reconstructed cursor");
        require(saw_resume_cursor, "resume request used derived next cursor");
        require(mcpo::validate_bundle(options.output, message),
                "resumed legacy bundle validates");

        const auto gap = make_legacy("legacy-gap");
        write_legacy_page(
            gap / "raw/page-000001.json",
            registry_page("", "cursor-two"));
        write_legacy_page(
            gap / "raw/page-000003.json",
            registry_page(""));
        require(!mcpo::reconstruct_registry_checkpoint(
                    gap, "http://127.0.0.1:8080", {}, message),
                "legacy page-number gap rejected");

        const auto malformed = make_legacy("legacy-malformed");
        write_legacy_page(malformed / "raw/page-000001.json", "{bad");
        require(!mcpo::reconstruct_registry_checkpoint(
                    malformed, "http://127.0.0.1:8080", {}, message),
                "malformed legacy page rejected");

        const auto duplicate_number = make_legacy("legacy-duplicate");
        write_legacy_page(
            duplicate_number / "raw/page-1.json",
            registry_page(""));
        write_legacy_page(
            duplicate_number / "raw/page-000001.json",
            registry_page(""));
        require(!mcpo::reconstruct_registry_checkpoint(
                    duplicate_number, "http://127.0.0.1:8080", {}, message),
                "duplicate legacy page number rejected");

        const auto broken_cursor = make_legacy("legacy-broken-cursor");
        write_legacy_page(
            broken_cursor / "raw/page-000001.json",
            registry_page("", "cursor-two"));
        write_legacy_page(
            broken_cursor / "raw/page-000002.json",
            "{\"metadata\":{\"cursor\":\"wrong-cursor\",\"nextCursor\":null},"
            "\"servers\":[]}");
        require(!mcpo::reconstruct_registry_checkpoint(
                    broken_cursor, "http://127.0.0.1:8080", {}, message),
                "broken legacy cursor continuity rejected");

        options = {};
        options.output = parent / "progress-new";
        options.registry_base_url = "http://127.0.0.1:8080/base";
        options.verbose = true;
        options.heartbeat_interval = std::chrono::seconds(1);
        const std::string long_cursor = "abc\n" + std::string(200U, 'x');
        const std::string first_progress_page =
            "{\"metadata\":{\"nextCursor\":\"abc\\n" + std::string(200U, 'x') +
            "\"},\"servers\":[" + registry_entry("progress/one", "1") + "]}";
        const std::string second_progress_page =
            registry_page(registry_entry("progress/two", "1"));
        std::ostringstream progress_error;
        std::ostringstream progress_output;
        std::streambuf* saved_error = std::cerr.rdbuf(progress_error.rdbuf());
        std::streambuf* saved_output = std::cout.rdbuf(progress_output.rdbuf());
        bool request_start_visible = false;
        std::size_t progress_requests = 0U;
        mcpo::HttpTransport progress_transport =
            [&](const std::string& url, unsigned, std::size_t,
                mcpo::HttpResponse& response, std::string&,
                const mcpo::HttpHeartbeat&, std::chrono::milliseconds) {
                request_start_visible =
                    request_start_visible ||
                    progress_error.str().find("request_start") != std::string::npos;
                response.status = 200U;
                response.content_type = "application/json";
                response.body = progress_requests++ == 0U ?
                    first_progress_page : second_progress_page;
                response.effective_url = url;
                return true;
            };
        const bool progress_success = mcpo::collect_registry(
            options, message, std::move(progress_transport));
        std::cerr.rdbuf(saved_error);
        std::cout.rdbuf(saved_output);
        const std::string progress_log = progress_error.str();
        require(progress_success, "verbose collection should succeed");
        require(request_start_visible,
                "request_start should be flushed before transport runs");
        require(progress_output.str().empty(),
                "library progress must not use stdout");
        require(progress_log.find("[registry] mode=new") != std::string::npos,
                "new collection startup progress");
        require(progress_log.find("request_timeout=30s run_timeout=900s") !=
                    std::string::npos,
                "startup timeout summary");
        require(progress_log.find("page=1 attempt=1 request_start") !=
                    std::string::npos,
                "first request progress");
        require(progress_log.find("page=2 attempt=1 request_start") !=
                    std::string::npos,
                "second request progress");
        require(progress_log.find("cursor_prefix=abc\\x0a") != std::string::npos,
                "cursor control characters escaped");
        require(progress_log.find(
                    "cursor_length=" + std::to_string(long_cursor.size())) !=
                    std::string::npos,
                "long cursor length reported");
        require(progress_log.find(long_cursor) == std::string::npos,
                "full cursor must not be logged");
        require(progress_log.find("page=2 status=200") != std::string::npos &&
                    progress_log.find("total_records=2 completed_pages=2") !=
                        std::string::npos,
                "page completion counters");
        require(progress_log.find(" waiting=") == std::string::npos,
                "fast responses should not emit heartbeat");
        require(progress_log.find("pagination_complete pages=2 records=2") !=
                    std::string::npos &&
                    progress_log.find("canonicalization_start") !=
                        std::string::npos &&
                    progress_log.find("canonicalization_complete") !=
                        std::string::npos &&
                    progress_log.find("manifest_generation") !=
                        std::string::npos &&
                    progress_log.find("validation_complete") !=
                        std::string::npos &&
                    progress_log.find("success_marker_create") !=
                        std::string::npos &&
                    progress_log.find("atomic_promotion") != std::string::npos,
                "finalization stages reported");
        require(progress_log.find("[registry] success output=") !=
                    std::string::npos,
                "success summary reported");

        options.output = parent / "progress-heartbeat";
        std::ostringstream heartbeat_error;
        saved_error = std::cerr.rdbuf(heartbeat_error.rdbuf());
        mcpo::HttpTransport heartbeat_transport =
            [&](const std::string& url, unsigned, std::size_t,
                mcpo::HttpResponse& response, std::string&,
                const mcpo::HttpHeartbeat& heartbeat,
                std::chrono::milliseconds interval) {
                require(interval >= std::chrono::seconds(1),
                        "heartbeat interval must be at least one second");
                heartbeat(std::chrono::seconds(1));
                response.status = 200U;
                response.content_type = "application/json";
                response.body = registry_page("");
                response.effective_url = url;
                return true;
            };
        const bool heartbeat_success = mcpo::collect_registry(
            options, message, std::move(heartbeat_transport));
        std::cerr.rdbuf(saved_error);
        require(heartbeat_success, "heartbeat fixture should succeed");
        require(heartbeat_error.str().find(
                    "page=1 attempt=1 waiting=1s remaining=") !=
                    std::string::npos,
                "delayed response heartbeat");

        options.output = parent / "progress-resume";
        options.resume = legacy;
        std::ostringstream resume_error;
        saved_error = std::cerr.rdbuf(resume_error.rdbuf());
        mcpo::HttpTransport verbose_resume_transport =
            [](const std::string& url, unsigned, std::size_t,
               mcpo::HttpResponse& response, std::string&,
               const mcpo::HttpHeartbeat&, std::chrono::milliseconds) {
                response.status = 200U;
                response.content_type = "application/json";
                response.body = registry_page(registry_entry("legacy/three", "1"));
                response.effective_url = url;
                return true;
            };
        const bool resume_success = mcpo::collect_registry(
            options, message, std::move(verbose_resume_transport));
        std::cerr.rdbuf(saved_error);
        const std::string resume_log = resume_error.str();
        require(resume_success, "verbose resume should succeed");
        require(resume_log.find("[registry] mode=resume") != std::string::npos &&
                    resume_log.find("resume_source=") != std::string::npos,
                "resume startup summary");
        require(resume_log.find("resume checkpoint_load") != std::string::npos &&
                    resume_log.find("resume checkpoint_validation") !=
                        std::string::npos &&
                    resume_log.find("resume raw_artifact_validation") !=
                        std::string::npos &&
                    resume_log.find("resume copy_start pages=2") !=
                        std::string::npos &&
                    resume_log.find("resume copy_complete pages=2") !=
                        std::string::npos &&
                    resume_log.find("resume continuation_page=3") !=
                        std::string::npos,
                "resume stages reported");
        require(resume_log.find("completed_pages=2 completed_records=2 next_page=3") !=
                    std::string::npos &&
                    resume_log.find("page=3 attempt=1 request_start") !=
                        std::string::npos &&
                    resume_log.find("page=3 status=200") != std::string::npos,
                "resumed numbering and counters");

        options = {};
        options.output = parent / "progress-request-timeout";
        options.registry_base_url = "http://127.0.0.1:8080/base";
        options.verbose = true;
        options.limits.request_timeout_seconds = 1U;
        options.limits.run_timeout_seconds = 30U;
        std::ostringstream request_timeout_error;
        saved_error = std::cerr.rdbuf(request_timeout_error.rdbuf());
        mcpo::HttpTransport timeout_transport =
            [](const std::string&, unsigned, std::size_t,
               mcpo::HttpResponse&, std::string& error,
               const mcpo::HttpHeartbeat&, std::chrono::milliseconds) {
                error = "child process timed out";
                return false;
            };
        const bool timeout_success = mcpo::collect_registry(
            options, message, timeout_transport);
        std::cerr.rdbuf(saved_error);
        require(!timeout_success, "request timeout fixture should fail");
        require(request_timeout_error.str().find(
                    "failure stage=http_request category=request_timeout page=1") !=
                    std::string::npos,
                "request timeout failure summary");

        options.output = parent / "progress-run-deadline";
        options.limits.request_timeout_seconds = 90U;
        options.limits.run_timeout_seconds = 1U;
        std::ostringstream deadline_error;
        saved_error = std::cerr.rdbuf(deadline_error.rdbuf());
        const bool deadline_success = mcpo::collect_registry(
            options, message, timeout_transport);
        std::cerr.rdbuf(saved_error);
        require(!deadline_success, "run deadline fixture should fail");
        require(deadline_error.str().find(
                    "category=total_run_deadline_exhausted page=1") !=
                    std::string::npos,
                "deadline-limited curl timeout classification");

        options = {};
        options.output = parent / "progress-quiet";
        options.registry_base_url = "http://127.0.0.1:8080/base";
        std::ostringstream quiet_error;
        saved_error = std::cerr.rdbuf(quiet_error.rdbuf());
        const bool quiet_success = mcpo::collect_registry(
            options, message, fixture_transport({first_progress_page, second_progress_page}));
        std::cerr.rdbuf(saved_error);
        require(quiet_success, "quiet comparison collection should succeed");
        require(quiet_error.str().empty(), "non-verbose collection emitted progress");
        const auto read_bytes = [](const std::filesystem::path& path) {
            std::ifstream input(path, std::ios::binary);
            return std::string(
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>());
        };
        require(read_bytes(parent / "progress-new/raw/page-000001.json") ==
                    read_bytes(parent / "progress-quiet/raw/page-000001.json") &&
                    read_bytes(parent / "progress-new/raw/page-000002.json") ==
                        read_bytes(parent / "progress-quiet/raw/page-000002.json") &&
                    read_bytes(parent / "progress-new/canonical/servers.jsonl") ==
                        read_bytes(parent / "progress-quiet/canonical/servers.jsonl"),
                "verbose mode changed generated evidence artifacts");

        std::filesystem::remove_all(parent, ec);
    }

    return 0;
}
