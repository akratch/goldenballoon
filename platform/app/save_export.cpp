#include "save_export.h"
#include "../save_container.h"
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <vector>
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#endif

namespace {
std::tm utcNow() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    return utc;
}
bool writeNew(const std::filesystem::path &path, const char *bytes,
              size_t size, std::string &error) {
#ifdef _WIN32
    const int fd = _wopen(path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                         _S_IREAD | _S_IWRITE);
#else
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
#endif
    if (fd < 0) {
        error = errno == EEXIST
            ? "That file already exists. Choose a new filename."
            : "Could not create the export. Choose a writable folder.";
        return false;
    }
    size_t offset = 0;
    while (offset < size) {
#ifdef _WIN32
        const int written = _write(fd, bytes + offset, static_cast<unsigned>(size - offset));
#else
        const ssize_t written = write(fd, bytes + offset, size - offset);
#endif
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) break;
        offset += static_cast<size_t>(written);
    }
#ifdef _WIN32
    const bool synced = offset == size && _commit(fd) == 0;
    const bool closed = _close(fd) == 0;
#else
    const bool synced = offset == size && fsync(fd) == 0;
    const bool closed = close(fd) == 0;
#endif
    if (!synced || !closed) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        error = "The export could not be written completely. Try another folder.";
        return false;
    }
    return true;
}
}

namespace SaveExport {
std::string suggestedFilename() {
    const auto utc = utcNow();
    char name[96];
    std::strftime(name, sizeof(name), "Golden-Balloon-Save-%Y%m%d-%H%M%S.mdkr-save", &utc);
    return name;
}
bool progress(const std::string &saveDirectory, const std::string &destination,
              const char *version, std::string &error) {
    error.clear();
    if (saveDirectory.empty() || destination.empty()) {
        error = "Choose a destination for your save export.";
        return false;
    }
    const auto source = std::filesystem::u8path(saveDirectory) / "eeprom.bin";
    std::ifstream input(source, std::ios::binary);
    if (!input) {
        error = "No saved progress could be read. Play and save first, then export.";
        return false;
    }
    uint8_t payload[MDKR_SAVE_IMAGE_SIZE];
    input.read(reinterpret_cast<char *>(payload), sizeof(payload));
    if (input.gcount() != sizeof(payload) || input.peek() != std::char_traits<char>::eof() || input.bad()) {
        error = "The saved progress has an unexpected size. The original save was kept.";
        return false;
    }
    MdkrSaveDocument document;
    MdkrSaveReport report;
    if (mdkr_save_decode(payload, sizeof(payload), &document) != MDKR_SAVE_OK ||
        mdkr_save_validate(&document, &report) != MDKR_SAVE_OK || report.corrupt_block_count) {
        error = "The saved progress needs recovery before it can be exported. The original save was kept.";
        return false;
    }
    MdkrSaveContainerMetadata metadata{};
    const auto utc = utcNow();
    std::strftime(metadata.created_at, sizeof(metadata.created_at), "%Y-%m-%dT%H:%M:%SZ", &utc);
    std::snprintf(metadata.app_version, sizeof(metadata.app_version), "%s", version ? version : "");
    std::snprintf(metadata.source, sizeof(metadata.source), "%s", "native");
    size_t size = 0, capacity = 0;
    if (mdkr_save_container_encode(payload, &metadata, nullptr, 0, &size, &capacity) != MDKR_SAVE_CONTAINER_OK ||
        capacity > MDKR_SAVE_CONTAINER_MAX_INPUT) {
        error = "Could not prepare the portable save.";
        return false;
    }
    std::vector<char> container(capacity);
    if (mdkr_save_container_encode(payload, &metadata, container.data(), capacity, &size, nullptr) != MDKR_SAVE_CONTAINER_OK) {
        error = "Could not prepare the portable save.";
        return false;
    }
    return writeNew(std::filesystem::u8path(destination), container.data(), size, error);
}
}
