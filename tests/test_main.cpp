#include "observatory/analyze.hpp"
#include "observatory/explorer.hpp"
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
#include <sys/stat.h>
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
               std::chrono::steady_clock::duration,
               std::size_t maximum,
               mcpo::HttpResponse& response,
               std::string& error,
               const mcpo::HttpHeartbeat&,
               std::chrono::steady_clock::duration) mutable {
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
        options.runtime.request_timeout = std::chrono::seconds(1);
        options.runtime.run_timeout = std::chrono::seconds(2);
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
            [&](const std::string& url, std::chrono::steady_clock::duration, std::size_t,
                mcpo::HttpResponse& response, std::string&,
                const mcpo::HttpHeartbeat&, std::chrono::steady_clock::duration) {
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
            [&](const std::string& url, std::chrono::steady_clock::duration, std::size_t,
                mcpo::HttpResponse& response, std::string&,
                const mcpo::HttpHeartbeat&, std::chrono::steady_clock::duration) {
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
        require(progress_log.find("[registry] mode=fresh") != std::string::npos,
                "new collection startup progress");
        require(progress_log.find(
                    "request_timeout=60s stall_timeout=300s run_timeout=unlimited") !=
                    std::string::npos,
                "startup timeout summary");
        require(progress_log.find("page=1 attempt=1/8 request_start") !=
                    std::string::npos,
                "first request progress");
        require(progress_log.find("page=2 attempt=1/8 request_start") !=
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
        require(progress_log.find("heartbeat state=request_wait") == std::string::npos,
                "fast responses should not emit heartbeat");
        require(progress_log.find("timing phase=pagination") !=
                    std::string::npos &&
                    progress_log.find("timing phase=canonicalization") !=
                        std::string::npos &&
                    progress_log.find("timing phase=manifest_generation") !=
                        std::string::npos &&
                    progress_log.find("timing phase=final_validation") !=
                        std::string::npos &&
                    progress_log.find("timing phase=atomic_promotion") !=
                        std::string::npos &&
                    progress_log.find("timing phase=total") !=
                        std::string::npos,
                "finalization stages reported");
        require(progress_log.find("[registry] success output=") !=
                    std::string::npos,
                "success summary reported");

        options.output = parent / "progress-heartbeat";
        std::ostringstream heartbeat_error;
        saved_error = std::cerr.rdbuf(heartbeat_error.rdbuf());
        mcpo::HttpTransport heartbeat_transport =
            [&](const std::string& url, std::chrono::steady_clock::duration, std::size_t,
                mcpo::HttpResponse& response, std::string&,
                const mcpo::HttpHeartbeat& heartbeat,
                std::chrono::steady_clock::duration interval) {
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
                    "heartbeat state=request_wait page=1 attempt=1/8 request_wait=1s") !=
                    std::string::npos,
                "delayed response heartbeat");

        options.output = parent / "progress-resume";
        options.resume = legacy;
        std::ostringstream resume_error;
        saved_error = std::cerr.rdbuf(resume_error.rdbuf());
        mcpo::HttpTransport verbose_resume_transport =
            [](const std::string& url, std::chrono::steady_clock::duration, std::size_t,
               mcpo::HttpResponse& response, std::string&,
               const mcpo::HttpHeartbeat&, std::chrono::steady_clock::duration) {
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
        require(resume_log.find("resume checkpoint_validation_start") !=
                    std::string::npos &&
                    resume_log.find(
                        "resume checkpoint_validation_complete completed_pages=2") !=
                    std::string::npos,
                "resume stages reported");
        require(resume_log.find("completed_pages=2 completed_records=2 next_page=3") !=
                    std::string::npos &&
                    resume_log.find("page=3 attempt=1/8 request_start") !=
                        std::string::npos &&
                    resume_log.find("page=3 status=200") != std::string::npos,
                "resumed numbering and counters");

        options = {};
        options.output = parent / "progress-request-timeout";
        options.registry_base_url = "http://127.0.0.1:8080/base";
        options.verbose = true;
        options.runtime.request_timeout = std::chrono::seconds(1);
        options.runtime.run_timeout = std::chrono::seconds(30);
        options.runtime.maximum_attempts_per_page = 1U;
        std::ostringstream request_timeout_error;
        saved_error = std::cerr.rdbuf(request_timeout_error.rdbuf());
        mcpo::HttpTransport timeout_transport =
            [](const std::string&, std::chrono::steady_clock::duration, std::size_t,
               mcpo::HttpResponse& response, std::string& error,
               const mcpo::HttpHeartbeat&, std::chrono::steady_clock::duration) {
                response.failure_kind = mcpo::HttpFailureKind::timeout;
                error = "child process timed out";
                return false;
            };
        const bool timeout_success = mcpo::collect_registry(
            options, message, timeout_transport);
        std::cerr.rdbuf(saved_error);
        require(!timeout_success, "request timeout fixture should fail");
        require(request_timeout_error.str().find(
                    "failure stage=http_request category=retry_budget_exhausted page=1") !=
                    std::string::npos,
                "request retry exhaustion summary");

        options.output = parent / "progress-run-deadline";
        options.runtime.request_timeout = std::chrono::seconds(90);
        options.runtime.run_timeout = std::chrono::seconds(1);
        auto fake_now = std::chrono::steady_clock::time_point{};
        options.now = [&] { return fake_now; };
        mcpo::HttpTransport deadline_transport =
            [&](const std::string&, std::chrono::steady_clock::duration timeout, std::size_t,
                mcpo::HttpResponse& response, std::string& error,
                const mcpo::HttpHeartbeat&, std::chrono::steady_clock::duration) {
                require(timeout == std::chrono::seconds(1),
                        "request timeout capped by remaining total runtime");
                fake_now += std::chrono::seconds(1);
                response.failure_kind = mcpo::HttpFailureKind::timeout;
                error = "child process timed out";
                return false;
            };
        std::ostringstream deadline_error;
        saved_error = std::cerr.rdbuf(deadline_error.rdbuf());
        const bool deadline_success = mcpo::collect_registry(
            options, message, deadline_transport);
        std::cerr.rdbuf(saved_error);
        require(!deadline_success, "run deadline fixture should fail");
        require(deadline_error.str().find(
                    "category=total_run_deadline_exhausted page=1") !=
                    std::string::npos,
                "deadline-limited curl timeout classification");

        struct DeadlineCaseResult {
            std::string log;
            std::chrono::steady_clock::duration elapsed{};
            std::vector<std::chrono::steady_clock::duration> wakes;
        };
        const auto deadline_case =
            [&](std::string_view name,
                std::chrono::seconds stall_timeout,
                std::optional<std::chrono::seconds> run_timeout) {
                mcpo::RegistryCollectOptions deadline_options;
                deadline_options.output = parent / std::string(name);
                deadline_options.registry_base_url =
                    "http://127.0.0.1:8080/base";
                deadline_options.verbose = true;
                deadline_options.runtime.request_timeout =
                    std::chrono::seconds(30);
                deadline_options.runtime.stall_timeout = stall_timeout;
                deadline_options.runtime.run_timeout = run_timeout;
                auto clock = std::chrono::steady_clock::time_point{};
                deadline_options.now = [&] { return clock; };
                std::ostringstream captured;
                std::streambuf* previous =
                    std::cerr.rdbuf(captured.rdbuf());
                std::vector<std::chrono::steady_clock::duration>
                    scheduled_wakes;
                mcpo::HttpTransport transport =
                    [&](const std::string&,
                        std::chrono::steady_clock::duration,
                        std::size_t,
                        mcpo::HttpResponse& response,
                        std::string& transport_error,
                        const mcpo::HttpHeartbeat& heartbeat,
                        std::chrono::steady_clock::duration initial_wake) {
                        auto waited =
                            std::chrono::steady_clock::duration::zero();
                        auto wake = initial_wake;
                        bool stopped{};
                        for (std::size_t count = 0U; count < 10U; ++count) {
                            require(
                                wake >
                                    std::chrono::steady_clock::duration::zero(),
                                "HTTP wait must not schedule a zero wake");
                            scheduled_wakes.push_back(wake);
                            clock += wake;
                            waited += wake;
                            const mcpo::HttpWaitDecision decision =
                                heartbeat(
                                    std::chrono::duration_cast<
                                        std::chrono::milliseconds>(
                                        waited));
                            if (!decision.continue_waiting) {
                                stopped = true;
                                break;
                            }
                            wake = decision.next_wake;
                        }
                        require(stopped,
                                "HTTP wait callback must not busy-loop");
                        response.failure_kind =
                            mcpo::HttpFailureKind::cancelled;
                        transport_error = "deadline fixture cancelled";
                        return false;
                    };
                require(
                    !mcpo::collect_registry(
                        deadline_options, message, transport),
                    "deadline classification fixture should fail");
                std::cerr.rdbuf(previous);
                DeadlineCaseResult result;
                result.log = captured.str();
                result.elapsed =
                    clock.time_since_epoch();
                result.wakes = std::move(scheduled_wakes);
                return result;
            };

        const DeadlineCaseResult bounded_smoke = deadline_case(
            "deadline-total-before-stall", std::chrono::seconds(120),
            std::chrono::seconds(2));
        require(
            bounded_smoke.log.find(
                "category=total_run_deadline_exhausted") !=
                std::string::npos &&
                bounded_smoke.log.find("category=collection_stalled") ==
                    std::string::npos &&
                bounded_smoke.elapsed == std::chrono::seconds(2),
            "two-second total deadline wins while 120-second stall is healthy");

        const DeadlineCaseResult unlimited_stall = deadline_case(
            "deadline-stall-unlimited", std::chrono::seconds(5),
            std::nullopt);
        require(
            unlimited_stall.log.find("category=collection_stalled") !=
                std::string::npos &&
                unlimited_stall.log.find(
                    "category=total_run_deadline_exhausted") ==
                    std::string::npos &&
                unlimited_stall.elapsed == std::chrono::seconds(5) &&
                unlimited_stall.wakes.size() == 1U &&
                unlimited_stall.log.find("heartbeat state=request_wait") ==
                    std::string::npos,
            "stall deadline expires in unlimited mode");

        const DeadlineCaseResult three_second_stall = deadline_case(
            "deadline-three-second-stall", std::chrono::seconds(3),
            std::nullopt);
        require(
            three_second_stall.elapsed == std::chrono::seconds(3) &&
                three_second_stall.log.find(
                    "category=collection_stalled") != std::string::npos,
            "stall deadline wakes before five-second heartbeat");

        const DeadlineCaseResult twelve_second_stall = deadline_case(
            "deadline-twelve-second-stall", std::chrono::seconds(12),
            std::nullopt);
        require(
            twelve_second_stall.elapsed == std::chrono::seconds(12) &&
                twelve_second_stall.wakes.size() == 3U &&
                twelve_second_stall.log.find("request_wait=5s") !=
                    std::string::npos &&
                twelve_second_stall.log.find("request_wait=10s") !=
                    std::string::npos &&
                twelve_second_stall.log.find("request_wait=12s") ==
                    std::string::npos,
            "heartbeat cadence yields to a non-aligned stall deadline");

        const DeadlineCaseResult simultaneous_deadlines = deadline_case(
            "deadline-simultaneous", std::chrono::seconds(5),
            std::chrono::seconds(5));
        require(
            simultaneous_deadlines.log.find(
                "category=total_run_deadline_exhausted") !=
                std::string::npos &&
                simultaneous_deadlines.log.find(
                    "category=collection_stalled") ==
                    std::string::npos,
            "explicit total deadline has deterministic precedence");

        const DeadlineCaseResult stall_first = deadline_case(
            "deadline-stall-first", std::chrono::seconds(5),
            std::chrono::seconds(10));
        require(
            stall_first.log.find("category=collection_stalled") !=
                std::string::npos,
            "stall is reported when it expires before total runtime");

        options = {};
        options.output = parent / "deadline-below-stall";
        options.registry_base_url = "http://127.0.0.1:8080/base";
        options.verbose = true;
        options.runtime.stall_timeout = std::chrono::seconds(120);
        auto below_stall_clock =
            std::chrono::steady_clock::time_point{};
        options.now = [&] { return below_stall_clock; };
        std::ostringstream below_stall_output;
        saved_error = std::cerr.rdbuf(below_stall_output.rdbuf());
        require(!mcpo::collect_registry(
                    options, message,
                    [&](const std::string&,
                        std::chrono::steady_clock::duration,
                        std::size_t,
                        mcpo::HttpResponse& response,
                        std::string& transport_error,
                        const mcpo::HttpHeartbeat&,
                        std::chrono::steady_clock::duration) {
                        below_stall_clock += std::chrono::seconds(1);
                        response.failure_kind =
                            mcpo::HttpFailureKind::other;
                        transport_error = "sub-stall fixture stopped";
                        return false;
                    }),
                "sub-stall fixture should stop");
        std::cerr.rdbuf(saved_error);
        const std::string below_stall = below_stall_output.str();
        require(
            below_stall.find("seconds_since_last_completed_page=1") !=
                std::string::npos &&
                below_stall.find("category=collection_stalled") ==
                    std::string::npos &&
                below_stall.find(
                    "category=total_run_deadline_exhausted") ==
                    std::string::npos,
            "sub-stall elapsed time cannot classify as a stall");

        options = {};
        options.output = parent / "deadline-reset-after-page";
        options.registry_base_url = "http://127.0.0.1:8080/base";
        options.verbose = true;
        options.runtime.stall_timeout = std::chrono::seconds(5);
        auto reset_clock = std::chrono::steady_clock::time_point{};
        options.now = [&] { return reset_clock; };
        std::size_t reset_requests{};
        std::ostringstream reset_output;
        saved_error = std::cerr.rdbuf(reset_output.rdbuf());
        require(!mcpo::collect_registry(
                    options, message,
                    [&](const std::string& url,
                        std::chrono::steady_clock::duration,
                        std::size_t,
                        mcpo::HttpResponse& response,
                        std::string& transport_error,
                        const mcpo::HttpHeartbeat& heartbeat,
                        std::chrono::steady_clock::duration initial_wake) {
                        if (reset_requests++ == 0U) {
                            reset_clock += std::chrono::seconds(4);
                            response.status = 200U;
                            response.content_type = "application/json";
                            response.body = registry_page(
                                registry_entry("reset/one", "1"),
                                "reset-cursor");
                            response.effective_url = url;
                            return true;
                        }
                        require(initial_wake == std::chrono::seconds(5),
                                "durable page reset full stall interval");
                        reset_clock += initial_wake;
                        const auto decision = heartbeat(
                            std::chrono::duration_cast<
                                std::chrono::milliseconds>(initial_wake));
                        require(!decision.continue_waiting,
                                "reset stall deadline cancels second request");
                        response.failure_kind =
                            mcpo::HttpFailureKind::cancelled;
                        transport_error = "reset fixture cancelled";
                        return false;
                    }),
                "reset deadline fixture should stall on second page");
        std::cerr.rdbuf(saved_error);
        require(
            reset_output.str().find("completed_pages=1") !=
                std::string::npos &&
                reset_output.str().find(
                    "seconds_since_last_completed_page=5") !=
                    std::string::npos &&
                std::chrono::duration_cast<std::chrono::seconds>(
                    reset_clock.time_since_epoch()) ==
                    std::chrono::seconds(9),
            "durable completion resets stall deadline from commit time");

        options = {};
        options.output = parent / "millisecond-request-cap";
        options.registry_base_url = "http://127.0.0.1:8080/base";
        options.runtime.request_timeout = std::chrono::seconds(30);
        options.runtime.run_timeout = std::chrono::seconds(2);
        std::size_t clock_reads{};
        options.now = [&] {
            return std::chrono::steady_clock::time_point{} +
                (clock_reads++ == 0U ?
                    std::chrono::milliseconds::zero() :
                    std::chrono::milliseconds(250));
        };
        std::chrono::steady_clock::duration observed_request_timeout{};
        require(!mcpo::collect_registry(
                    options, message,
                    [&](const std::string&,
                        std::chrono::steady_clock::duration timeout,
                        std::size_t,
                        mcpo::HttpResponse& response,
                        std::string& transport_error,
                        const mcpo::HttpHeartbeat&,
                        std::chrono::steady_clock::duration) {
                        observed_request_timeout = timeout;
                        response.failure_kind =
                            mcpo::HttpFailureKind::other;
                        transport_error = "rounding fixture stopped";
                        return false;
                    }),
                "millisecond request-timeout fixture should stop");
        require(
            observed_request_timeout ==
                std::chrono::milliseconds(1'750),
            "effective request timeout preserves fractional remaining runtime");

        options = {};
        options.output = parent / "unlimited-long-run";
        options.registry_base_url = "http://127.0.0.1:8080/base";
        options.runtime.stall_timeout = std::chrono::seconds(700);
        fake_now = std::chrono::steady_clock::time_point{};
        options.now = [&] { return fake_now; };
        std::size_t long_run_requests{};
        mcpo::HttpTransport long_run_transport =
            [&](const std::string& url, std::chrono::steady_clock::duration timeout, std::size_t,
                mcpo::HttpResponse& response, std::string&,
                const mcpo::HttpHeartbeat&, std::chrono::steady_clock::duration) {
                require(timeout == std::chrono::seconds(60),
                        "unlimited request timeout is not total-deadline capped");
                fake_now += std::chrono::seconds(600);
                response.status = 200U;
                response.content_type = "application/json";
                response.body = long_run_requests++ == 0U ?
                    registry_page(
                        registry_entry("long/one", "1"), "continue") :
                    registry_page(registry_entry("long/two", "1"));
                response.effective_url = url;
                return true;
            };
        require(mcpo::collect_registry(
                    options, message, long_run_transport),
                "healthy unlimited collection beyond old deadline succeeds");
        require(
            fake_now >= std::chrono::steady_clock::time_point{} +
                std::chrono::seconds(1'200),
            "long-running fixture crossed old total timeout");

        options = {};
        options.output = parent / "stall-watchdog";
        options.registry_base_url = "http://127.0.0.1:8080/base";
        options.verbose = true;
        options.runtime.stall_timeout = std::chrono::seconds(10);
        fake_now = std::chrono::steady_clock::time_point{};
        options.now = [&] { return fake_now; };
        std::ostringstream stall_error;
        saved_error = std::cerr.rdbuf(stall_error.rdbuf());
        mcpo::HttpTransport stalled_transport =
            [&](const std::string&, std::chrono::steady_clock::duration, std::size_t,
                mcpo::HttpResponse& response, std::string& transport_error,
                const mcpo::HttpHeartbeat& heartbeat,
                std::chrono::steady_clock::duration) {
                fake_now += std::chrono::seconds(10);
                require(
                    !heartbeat(std::chrono::seconds(10)).continue_waiting,
                        "stall heartbeat must request transport cancellation");
                response.failure_kind = mcpo::HttpFailureKind::cancelled;
                transport_error = "cancelled by progress watchdog";
                return false;
            };
        const bool stalled = mcpo::collect_registry(
            options, message, stalled_transport);
        std::cerr.rdbuf(saved_error);
        require(!stalled, "stalled collection should fail");
        require(
            stall_error.str().find(
                "stage=progress_watchdog category=collection_stalled") !=
                std::string::npos &&
                stall_error.str().find(
                    "seconds_since_last_completed_page=10") !=
                std::string::npos,
            "stall has a distinct durable-progress failure");

        for (const unsigned retry_status :
             {408U, 425U, 429U, 500U, 502U, 503U, 504U}) {
            options = {};
            options.output =
                parent / ("retry-" + std::to_string(retry_status));
            options.registry_base_url =
                "http://127.0.0.1:8080/base";
            options.runtime.retry_initial = std::chrono::seconds(2);
            options.runtime.retry_maximum = std::chrono::seconds(10);
            fake_now = std::chrono::steady_clock::time_point{};
            options.now = [&] { return fake_now; };
            std::chrono::steady_clock::duration waited{};
            options.wait = [&](std::chrono::steady_clock::duration duration) {
                waited += duration;
                fake_now += duration;
            };
            std::size_t attempts{};
            mcpo::HttpTransport retry_transport =
                [&](const std::string& url, std::chrono::steady_clock::duration, std::size_t,
                    mcpo::HttpResponse& response, std::string&,
                    const mcpo::HttpHeartbeat&,
                    std::chrono::steady_clock::duration) {
                    response.status =
                        attempts++ == 0U ? retry_status : 200U;
                    response.content_type = "application/json";
                    response.body = registry_page(
                        registry_entry("retry/server", "1"));
                    response.effective_url = url;
                    return true;
                };
            require(mcpo::collect_registry(
                        options, message, retry_transport),
                    "retryable HTTP status should recover");
            require(attempts == 2U &&
                        waited == std::chrono::seconds(2),
                    "retryable HTTP status uses bounded initial backoff");
        }

        options = {};
        options.output = parent / "retry-after";
        options.registry_base_url = "http://127.0.0.1:8080/base";
        options.runtime.retry_initial = std::chrono::seconds(2);
        options.runtime.retry_maximum = std::chrono::seconds(5);
        fake_now = std::chrono::steady_clock::time_point{};
        options.now = [&] { return fake_now; };
        std::chrono::steady_clock::duration retry_after_wait{};
        options.wait = [&](std::chrono::steady_clock::duration duration) {
            retry_after_wait += duration;
            fake_now += duration;
        };
        std::size_t retry_after_attempts{};
        mcpo::HttpTransport retry_after_transport =
            [&](const std::string& url, std::chrono::steady_clock::duration, std::size_t,
                mcpo::HttpResponse& response, std::string&,
                const mcpo::HttpHeartbeat&, std::chrono::steady_clock::duration) {
                response.status =
                    retry_after_attempts++ == 0U ? 503U : 200U;
                response.retry_after = "99";
                response.content_type = "application/json";
                response.body =
                    registry_page(registry_entry("retry-after/server", "1"));
                response.effective_url = url;
                return true;
            };
        require(mcpo::collect_registry(
                    options, message, retry_after_transport),
                "integer Retry-After retry should recover");
        require(retry_after_wait == std::chrono::seconds(5),
                "Retry-After is capped at retry maximum");

        options = {};
        options.output = parent / "temporary-network-backoff";
        options.registry_base_url = "http://127.0.0.1:8080/base";
        options.runtime.retry_initial = std::chrono::seconds(2);
        options.runtime.retry_maximum = std::chrono::seconds(5);
        fake_now = std::chrono::steady_clock::time_point{};
        options.now = [&] { return fake_now; };
        std::chrono::steady_clock::duration network_wait{};
        options.wait = [&](std::chrono::steady_clock::duration duration) {
            network_wait += duration;
            fake_now += duration;
        };
        std::size_t network_attempts{};
        mcpo::HttpTransport temporary_network_transport =
            [&](const std::string& url, std::chrono::steady_clock::duration, std::size_t,
                mcpo::HttpResponse& response, std::string& transport_error,
                const mcpo::HttpHeartbeat&, std::chrono::steady_clock::duration) {
                ++network_attempts;
                if (network_attempts <= 3U) {
                    response.failure_kind =
                        mcpo::HttpFailureKind::temporary_network;
                    transport_error = "temporary network failure";
                    return false;
                }
                response.status = 200U;
                response.content_type = "application/json";
                response.body = registry_page(
                    registry_entry("network/recovered", "1"));
                response.effective_url = url;
                return true;
            };
        require(mcpo::collect_registry(
                    options, message, temporary_network_transport),
                "temporary network failure should recover");
        require(
            network_attempts == 4U &&
                network_wait == std::chrono::seconds(11),
            "exponential retry delays double and cap without overflow");

        options = {};
        options.output = parent / "request-timeout-recovery";
        options.registry_base_url = "http://127.0.0.1:8080/base";
        fake_now = std::chrono::steady_clock::time_point{};
        options.now = [&] { return fake_now; };
        options.wait = [&](std::chrono::steady_clock::duration duration) {
            fake_now += duration;
        };
        std::size_t timeout_attempts{};
        mcpo::HttpTransport recovering_timeout_transport =
            [&](const std::string& url, std::chrono::steady_clock::duration, std::size_t,
                mcpo::HttpResponse& response, std::string& transport_error,
                const mcpo::HttpHeartbeat&, std::chrono::steady_clock::duration) {
                ++timeout_attempts;
                if (timeout_attempts == 1U) {
                    response.failure_kind =
                        mcpo::HttpFailureKind::timeout;
                    transport_error = "request timed out";
                    return false;
                }
                response.status = 200U;
                response.content_type = "application/json";
                response.body = registry_page(
                    registry_entry("timeout/recovered", "1"));
                response.effective_url = url;
                return true;
            };
        require(mcpo::collect_registry(
                    options, message, recovering_timeout_transport),
                "request timeout should be retryable");
        require(timeout_attempts == 2U,
                "request timeout consumed one retry");

        options = {};
        options.output = parent / "retry-stall-deadline";
        options.registry_base_url = "http://127.0.0.1:8080/base";
        options.runtime.stall_timeout = std::chrono::seconds(3);
        options.runtime.retry_initial = std::chrono::seconds(10);
        options.runtime.retry_maximum = std::chrono::seconds(10);
        fake_now = std::chrono::steady_clock::time_point{};
        options.now = [&] { return fake_now; };
        std::chrono::steady_clock::duration stall_wait{};
        options.wait = [&](std::chrono::steady_clock::duration duration) {
            stall_wait += duration;
            fake_now += duration;
        };
        mcpo::HttpTransport retry_stall_transport =
            [&](const std::string&, std::chrono::steady_clock::duration, std::size_t,
                mcpo::HttpResponse& response, std::string& transport_error,
                const mcpo::HttpHeartbeat&, std::chrono::steady_clock::duration) {
                response.failure_kind =
                    mcpo::HttpFailureKind::temporary_network;
                transport_error = "temporary network failure";
                return false;
            };
        require(!mcpo::collect_registry(
                    options, message, retry_stall_transport),
                "retry wait should stop at stall deadline");
        require(
            stall_wait == std::chrono::seconds(3) &&
                message.find("stall deadline") != std::string::npos,
            "retry sleep did not pass durable-progress deadline");

        options.output = parent / "retry-total-deadline";
        options.runtime.stall_timeout = std::chrono::seconds(30);
        options.runtime.run_timeout = std::chrono::seconds(3);
        fake_now = std::chrono::steady_clock::time_point{};
        std::chrono::steady_clock::duration total_wait{};
        options.wait = [&](std::chrono::steady_clock::duration duration) {
            total_wait += duration;
            fake_now += duration;
        };
        require(!mcpo::collect_registry(
                    options, message, retry_stall_transport),
                "retry wait should stop at total deadline");
        require(
            total_wait == std::chrono::seconds(3) &&
                message.find("total run deadline") != std::string::npos,
            "retry sleep did not pass optional total deadline");

        options = {};
        options.output = parent / "nonretry-http";
        options.registry_base_url = "http://127.0.0.1:8080/base";
        std::size_t nonretry_attempts{};
        mcpo::HttpTransport nonretry_transport =
            [&](const std::string& url, std::chrono::steady_clock::duration, std::size_t,
                mcpo::HttpResponse& response, std::string&,
                const mcpo::HttpHeartbeat&, std::chrono::steady_clock::duration) {
                ++nonretry_attempts;
                response.status = 400U;
                response.effective_url = url;
                return true;
            };
        require(!mcpo::collect_registry(
                    options, message, nonretry_transport),
                "HTTP 400 should not be retried");
        require(nonretry_attempts == 1U, "HTTP 400 had one attempt");

        options = {};
        options.output = parent / "malformed-no-retry";
        options.registry_base_url = "http://127.0.0.1:8080/base";
        std::size_t malformed_attempts{};
        mcpo::HttpTransport malformed_transport =
            [&](const std::string& url, std::chrono::steady_clock::duration, std::size_t,
                mcpo::HttpResponse& response, std::string&,
                const mcpo::HttpHeartbeat&, std::chrono::steady_clock::duration) {
                ++malformed_attempts;
                response.status = 200U;
                response.content_type = "application/json";
                response.body = "{bad";
                response.effective_url = url;
                return true;
            };
        require(!mcpo::collect_registry(
                    options, message, malformed_transport),
                "malformed registry JSON should fail");
        require(malformed_attempts == 1U,
                "malformed JSON was not retried");

        options = {};
        options.output = parent / "checkpoint-failure";
        options.registry_base_url = "http://127.0.0.1:8080/base";
        std::size_t checkpoint_failure_attempts{};
        mcpo::HttpTransport checkpoint_failure_transport =
            [&](const std::string& url, std::chrono::steady_clock::duration, std::size_t,
                mcpo::HttpResponse& response, std::string&,
                const mcpo::HttpHeartbeat&, std::chrono::steady_clock::duration) {
                ++checkpoint_failure_attempts;
                for (const auto& entry :
                     std::filesystem::directory_iterator(parent)) {
                    if (!entry.path().filename().string().starts_with(
                            "checkpoint-failure.partial-"))
                        continue;
                    std::error_code local_error;
                    std::filesystem::remove(
                        entry.path() / "checkpoint.json", local_error);
                    std::filesystem::create_directory(
                        entry.path() / "checkpoint.json", local_error);
                }
                response.status = 200U;
                response.content_type = "application/json";
                response.body = registry_page(
                    registry_entry("checkpoint/failure", "1"));
                response.effective_url = url;
                return true;
            };
        require(!mcpo::collect_registry(
                    options, message, checkpoint_failure_transport),
                "checkpoint persistence failure should abort");
        require(
            checkpoint_failure_attempts == 1U &&
                message.find("compact checkpoint") != std::string::npos,
            "checkpoint persistence failure is fatal and not retried");

        const auto read_collector_bytes =
            [](const std::filesystem::path& path) {
                std::ifstream input(path, std::ios::binary);
                return std::string(
                    std::istreambuf_iterator<char>(input),
                    std::istreambuf_iterator<char>());
            };
        const std::string compact =
            read_collector_bytes(parent / "multi/checkpoint.json");
        require(
            compact.find("\"checkpoint_version\":3") != std::string::npos &&
                compact.find("\"last_completed_page\":2") !=
                    std::string::npos &&
                compact.find("\"pages_metadata_path\":\"raw/pages.jsonl\"") !=
                    std::string::npos &&
                compact.find("\"artifacts\"") == std::string::npos,
            "completed pages leave a compact version-2 resume head");
        const auto one_checkpoint_size =
            std::filesystem::file_size(parent / "one/checkpoint.json");
        const auto multi_checkpoint_size =
            std::filesystem::file_size(parent / "multi/checkpoint.json");
        require(
            multi_checkpoint_size <= one_checkpoint_size + 64U,
            "checkpoint size is not proportional to page history");

        options = {};
        options.output = parent / "durable-partial";
        options.registry_base_url = "http://127.0.0.1:8080/base";
        options.runtime.maximum_attempts_per_page = 1U;
        std::size_t partial_requests{};
        mcpo::HttpTransport partial_transport =
            [&](const std::string& url, std::chrono::steady_clock::duration, std::size_t,
                mcpo::HttpResponse& response, std::string&,
                const mcpo::HttpHeartbeat&, std::chrono::steady_clock::duration) {
                response.effective_url = url;
                if (partial_requests++ == 0U) {
                    response.status = 200U;
                    response.content_type = "application/json";
                    response.body = registry_page(
                        registry_entry("resume/one", "1"),
                        "resume-cursor");
                } else {
                    response.status = 400U;
                }
                return true;
            };
        require(!mcpo::collect_registry(
                    options, message, partial_transport),
                "second-page failure retains partial bundle");
        std::filesystem::path durable_partial;
        for (const auto& entry :
             std::filesystem::directory_iterator(parent)) {
            const std::string filename =
                entry.path().filename().string();
            if (filename.starts_with("durable-partial.partial-"))
                durable_partial = entry.path();
        }
        require(!durable_partial.empty(),
                "retained partial bundle path is discoverable");
        {
            std::ofstream uncommitted(
                durable_partial / "raw/page-000002.json",
                std::ios::binary | std::ios::trunc);
            uncommitted << registry_page(
                registry_entry("uncommitted/server", "1"));
        }
        options.output = parent / "resume-first-uncommitted";
        options.resume = durable_partial;
        std::size_t resumed_requests{};
        mcpo::HttpTransport resume_head_transport =
            [&](const std::string& url, std::chrono::steady_clock::duration, std::size_t,
                mcpo::HttpResponse& response, std::string&,
                const mcpo::HttpHeartbeat&, std::chrono::steady_clock::duration) {
                require(
                    url.find("cursor=resume-cursor") != std::string::npos,
                    "resume uses cursor from last committed page");
                ++resumed_requests;
                response.status = 200U;
                response.content_type = "application/json";
                response.body = registry_page(
                    registry_entry("resume/two", "1"));
                response.effective_url = url;
                return true;
            };
        require(mcpo::collect_registry(
                    options, message, resume_head_transport),
                "resume redownloads first uncommitted page");
        require(resumed_requests == 1U,
                "committed pages were not redownloaded");
        require(
            read_collector_bytes(
                parent /
                "resume-first-uncommitted/canonical/servers.jsonl")
                    .find("uncommitted/server") == std::string::npos,
            "uncheckpointed raw page was ignored");

        {
            std::ofstream corrupt(
                durable_partial / "raw/page-000001.json",
                std::ios::binary | std::ios::app);
            corrupt << ' ';
        }
        options.output = parent / "resume-corrupt";
        std::size_t corrupt_resume_requests{};
        require(!mcpo::collect_registry(
                    options, message,
                    [&](const std::string&, std::chrono::steady_clock::duration, std::size_t,
                        mcpo::HttpResponse&, std::string&,
                        const mcpo::HttpHeartbeat&,
                        std::chrono::steady_clock::duration) {
                        ++corrupt_resume_requests;
                        return false;
                    }),
                "resume rejects a last-page digest mismatch");
        require(corrupt_resume_requests == 0U,
                "invalid checkpoint state is rejected before network access");

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

    {
        // npm metadata parsing and integrity verification
        std::string error;
        mcpo::NpmDistMetadata metadata;
        require(
            mcpo::parse_npm_version_metadata(
                R"json({"name":"demo","version":"1.2.3","dist":{"tarball":"https://example/demo-1.2.3.tgz","integrity":"sha512-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=="}})json",
                "demo",
                "1.2.3",
                metadata,
                error),
            "nested dist metadata should parse");
        require(metadata.tarball_url == "https://example/demo-1.2.3.tgz", "tarball url");
        require(
            mcpo::parse_npm_version_metadata(
                R"json({"name":"demo","version":"1.2.3","dist.tarball":"https://example/flat.tgz","dist.integrity":"sha512-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=="})json",
                "demo",
                "1.2.3",
                metadata,
                error),
            "flattened dist keys should parse");
        require(metadata.tarball_url == "https://example/flat.tgz", "flattened tarball");

        const std::string payload = "fixture-bytes-for-integrity";
        // Generate integrity with openssl in-process via analyze API after computing expected.
        // Use a known sha512 integrity by verifying mismatch first.
        require(
            !mcpo::verify_npm_integrity(
                payload,
                "sha512-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA==",
                error),
            "integrity mismatch should fail");
        require(!error.empty(), "integrity failure should set error");
    }

    {
        // Exact PyPI release parsing selects only the minimal supported sdist.
        std::string error;
        mcpo::ArtifactDescriptor artifact;
        mcpo::AcquisitionLimits limits;
        require(
            mcpo::parse_pypi_release_metadata(
                R"json({"info":{"name":"Demo_Package","version":"2.0.0"},"urls":[{"filename":"demo_package-2.0.0-py3-none-any.whl","packagetype":"bdist_wheel","url":"https://files.pythonhosted.org/demo.whl","size":100,"digests":{"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"},"yanked":false},{"filename":"demo_package-2.0.0.tar.gz","packagetype":"sdist","url":"https://files.pythonhosted.org/demo.tar.gz","size":200,"digests":{"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"},"yanked":false}]})json",
                "demo-package",
                "2.0.0",
                limits,
                artifact,
                error),
            "PyPI exact release metadata should select its tar-gzip sdist");
        require(
            artifact.registry == mcpo::ArtifactRegistry::pypi &&
                artifact.filename == "demo_package-2.0.0.tar.gz" &&
                artifact.published_size == 200U,
            "PyPI artifact descriptor fields");
        require(
            !mcpo::parse_pypi_release_metadata(
                R"json({"info":{"name":"demo","version":"2.0.1"},"urls":[]})json",
                "demo",
                "2.0.0",
                limits,
                artifact,
                error),
            "PyPI metadata version mismatch should fail");
        require(
            !mcpo::parse_pypi_release_metadata(
                R"json({"info":{"name":"demo","name":"other","version":"2.0.0"},"urls":[]})json",
                "demo",
                "2.0.0",
                limits,
                artifact,
                error),
            "duplicate PyPI metadata keys should fail");
        require(
            !mcpo::parse_pypi_release_metadata(
                R"json({"info":{"name":"demo","version":"2.0.0"},"urls":[{"filename":"demo.tar.gz","packagetype":"sdist","url":"https://files.pythonhosted.org/demo.tar.gz","size":"200","digests":{"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"},"yanked":false}]})json",
                "demo",
                "2.0.0",
                limits,
                artifact,
                error),
            "PyPI file size encoded as a string should fail");

        require(
            mcpo::verify_pypi_integrity(
                "abc",
                "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
                3U,
                error),
            "PyPI integrity should accept a matching case-insensitive SHA-256");
        require(
            !mcpo::verify_pypi_integrity(
                "abc",
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                4U,
                error),
            "PyPI integrity should reject a published size mismatch");
        require(
            !mcpo::verify_pypi_integrity(
                "abd",
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                3U,
                error),
            "PyPI integrity should reject a SHA-256 mismatch");
    }

    {
        const auto parent = std::filesystem::temp_directory_path() /
            ("mcpo-analyze-unit-" + std::to_string(getpid()));
        std::error_code ec;
        std::filesystem::remove_all(parent, ec);
        std::filesystem::create_directories(parent / "package");
        {
            std::ofstream manifest(parent / "package" / "package.json");
            manifest << R"json({
  "name": "fixture-pkg",
  "version": "9.9.9",
  "license": "MIT",
  "main": "index.js",
  "bin": {"fixture-pkg": "index.js"},
  "engines": {"node": ">=18"},
  "scripts": {"prepare": "echo prepare", "preinstall": "echo pre"},
  "dependencies": {"leftpad": "1.0.0"},
  "devDependencies": {"tape": "5.0.0"},
  "repository": {"type": "git", "url": "git+https://example/repo.git"}
})json";
        }
        {
            std::ofstream js(parent / "package" / "index.js");
            js << "import fs from 'node:fs';\n"
               << "import { fileURLToPath } from 'node:url';\n"
               << "const ignored = /abc/.exec('abc');\n"
               << "const docs = 'see https://example.com/docs for fetch guidance';\n"
               << "const TOKEN = 'literal';\n";
        }
        {
            std::ofstream bad(parent / "package" / "danger.js");
            bad << "import cp from 'child_process';\ncp.exec('true');\n";
        }
        require(
            std::system(("tar -C '" + parent.string() +
                         "' -czf '" + (parent / "good.tgz").string() +
                         "' package").c_str()) == 0,
            "create good fixture tarball");

        std::ifstream input(parent / "good.tgz", std::ios::binary);
        const std::string bytes(
            (std::istreambuf_iterator<char>(input)),
            std::istreambuf_iterator<char>());
        mcpo::ArchiveLimits limits;
        const auto rules_path = std::filesystem::path(__FILE__).parent_path()
            .parent_path() / "rules/artifact-static-analysis-v1.json";
        mcpo::AnalyzerWorkerResult result;
        std::string error;
        require(
            mcpo::analyze_npm_tarball_bytes(
                bytes, rules_path, limits, result, error),
            "analyze fixture tarball");
        require(result.package_version == "9.9.9", "package version");
        require(result.lifecycle_scripts.size() >= 2U, "lifecycle scripts");
        bool saw_prepare = false;
        bool saw_fs = false;
        bool saw_url = false;
        bool saw_child = false;
        bool saw_regexp_false_positive = false;
        bool saw_docs_fetch = false;
        for (const auto& script : result.lifecycle_scripts)
            if (script.first == "prepare") saw_prepare = true;
        for (const auto& finding : result.findings) {
            if (finding.symbol && *finding.symbol == "fs") saw_fs = true;
            if (finding.symbol && *finding.symbol == "url") saw_url = true;
            if (finding.symbol && *finding.symbol == "child_process") saw_child = true;
            if (finding.rule_id == "risk-api:exec" &&
                finding.evidence.find("RegExp") == std::string::npos &&
                finding.subject_path.find("index.js") != std::string::npos)
                saw_regexp_false_positive = true;
            if (finding.rule_id == "risk-api:fetch" &&
                finding.subject_path.find("index.js") != std::string::npos)
                saw_docs_fetch = true;
        }
        require(saw_prepare, "prepare lifecycle");
        require(saw_fs, "node:fs finding");
        require(saw_url, "node:url finding");
        require(saw_child, "child_process finding");
        require(!saw_regexp_false_positive, "RegExp.exec false positive");
        require(!saw_docs_fetch, "documentation URL false positive");
        require(result.dependencies.size() >= 2U, "dependencies extracted");
        require(!result.has_native_code, "no native code in fixture");

        std::filesystem::create_directories(parent / "mixed");
        {
            std::ofstream metadata(parent / "mixed" / "PKG-INFO");
            metadata << "Metadata-Version: 2.1\n"
                     << "Name: python-fixture\n"
                     << "Version: 4.5.6\n"
                     << "Requires-Dist: requests[security]>=2\n";
        }
        {
            std::ofstream bundled_manifest(parent / "mixed" / "package.json");
            bundled_manifest
                << R"json({"name":"bundled-frontend","version":"99.0.0"})json";
        }
        require(
            std::system(("tar -C '" + parent.string() +
                         "' -czf '" + (parent / "mixed.tgz").string() +
                         "' mixed").c_str()) == 0,
            "create mixed PyPI fixture tarball");
        std::ifstream mixed_input(parent / "mixed.tgz", std::ios::binary);
        const std::string mixed_bytes(
            (std::istreambuf_iterator<char>(mixed_input)),
            std::istreambuf_iterator<char>());
        mcpo::AnalyzerWorkerResult pypi_result;
        require(
            mcpo::analyze_package_tarball_bytes(
                "pypi", mixed_bytes, rules_path, limits, pypi_result, error),
            "PyPI worker should select PKG-INFO over bundled package.json");
        require(
            pypi_result.package_name == "python-fixture" &&
                pypi_result.package_version == "4.5.6",
            "PyPI worker identity should come from PKG-INFO");
        require(
            pypi_result.dependencies.size() == 1U &&
                pypi_result.dependencies.front().dependency_name == "requests" &&
                pypi_result.dependencies.front().declared_version ==
                    "requests[security]>=2",
            "PyPI dependency extras should not be part of the project name");
        mcpo::AnalyzerWorkerResult missing_pypi_metadata;
        require(
            !mcpo::analyze_package_tarball_bytes(
                "pypi",
                bytes,
                rules_path,
                limits,
                missing_pypi_metadata,
                error),
            "PyPI worker must reject a tarball without root PKG-INFO");

        // Rules are loaded from JSON: changing policy changes findings without
        // recompiling the analyzer.
        std::ifstream rules_input(rules_path);
        std::string custom_rules(
            (std::istreambuf_iterator<char>(rules_input)),
            std::istreambuf_iterator<char>());
        const std::size_t child_rule =
            custom_rules.find("\"symbol\": \"child_process\"");
        require(child_rule != std::string::npos, "child_process rule fixture");
        const std::size_t child_severity =
            custom_rules.find("\"severity\": \"high\"", child_rule);
        require(child_severity != std::string::npos, "child_process severity fixture");
        custom_rules.replace(
            child_severity,
            std::string("\"severity\": \"high\"").size(),
            "\"severity\": \"low\"");
        const std::size_t ruleset_version =
            custom_rules.find("\"ruleset_version\": \"1.0.0\"");
        require(ruleset_version != std::string::npos, "ruleset version fixture");
        custom_rules.replace(
            ruleset_version,
            std::string("\"ruleset_version\": \"1.0.0\"").size(),
            "\"ruleset_version\": \"test-low-child-process\"");
        const auto custom_rules_path = parent / "custom-rules.json";
        {
            std::ofstream custom_rules_out(custom_rules_path);
            custom_rules_out << custom_rules;
        }
        mcpo::AnalyzerWorkerResult custom_result;
        require(
            mcpo::analyze_npm_tarball_bytes(
                bytes, custom_rules_path, limits, custom_result, error),
            "custom analysis rules should load");
        bool child_became_low = false;
        for (const auto& finding : custom_result.findings) {
            if (finding.rule_id == "node-builtin:child_process" &&
                finding.severity == "low")
                child_became_low = true;
        }
        require(child_became_low, "custom rules should control severity");
        require(
            custom_result.ruleset_version == "test-low-child-process",
            "worker should report loaded ruleset version");

        const auto malformed_rules_path = parent / "malformed-rules.json";
        {
            std::ofstream malformed_rules(malformed_rules_path);
            malformed_rules << R"json({"schema_version":1})json";
        }
        mcpo::AnalyzerWorkerResult malformed_rules_result;
        require(
            !mcpo::analyze_npm_tarball_bytes(
                bytes,
                malformed_rules_path,
                limits,
                malformed_rules_result,
                error),
            "incomplete analysis rules should fail closed");
        require(
            error.find("ruleset_version") != std::string::npos,
            "malformed rules error should identify missing field");

        // absolute path rejection
        std::filesystem::create_directories(parent / "abs");
        require(
            std::system(("tar -C '" + (parent / "abs").string() +
                         "' --absolute-names -czf '" +
                         (parent / "abs.tgz").string() +
                         "' -T /dev/null 2>/dev/null; "
                         "python3 - <<'PY'\n"
                         "import tarfile, io\n"
                         "path='" +
                         (parent / "abs.tgz").string() +
                         "'\n"
                         "with tarfile.open(path,'w:gz') as t:\n"
                         "    info=tarfile.TarInfo('/etc/passwd')\n"
                         "    data=b'x'\n"
                         "    info.size=len(data)\n"
                         "    t.addfile(info, io.BytesIO(data))\n"
                         "PY")
                            .c_str()) == 0,
            "create absolute-path tarball");
        {
            std::ifstream abs_input(parent / "abs.tgz", std::ios::binary);
            const std::string abs_bytes(
                (std::istreambuf_iterator<char>(abs_input)),
                std::istreambuf_iterator<char>());
            mcpo::AnalyzerWorkerResult abs_result;
            require(
                !mcpo::analyze_npm_tarball_bytes(
                    abs_bytes, rules_path, limits, abs_result, error),
                "absolute path rejected");
            require(error.find("absolute") != std::string::npos, "absolute message");
        }

        // parent traversal
        {
            require(
                std::system(("python3 - <<'PY'\n"
                             "import tarfile, io\n"
                             "path='" +
                             (parent / "trav.tgz").string() +
                             "'\n"
                             "with tarfile.open(path,'w:gz') as t:\n"
                             "    info=tarfile.TarInfo('package/../evil')\n"
                             "    data=b'x'\n"
                             "    info.size=len(data)\n"
                             "    t.addfile(info, io.BytesIO(data))\n"
                             "PY")
                                .c_str()) == 0,
                "create traversal tarball");
            std::ifstream trav_input(parent / "trav.tgz", std::ios::binary);
            const std::string trav_bytes(
                (std::istreambuf_iterator<char>(trav_input)),
                std::istreambuf_iterator<char>());
            mcpo::AnalyzerWorkerResult trav_result;
            require(
                !mcpo::analyze_npm_tarball_bytes(
                    trav_bytes, rules_path, limits, trav_result, error),
                "traversal rejected");
            require(error.find("parent-traversal") != std::string::npos, "traversal message");
        }

        // escaping symlink
        {
            require(
                std::system(("python3 - <<'PY'\n"
                             "import tarfile, io\n"
                             "path='" +
                             (parent / "link.tgz").string() +
                             "'\n"
                             "with tarfile.open(path,'w:gz') as t:\n"
                             "    info=tarfile.TarInfo('package/link')\n"
                             "    info.type=tarfile.SYMTYPE\n"
                             "    info.linkname='/tmp/escape'\n"
                             "    t.addfile(info)\n"
                             "    man=tarfile.TarInfo('package/package.json')\n"
                             "    data=b'{\"name\":\"x\",\"version\":\"1.0.0\"}'\n"
                             "    man.size=len(data)\n"
                             "    t.addfile(man, io.BytesIO(data))\n"
                             "PY")
                                .c_str()) == 0,
                "create symlink tarball");
            std::ifstream link_input(parent / "link.tgz", std::ios::binary);
            const std::string link_bytes(
                (std::istreambuf_iterator<char>(link_input)),
                std::istreambuf_iterator<char>());
            mcpo::AnalyzerWorkerResult link_result;
            require(
                !mcpo::analyze_npm_tarball_bytes(
                    link_bytes, rules_path, limits, link_result, error),
                "escaping symlink rejected");
        }

        // file count limit
        {
            limits.maximum_files = 1U;
            mcpo::AnalyzerWorkerResult limited;
            require(
                !mcpo::analyze_npm_tarball_bytes(
                    bytes, rules_path, limits, limited, error),
                "file count limit");
            require(error.find("file count") != std::string::npos, "file count message");
            limits.maximum_files = 10'000U;
        }

        // individual file size limit
        {
            limits.maximum_individual_file_bytes = 8U;
            mcpo::AnalyzerWorkerResult limited;
            require(
                !mcpo::analyze_npm_tarball_bytes(
                    bytes, rules_path, limits, limited, error),
                "individual size limit");
            limits.maximum_individual_file_bytes = 8U * 1024U * 1024U;
        }

        // total size limit
        {
            limits.maximum_total_uncompressed_bytes = 32U;
            mcpo::AnalyzerWorkerResult limited;
            require(
                !mcpo::analyze_npm_tarball_bytes(
                    bytes, rules_path, limits, limited, error),
                "total size limit");
            limits.maximum_total_uncompressed_bytes = 64U * 1024U * 1024U;
        }

        // native binary detection
        {
            require(
                std::system(("python3 - <<'PY'\n"
                             "import tarfile, io\n"
                             "path='" +
                             (parent / "elf.tgz").string() +
                             "'\n"
                             "with tarfile.open(path,'w:gz') as t:\n"
                             "    man=tarfile.TarInfo('package/package.json')\n"
                             "    data=b'{\"name\":\"x\",\"version\":\"1.0.0\"}'\n"
                             "    man.size=len(data)\n"
                             "    t.addfile(man, io.BytesIO(data))\n"
                             "    elf=tarfile.TarInfo('package/native.node')\n"
                             "    payload=b'\\x7fELF'+b'\\0'*12\n"
                             "    elf.size=len(payload)\n"
                             "    t.addfile(elf, io.BytesIO(payload))\n"
                             "PY")
                                .c_str()) == 0,
                "create elf tarball");
            std::ifstream elf_input(parent / "elf.tgz", std::ios::binary);
            const std::string elf_bytes(
                (std::istreambuf_iterator<char>(elf_input)),
                std::istreambuf_iterator<char>());
            mcpo::AnalyzerWorkerResult elf_result;
            require(
                mcpo::analyze_npm_tarball_bytes(
                    elf_bytes, rules_path, limits, elf_result, error),
                "analyze elf fixture");
            require(elf_result.has_native_code, "native binary detected");
        }

        // successful integrity with generated sha512
        std::string integrity;
        {
            require(
                std::system(("python3 - <<'PY'\n"
                             "import hashlib, base64, pathlib\n"
                             "data=pathlib.Path('" +
                             (parent / "good.tgz").string() +
                             "').read_bytes()\n"
                             "digest=base64.b64encode(hashlib.sha512(data).digest()).decode()\n"
                             "pathlib.Path('" +
                             (parent / "good.integrity").string() +
                             "').write_text('sha512-'+digest)\n"
                             "PY")
                                .c_str()) == 0,
                "create integrity text");
            std::ifstream integrity_input(parent / "good.integrity");
            std::getline(integrity_input, integrity);
            require(
                mcpo::verify_npm_integrity(bytes, integrity, error),
                "sha512 integrity success");
        }

        // Container staging remains readable by the fixed unprivileged worker
        // identity even when the parent process uses a restrictive umask.
        {
            mcpo::RegistryCollectOptions collect;
            collect.output = parent / "staging-permissions-bundle";
            collect.registry_base_url = "http://127.0.0.1:8080/base";
            std::string message;
            require(
                mcpo::collect_registry(
                    collect,
                    message,
                    fixture_transport({registry_page(registry_entry(
                        "io.example/staging-permissions",
                        "9.9.9",
                        R"json(,"packages":[{"registryType":"npm","identifier":"fixture-pkg","version":"9.9.9","transport":{"type":"stdio"}}])json"))})),
                "collect staging-permissions registry fixture");

            mcpo::RegistryIndexOptions index;
            index.bundle = collect.output;
            index.database = parent / "staging-permissions.sqlite";
            require(
                mcpo::index_registry_bundle(index).ok(),
                "index staging-permissions registry fixture");

            mcpo::AnalyzePackageOptions options;
            options.database = index.database;
            options.server_identifier = "io.example/staging-permissions";
            options.server_version = "9.9.9";
            options.package_identifier = "fixture-pkg";
            options.evidence_root = parent / "staging-permissions-evidence";
            options.rules_path = rules_path;

            const std::string metadata_json =
                std::string(
                    R"json({"name":"fixture-pkg","version":"9.9.9","dist":{"tarball":"https://example.invalid/fixture-pkg-9.9.9.tgz","integrity":")json") +
                integrity + R"json("}})json";
            std::size_t download_count{};
            mcpo::AnalyzeDownloadTransport download =
                [&](const std::string&,
                    std::chrono::steady_clock::duration,
                    std::size_t,
                    std::string& body,
                    std::string&) {
                    body = download_count++ == 0U ? metadata_json : bytes;
                    return true;
                };

            bool worker_read_both_inputs = false;
            bool staging_permissions_are_restricted = false;
            bool input_permissions_are_container_readable = false;
            mcpo::AnalyzeWorkerRunner worker =
                [&](std::string_view registry_type,
                    const std::filesystem::path& tarball,
                    const std::filesystem::path& staged_rules,
                    const mcpo::ArchiveLimits& worker_limits,
                    mcpo::AnalyzerWorkerResult& worker_result,
                    std::string& raw_json,
                    std::string& worker_error) {
                    const auto permission_bits = [](const std::filesystem::path& path) {
                        std::error_code status_error;
                        const auto status = std::filesystem::status(path, status_error);
                        return status_error
                            ? std::filesystem::perms::unknown
                            : status.permissions() & std::filesystem::perms::mask;
                    };
                    constexpr auto read_only_for_all =
                        std::filesystem::perms::owner_read |
                        std::filesystem::perms::group_read |
                        std::filesystem::perms::others_read;
                    staging_permissions_are_restricted =
                        permission_bits(tarball.parent_path()) ==
                        std::filesystem::perms::owner_all;
                    input_permissions_are_container_readable =
                        permission_bits(tarball) == read_only_for_all &&
                        permission_bits(staged_rules) == read_only_for_all;

                    std::ifstream artifact_input(tarball, std::ios::binary);
                    std::ifstream rules_input(staged_rules, std::ios::binary);
                    worker_read_both_inputs =
                        artifact_input.good() && rules_input.good();
                    raw_json.clear();
                    return worker_read_both_inputs && registry_type == "npm" &&
                        mcpo::analyze_package_tarball_bytes(
                            registry_type,
                            std::string(
                                std::istreambuf_iterator<char>(artifact_input),
                                std::istreambuf_iterator<char>()),
                            staged_rules,
                            worker_limits,
                            worker_result,
                            worker_error);
                };

            const mode_t previous_umask = umask(0077);
            const auto package_result =
                mcpo::analyze_package(options, download, worker);
            umask(previous_umask);

            require(package_result.ok(), "restrictive-umask package analysis");
            require(download_count == 2U, "metadata and artifact downloaded");
            require(
                staging_permissions_are_restricted,
                "analysis staging directory must be mode 0700");
            require(
                input_permissions_are_container_readable,
                "container inputs must be mode 0444");
            require(
                worker_read_both_inputs,
                "unprivileged analyzer boundary reads artifact and rules");
        }

        std::filesystem::remove_all(parent, ec);
    }

    return 0;
}
