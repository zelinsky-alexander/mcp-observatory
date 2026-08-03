#include "observatory/catalog_lock.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace mcpo {

std::filesystem::path catalog_writer_lock_path(
    const std::filesystem::path& database) {
    return std::filesystem::path(database.string() + ".writer.lock");
}

CatalogWriterLock::~CatalogWriterLock() {
    if (descriptor_ >= 0) close(descriptor_);
}

bool CatalogWriterLock::acquire(
    const std::filesystem::path& database,
    std::string& error) {
    if (descriptor_ >= 0) return true;
    const std::filesystem::path path = catalog_writer_lock_path(database);
    descriptor_ = open(
        path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor_ < 0) {
        error = "open catalog writer lock " + path.string() + ": " +
            std::strerror(errno);
        return false;
    }
    while (flock(descriptor_, LOCK_EX) != 0) {
        if (errno == EINTR) continue;
        error = "acquire catalog writer lock " + path.string() + ": " +
            std::strerror(errno);
        close(descriptor_);
        descriptor_ = -1;
        return false;
    }
    return true;
}

}  // namespace mcpo
