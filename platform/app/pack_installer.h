#ifndef MDKR64_PACK_INSTALLER_H
#define MDKR64_PACK_INSTALLER_H

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

// Intake budgets, not runtime memory limits. ZIP bytes are compressed bytes;
// folder entries include directories (even empty ones) and regular files.
// ZIP member count is also bounded by maxEntries; maxExpandedBytes bounds the
// sum of declared uncompressed member sizes without inflating the archive.
struct PackInstallLimits {
    std::uintmax_t maxCopyBytes = std::uintmax_t{8} * 1024 * 1024 * 1024;
    std::uintmax_t maxEntries = 65536;
    std::uintmax_t maxExpandedBytes = std::uintmax_t{8} * 1024 * 1024 * 1024;
    std::uintmax_t maxZipMetadataBytes = 64u * 1024u * 1024u;
};

struct PackInstallResult {
    bool installed = false;
    std::filesystem::path destination;
    std::string message;
};

// A background copy, not yet visible to the content-pack scanner. Move the
// result back to the launcher thread and call finalize() there so publication
// follows the current frame's cancellation/quit input and registry refresh.
class PreparedPackInstall {
public:
    PreparedPackInstall() = default;
    PreparedPackInstall(const PreparedPackInstall &) = delete;
    PreparedPackInstall &operator=(const PreparedPackInstall &) = delete;
    PreparedPackInstall(PreparedPackInstall &&other) noexcept;
    PreparedPackInstall &operator=(PreparedPackInstall &&other) noexcept;
    ~PreparedPackInstall();

    static PreparedPackInstall copyZip(const std::filesystem::path &source,
                                       const std::filesystem::path &modsDir,
                                       const std::function<bool()> &cancelled = {},
                                       PackInstallLimits limits = {});
    static PreparedPackInstall copyPack(const std::filesystem::path &source,
                                        const std::filesystem::path &modsDir,
                                        const std::function<bool()> &cancelled = {},
                                        PackInstallLimits limits = {});
    // Recheck cancellation before validation and immediately before the atomic
    // move. Cancellation after that commit point does not undo an installation.
    PackInstallResult finalize(const std::function<bool()> &cancelled = {});

private:
    void cleanup() noexcept;
    std::filesystem::path stage_;
    // Prepared before directory creation; cleanup must not allocate filename().
    std::filesystem::path stageName_;
    std::filesystem::path modsDir_;
    std::filesystem::path filename_;
    std::string error_;
#ifndef _WIN32
    int modsFd_ = -1;
    int stageFd_ = -1;
#else
    intptr_t modsHandle_ = -1;
    intptr_t stageHandle_ = -1;
#endif
};

#endif
