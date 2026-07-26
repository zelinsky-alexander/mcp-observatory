#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

int mcpo_original_main(int argc, char** argv);

namespace {

using Clock = std::chrono::steady_clock;

struct CollectArguments {
    bool enabled{};
    std::filesystem::path output;
    std::optional<std::filesystem::path> resume;
};

bool option_has_value(std::string_view argument) {
    return argument == "--output" || argument == "--resume" ||
        argument == "--registry-base-url" || argument == "--maximum-pages" ||
        argument == "--maximum-page-bytes" || argument == "--maximum-records" ||
        argument == "--maximum-redirects" ||
        argument == "--request-timeout-seconds" ||
        argument == "--run-timeout-seconds";
}

CollectArguments parse_collect_arguments(int argc, char** argv) {
    CollectArguments result;
    if (argc < 3 || std::string_view(argv[1]) != "registry" ||
        std::string_view(argv[2]) != "collect") {
        return result;
    }

    for (int index = 3; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--verbose") {
            result.enabled = true;
            continue;
        }
        if (!option_has_value(argument) || index + 1 >= argc) continue;

        const std::string_view value(argv[++index]);
        if (argument == "--output") {
            result.output = std::filesystem::path(std::string(value));
        } else if (argument == "--resume") {
            result.resume = std::filesystem::path(std::string(value));
        }
    }
    return result;
}

std::size_t count_raw_pages(const std::filesystem::path& root) {
    const std::filesystem::path raw = root / "raw";
    std::error_code error;
    if (!std::filesystem::is_directory(raw, error) || error) return 0U;

    std::size_t pages = 0U;
    for (std::filesystem::directory_iterator iterator(raw, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (!iterator->is_regular_file(error) || error) continue;
        const std::string name = iterator->path().filename().string();
        if (name.starts_with("page-") && name.ends_with(".json")) ++pages;
    }
    return error ? 0U : pages;
}

std::filesystem::path active_bundle_path(const std::filesystem::path& output) {
    std::error_code error;
    if (std::filesystem::is_directory(output, error) && !error) return output;

    const std::filesystem::path parent =
        output.parent_path().empty() ? std::filesystem::path(".") : output.parent_path();
    const std::string prefix = output.filename().string() + ".partial-";
    std::filesystem::path newest;
    std::filesystem::file_time_type newest_time{};

    for (std::filesystem::directory_iterator iterator(parent, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (!iterator->is_directory(error) || error) continue;
        const std::string name = iterator->path().filename().string();
        if (!name.starts_with(prefix)) continue;
        const auto modified = iterator->last_write_time(error);
        if (error) continue;
        if (newest.empty() || modified > newest_time) {
            newest = iterator->path();
            newest_time = modified;
        }
    }
    return newest;
}

std::uintmax_t page_size(const std::filesystem::path& bundle, std::size_t page) {
    char name[32]{};
    std::snprintf(name, sizeof(name), "page-%06zu.json", page);
    std::error_code error;
    const auto size = std::filesystem::file_size(bundle / "raw" / name, error);
    return error ? 0U : size;
}

void log_request_started(std::size_t page) {
    std::cerr << "[verbose] page request started: page=" << page << '\n' << std::flush;
}

int run_with_progress(int argc, char** argv, const CollectArguments& arguments) {
    const std::size_t resumed_pages =
        arguments.resume.has_value() ? count_raw_pages(*arguments.resume) : 0U;
    std::size_t observed_pages = resumed_pages;
    std::size_t requested_page = observed_pages + 1U;

    std::cerr << "[verbose] registry collection started: output=" << arguments.output.string();
    if (arguments.resume.has_value()) {
        std::cerr << " resume=" << arguments.resume->string()
                  << " completed_pages=" << resumed_pages;
    }
    std::cerr << '\n' << std::flush;
    log_request_started(requested_page);

    std::atomic<bool> finished{false};
    int exit_code = 1;
    std::thread worker([&] {
        exit_code = mcpo_original_main(argc, argv);
        finished.store(true, std::memory_order_release);
    });

    auto request_started_at = Clock::now();
    auto next_heartbeat = request_started_at + std::chrono::seconds(5);

    const auto inspect_progress = [&] {
        const std::filesystem::path bundle = active_bundle_path(arguments.output);
        if (bundle.empty()) return;
        const std::size_t pages = count_raw_pages(bundle);
        while (observed_pages < pages) {
            ++observed_pages;
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - request_started_at);
            std::cerr << "[verbose] page request completed: page=" << observed_pages
                      << " response_bytes=" << page_size(bundle, observed_pages)
                      << " elapsed_ms=" << elapsed.count() << '\n' << std::flush;
            requested_page = observed_pages + 1U;
            request_started_at = Clock::now();
            next_heartbeat = request_started_at + std::chrono::seconds(5);
            if (!finished.load(std::memory_order_acquire)) log_request_started(requested_page);
        }
    };

    while (!finished.load(std::memory_order_acquire)) {
        inspect_progress();
        const auto now = Clock::now();
        if (now >= next_heartbeat) {
            const auto waiting = std::chrono::duration_cast<std::chrono::seconds>(
                now - request_started_at);
            std::cerr << "[verbose] waiting for page response: page=" << requested_page
                      << " waiting_seconds=" << waiting.count() << '\n' << std::flush;
            next_heartbeat += std::chrono::seconds(5);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    worker.join();
    inspect_progress();
    std::cerr << "[verbose] registry collection finished: exit_code=" << exit_code
              << " completed_pages=" << observed_pages << '\n' << std::flush;
    return exit_code;
}

}  // namespace

int main(int argc, char** argv) {
    const CollectArguments arguments = parse_collect_arguments(argc, argv);
    if (!arguments.enabled || arguments.output.empty()) {
        return mcpo_original_main(argc, argv);
    }
    return run_with_progress(argc, argv, arguments);
}
