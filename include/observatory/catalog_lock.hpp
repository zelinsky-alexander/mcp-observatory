#pragma once

#include <filesystem>
#include <string>

namespace mcpo {

class CatalogWriterLock {
public:
    CatalogWriterLock() = default;
    CatalogWriterLock(const CatalogWriterLock&) = delete;
    CatalogWriterLock& operator=(const CatalogWriterLock&) = delete;
    ~CatalogWriterLock();

    [[nodiscard]] bool acquire(
        const std::filesystem::path& database,
        std::string& error);

private:
    int descriptor_{-1};
};

[[nodiscard]] std::filesystem::path catalog_writer_lock_path(
    const std::filesystem::path& database);

}  // namespace mcpo
