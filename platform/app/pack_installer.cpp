#include "pack_installer.h"

#include "mod_registry.h"
#include "mod_source_stream.h"
#include "fs_utf8.h"
#include "miniz.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <utility>
#include <memory>
#include <cstdlib>
#include <cstddef>
#include <limits>

#if MDKR_PACK_BY_PATH
#include <thread>
#include <vector>
#else
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#endif

namespace fs = std::filesystem;

namespace {

#if !MDKR_PACK_BY_PATH
struct ScopedFd {
    int value;
    explicit ScopedFd(int fd) : value(fd) {}
    ScopedFd(const ScopedFd &) = delete;
    ScopedFd &operator=(const ScopedFd &) = delete;
    ~ScopedFd() { if (value >= 0) close(value); }
};
// Every component is opened relative to the pinned staging root. Never let a
// replaced intermediate directory turn an output open into a symlink walk.
int relativeParent(int root, const fs::path &path) {
    int fd = openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    for (const auto &part : path.parent_path()) {
        if (fd < 0) break;
        if (part.empty() || part == "." || part == ".." || part == "/") {
            close(fd); errno = EINVAL; return -1;
        }
        int next = openat(fd, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(fd);
        fd = next;
    }
    return fd;
}

FILE *relativeFile(int root, const fs::path &path, bool write) {
    int parent = relativeParent(root, path);
    if (parent < 0) return nullptr;
    int fd = openat(parent, path.filename().c_str(), O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK |
        (write ? O_WRONLY | O_CREAT | O_EXCL : O_RDONLY), 0600);
    close(parent);
    if (fd < 0) return nullptr;
    struct stat status{};
    if (fstat(fd, &status) || !S_ISREG(status.st_mode)) { close(fd); return nullptr; }
    FILE *stream = fdopen(fd, write ? "wb" : "rb");
    if (!stream) close(fd);
    return stream;
}

bool relativeMkdir(int root, const fs::path &path, std::error_code &error) {
    int parent = relativeParent(root, path);
    if (parent < 0) { error = std::error_code(errno, std::generic_category()); return false; }
    int result = mkdirat(parent, path.filename().c_str(), 0700);
    if (result) error = std::error_code(errno, std::generic_category());
    close(parent);
    return result == 0;
}

// `follow` only for the caller-chosen mods root, which may be a link; every
// entry the installer owns is compared without following links.
bool sameDirectory(int fd, int parent, const char *name, bool follow = false) {
    struct stat owned{}, named{};
    return fd >= 0 && fstat(fd, &owned) == 0 &&
        fstatat(parent, name, &named, follow ? 0 : AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISDIR(named.st_mode) && owned.st_dev == named.st_dev &&
        owned.st_ino == named.st_ino;
}

// Walk through descriptors, never through the staging pathname. A replaced
// child link is unlinked as a link, not followed into another directory tree.
bool clearDirectory(int fd, unsigned depth = 0) {
    if (depth > MDKR_MOD_SOURCE_WALK_DEPTH_MAX + 2u) return false;
    int scan = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (scan < 0) return false;
    DIR *dir = fdopendir(scan);
    if (!dir) { close(scan); return false; }
    bool okay = true;
    for (;;) {
        errno = 0;
        auto *entry = readdir(dir);
        if (!entry) { if (errno) okay = false; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        struct stat status{};
        if (fstatat(fd, entry->d_name, &status, AT_SYMLINK_NOFOLLOW)) { okay = false; continue; }
        if (S_ISDIR(status.st_mode)) {
            int child = openat(fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) { okay = false; continue; }
            struct stat opened{};
            if (fstat(child, &opened) || opened.st_dev != status.st_dev || opened.st_ino != status.st_ino ||
                !clearDirectory(child, depth + 1) || !sameDirectory(child, fd, entry->d_name) ||
                unlinkat(fd, entry->d_name, AT_REMOVEDIR)) okay = false;
            close(child);
        } else if (unlinkat(fd, entry->d_name, 0)) okay = false;
    }
    closedir(dir);
    return okay;
}
#else
// The pathname arm. Every filesystem call goes through fs_utf8, the one UTF-8
// boundary, so the Windows logic is the same code these tests run on POSIX.
//
// Removal runs from cleanup(), which must not throw or use C++ allocation:
// paths here are malloc'd C strings, never std::filesystem objects.
char *joinPath(const char *parent, const char *name) {
    const size_t head = std::strlen(parent), tail = std::strlen(name);
    auto *joined = static_cast<char *>(std::malloc(head + tail + 2));
    if (!joined) return nullptr;
    std::memcpy(joined, parent, head);
    joined[head] = '/';
    std::memcpy(joined + head + 1, name, tail + 1);
    return joined;
}

// A security scanner can hold a just-deleted file open for a moment, which
// keeps its folder non-empty; retry briefly before giving up on the folder.
bool removeDirectory(const char *path) {
    for (int attempt = 0;; ++attempt) {
        if (mdkr_rmdir_utf8(path) == 0) return true;
#ifdef _WIN32
        if (attempt < 5 && (errno == ENOTEMPTY || errno == EACCES || errno == EBUSY)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
#endif
        (void)attempt;
        return false;
    }
}

bool clearDirectory(const char *directory, unsigned depth);

struct ClearPass {
    const char *directory;
    unsigned depth;
    bool removed;
    bool okay;
};

int clearEntry(const char *name, int kind, void *opaque) {
    auto &pass = *static_cast<ClearPass *>(opaque);
    char *child = joinPath(pass.directory, name);
    if (!child) {
        pass.okay = false;
        return 1;
    }
    bool removed;
    if (kind == MDKR_PATH_DIRECTORY) {
        removed = clearDirectory(child, pass.depth + 1) && removeDirectory(child);
    } else if (kind == MDKR_PATH_LINK) {
        // Remove the link itself, never what it points at. A Windows
        // directory link or junction is removed as a directory.
        removed = mdkr_unlink_utf8(child) == 0 || mdkr_rmdir_utf8(child) == 0;
    } else {
        removed = mdkr_unlink_utf8(child) == 0;
    }
    std::free(child);
    if (removed) pass.removed = true;
    else pass.okay = false;
    return 0;
}

// Empty `directory` without following links. A listing taken while entries
// are removed may miss some, so repeat while a pass makes progress.
bool clearDirectory(const char *directory, unsigned depth) {
    if (depth > MDKR_MOD_SOURCE_WALK_DEPTH_MAX + 2u) return false;
    for (int round = 0; round < 16; ++round) {
        ClearPass pass{directory, depth, false, true};
        if (mdkr_directory_list_utf8(directory, clearEntry, &pass) != 0) return false;
        if (!pass.removed) return pass.okay;
    }
    return false;
}

struct DirectoryEntry {
    std::string name;
    int kind;
};

bool listDirectory(const fs::path &directory, std::vector<DirectoryEntry> &entries) {
    struct Collect {
        std::vector<DirectoryEntry> *entries;
        bool failed;
    } collect{&entries, false};
    const auto visit = [](const char *name, int kind, void *opaque) -> int {
        auto &state = *static_cast<Collect *>(opaque);
        try {
            state.entries->push_back({name, kind});
        } catch (...) {
            state.failed = true;
            return 1;
        }
        return 0;
    };
    return mdkr_directory_list_utf8(directory.u8string().c_str(), visit, &collect) == 0 &&
           !collect.failed;
}
#endif

// Bound miniz's live requested heap bytes, including our allocation headers.
// This is not an RSS bound (the system allocator may retain freed pages).
struct BudgetZip {
    struct alignas(std::max_align_t) Header { size_t bytes; };
    mz_zip_archive archive{};
    std::uintmax_t limit;
    size_t used = 0;
    bool refused = false;

    explicit BudgetZip(std::uintmax_t bytes) : limit(bytes) {
        archive.m_pAlloc_opaque = this;
        archive.m_pAlloc = allocate;
        archive.m_pRealloc = resize;
        archive.m_pFree = release;
    }
    BudgetZip(const BudgetZip &) = delete;
    BudgetZip &operator=(const BudgetZip &) = delete;
    ~BudgetZip() { if (archive.m_pState) mz_zip_reader_end(&archive); }

    static void release(void *opaque, void *ptr) {
        if (!ptr) return;
        auto &self = *static_cast<BudgetZip *>(opaque);
        auto *header = static_cast<Header *>(ptr) - 1;
        self.used -= header->bytes;
        std::free(header);
    }
    static void *allocate(void *opaque, size_t items, size_t size) {
        return resize(opaque, nullptr, items, size);
    }
    static void *resize(void *opaque, void *ptr, size_t items, size_t size) {
        auto &self = *static_cast<BudgetZip *>(opaque);
        if (!items || !size) { release(opaque, ptr); return nullptr; }
        const size_t maximum = std::numeric_limits<size_t>::max();
        if (items > (maximum - sizeof(Header)) / size) {
            self.refused = true;
            return nullptr;
        }
        const size_t bytes = items * size + sizeof(Header);
        auto *old = ptr ? static_cast<Header *>(ptr) - 1 : nullptr;
        const size_t retained = self.used - (old ? old->bytes : 0);
        if (retained > self.limit || bytes > self.limit - retained ||
            bytes > maximum - retained) {
            self.refused = true;
            return nullptr;
        }
        auto *replacement = static_cast<Header *>(std::realloc(old, bytes));
        if (!replacement) return nullptr; // old allocation remains owned
        replacement->bytes = bytes;
        self.used = retained + bytes;
        return replacement + 1;
    }
};

bool zipExtension(const fs::path &path) {
    // Keep arbitrary user-selected extensions independent of the Windows
    // narrow locale, including unsupported extensions we must reject cleanly.
    std::string ext = path.extension().u8string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".zip";
}

std::string fileError(const char *operation, const std::error_code &error) {
    return std::string(operation) + ": " + error.message();
}

// Hold a ZIP's central directory to the intake budgets without inflating any
// member. Returns an empty string when it fits. `poll`, when set, is called
// before each member and a true answer abandons the check (the caller records
// why). `staged` only chooses the wording of a failed read.
std::string zipDirectoryProblem(FILE *stream, const PackInstallLimits &limits,
                                bool staged, const std::function<bool()> &poll) {
    BudgetZip reader(limits.maxZipMetadataBytes);
    auto &zip = reader.archive;
    if (!stream || std::fseek(stream, 0, SEEK_SET) != 0 ||
        !mz_zip_reader_init_cfile(&zip, stream, 0,
                                  MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY)) {
        if (reader.refused) return "The ZIP exceeds the installation metadata budget.";
        return staged ? "Could not read the staged ZIP directory."
                      : "This is not an installable pack: the ZIP is damaged or is "
                        "not a ZIP archive.";
    }
    const auto members = mz_zip_reader_get_num_files(&zip);
    if (members > limits.maxEntries) return "The ZIP exceeds the installation entry budget.";
    std::uintmax_t expanded = 0;
    for (mz_uint i = 0; i < members; ++i) {
        if (poll && poll()) return {};
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat))
            return staged ? "Could not read a staged ZIP member."
                          : "Could not read a member of the ZIP.";
        if (stat.m_uncomp_size > MDKR_MOD_SOURCE_ENTRY_MAX)
            return "A ZIP member exceeds the supported entry size.";
        if (stat.m_uncomp_size > limits.maxExpandedBytes - expanded)
            return "The ZIP exceeds the installation expanded-byte budget.";
        expanded += stat.m_uncomp_size;
    }
    return {};
}

}  // namespace

PreparedPackInstall::PreparedPackInstall(PreparedPackInstall &&other) noexcept
    : stage_(std::exchange(other.stage_, {})),
      stageName_(std::move(other.stageName_)),
      stagePath_(std::move(other.stagePath_)),
      modsDir_(std::move(other.modsDir_)),
      filename_(std::move(other.filename_)),
      error_(std::move(other.error_))
#if !MDKR_PACK_BY_PATH
      , modsFd_(std::exchange(other.modsFd_, -1)),
      stageFd_(std::exchange(other.stageFd_, -1))
#else
      , hold_(std::exchange(other.hold_, MdkrFileLock{-1}))
#endif
      {}

PreparedPackInstall &PreparedPackInstall::operator=(PreparedPackInstall &&other) noexcept {
    if (this != &other) {
        cleanup();
        stage_ = std::exchange(other.stage_, {});
        stageName_ = std::move(other.stageName_);
        modsDir_ = std::move(other.modsDir_);
        filename_ = std::move(other.filename_);
        error_ = std::move(other.error_);
        stagePath_ = std::move(other.stagePath_);
#if !MDKR_PACK_BY_PATH
        modsFd_ = std::exchange(other.modsFd_, -1);
        stageFd_ = std::exchange(other.stageFd_, -1);
#else
        hold_ = std::exchange(other.hold_, MdkrFileLock{-1});
#endif
    }
    return *this;
}

PreparedPackInstall::~PreparedPackInstall() { cleanup(); }

void PreparedPackInstall::cleanup() noexcept {
#if !MDKR_PACK_BY_PATH
    // On identity loss, preserve both trees. Do not delete a replacement or
    // guess where somebody moved the original owned directory.
    if (!stage_.empty() && sameDirectory(stageFd_, modsFd_, stageName_.c_str())) {
        if (clearDirectory(stageFd_) && sameDirectory(stageFd_, modsFd_, stageName_.c_str()))
            (void)unlinkat(modsFd_, stageName_.c_str(), AT_REMOVEDIR);
    }
    if (stageFd_ >= 0) close(std::exchange(stageFd_, -1));
    if (modsFd_ >= 0) close(std::exchange(modsFd_, -1));
    stage_.clear();
#else
    // Never descend through a link that replaced the staging folder. On
    // Windows the hold refuses that replacement in the first place.
    if (!stage_.empty() &&
        mdkr_path_kind_utf8(stagePath_.c_str()) == MDKR_PATH_DIRECTORY) {
        const bool cleared = clearDirectory(stagePath_.c_str(), 0);
        // Windows refuses to remove a directory this process still holds.
        mdkr_file_lock_release(&hold_);
        if (cleared) (void)removeDirectory(stagePath_.c_str());
    }
    mdkr_file_lock_release(&hold_);
    stage_.clear();
#endif
    stagePath_.clear();
    stageName_.clear();
}

PreparedPackInstall PreparedPackInstall::copyZip(const fs::path &source,
                                                  const fs::path &modsDir,
                                                  const std::function<bool()> &cancelled,
                                                  PackInstallLimits limits) {
    std::error_code ec;
    if (!zipExtension(source) || !fs::is_regular_file(source, ec) || ec) {
        PreparedPackInstall refused;
        refused.error_ = "Choose a readable .zip content pack.";
        return refused;
    }
    return copyPack(source, modsDir, cancelled, limits);
}

PreparedPackInstall PreparedPackInstall::copyPack(const fs::path &source,
                                                   const fs::path &modsDir,
                                                   const std::function<bool()> &cancelled,
                                                   PackInstallLimits limits) {
    PreparedPackInstall prepared;
    const auto stop = [&] {
        if (!cancelled || !cancelled()) return false;
        prepared.error_ = "Pack installation cancelled. Nothing was installed.";
        return true;
    };
    if (stop()) return prepared;
    prepared.filename_ = source.filename();
    if (prepared.filename_.empty() || prepared.filename_ == "." || prepared.filename_ == "..") {
        prepared.error_ = "Choose a named content-pack folder or ZIP.";
        return prepared;
    }

    std::error_code ec;
#if MDKR_PACK_BY_PATH
    // std::filesystem cannot see Windows junctions or symbolic links, so the
    // pathname arm asks fs_utf8, which reports them as links.
    const int sourceKind = mdkr_path_kind_utf8(source.u8string().c_str());
    const bool directory = sourceKind == MDKR_PATH_DIRECTORY;
    if (!directory && (sourceKind != MDKR_PATH_FILE || !zipExtension(source))) {
#else
    const auto sourceStatus = fs::symlink_status(source, ec);
    const bool directory = fs::is_directory(sourceStatus);
    if (ec || (!directory && (!fs::is_regular_file(sourceStatus) || !zipExtension(source)))) {
#endif
        prepared.error_ = "Choose a regular ZIP or folder, not a link or special file.";
        return prepared;
    }
    prepared.modsDir_ = fs::absolute(modsDir, ec);
    if (ec || prepared.modsDir_.empty()) {
        prepared.error_ = fileError("Cannot locate the mods folder", ec);
        return prepared;
    }
    if (directory) {
        const auto sourceReal = fs::canonical(source, ec);
        if (ec) {
            prepared.error_ = fileError("Cannot resolve the pack folder", ec);
            return prepared;
        }
        const auto modsReal = fs::weakly_canonical(prepared.modsDir_, ec);
        if (ec) {
            prepared.error_ = fileError("Cannot resolve the mods folder", ec);
            return prepared;
        }
        auto child = modsReal.begin();
        auto parent = sourceReal.begin();
        while (child != modsReal.end() && parent != sourceReal.end() && *child == *parent) {
            ++child; ++parent;
        }
        if (parent == sourceReal.end()) {
            prepared.error_ = "That folder contains your mods folder. Choose the pack itself.";
            return prepared;
        }
    }
#if !MDKR_PACK_BY_PATH
    // Pin the source before traversal/cancellation callbacks. A pathname check
    // followed by ifstream can follow a replaced parent into unrelated files.
    // A folder is pinned by its descriptor. A ZIP is opened as the one file
    // the player chose, never through its folder: macOS lets an Open-panel
    // pick be read, but may refuse to let this process open the folder it sits
    // in (Downloads, Desktop, Documents).
    std::unique_ptr<MdkrModSource, decltype(&mdkr_mod_source_close)> sourceRoot(
        directory ? mdkr_mod_source_open(source.c_str(), 0) : nullptr,
        mdkr_mod_source_close);
    if (directory && !sourceRoot) {
        prepared.error_ = "Cannot open the selected pack's source folder.";
        return prepared;
    }
    ScopedFd zipFd(directory ? -1 : open(source.c_str(),
        O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC | O_NOCTTY));
    std::uintmax_t zipSize = 0;
    if (!directory) {
        struct stat zipStatus{};
        const int openError = zipFd.value < 0 ? errno : 0;
        if (openError == ELOOP || (zipFd.value >= 0 &&
            (fstat(zipFd.value, &zipStatus) != 0 || !S_ISREG(zipStatus.st_mode)))) {
            prepared.error_ = "Choose a regular ZIP or folder, not a link or special file.";
            return prepared;
        }
        if (openError != 0) {
            prepared.error_ = fileError("Cannot open the selected pack",
                                        std::error_code(openError, std::generic_category()));
            return prepared;
        }
        zipSize = static_cast<std::uintmax_t>(zipStatus.st_size);
    }
    const auto openSource = [&](const fs::path &relative) -> FILE * {
        if (directory) {
            return mdkr_mod_source_open_directory_file(sourceRoot.get(),
                                                       relative.generic_u8string().c_str());
        }
        // A private stream on the pinned ZIP; each reader starts at byte zero.
        const int duplicate = fcntl(zipFd.value, F_DUPFD_CLOEXEC, 0);
        FILE *stream = duplicate >= 0 ? fdopen(duplicate, "rb") : nullptr;
        if (!stream) {
            if (duplicate >= 0) close(duplicate);
            return nullptr;
        }
        if (std::fseek(stream, 0, SEEK_SET) != 0) {
            std::fclose(stream);
            return nullptr;
        }
        return stream;
    };
#else
    // A folder's files are read through the same link-refusing reader the game
    // uses. A ZIP is read as the one file the player chose.
    std::unique_ptr<MdkrModSource, decltype(&mdkr_mod_source_close)> sourceRoot(
        directory ? mdkr_mod_source_open(source.u8string().c_str(), 0) : nullptr,
        mdkr_mod_source_close);
    if (directory && !sourceRoot) {
        prepared.error_ = "Cannot open the selected pack's source folder.";
        return prepared;
    }
    std::uintmax_t zipSize = 0;
    if (!directory) {
        zipSize = fs::file_size(source, ec);
        if (ec) {
            prepared.error_ = fileError("Cannot open the selected pack", ec);
            return prepared;
        }
    }
    const auto openSource = [&](const fs::path &relative) {
        return directory
            ? mdkr_mod_source_open_directory_file(sourceRoot.get(),
                                                  relative.generic_u8string().c_str())
            : mdkr_fopen_utf8(source.u8string().c_str(), "rb");
    };
#endif
    const fs::path destination = prepared.modsDir_ / prepared.filename_;
    if (fs::exists(destination, ec) || ec) {
        prepared.error_ = ec ? fileError("Cannot check for an existing pack", ec)
                             : "A pack with that filename is already installed. "
                               "Nothing was replaced.";
        return prepared;
    }
    // A wrong pick fails here, before anything is copied or created: a ZIP's
    // directory is held to the intake budgets, then the pack to the admission
    // rules the game applies when it loads packs. The staged copy is checked
    // again before publication.
    {
        std::unique_ptr<MdkrModSource, decltype(&mdkr_mod_source_close)> archive(
            nullptr, mdkr_mod_source_close);
        if (!directory) {
            if (zipSize > limits.maxCopyBytes) {
                prepared.error_ = "The pack exceeds the installation byte budget.";
                return prepared;
            }
            std::unique_ptr<FILE, decltype(&std::fclose)> listing(
                openSource(prepared.filename_), std::fclose);
            const auto problem = zipDirectoryProblem(listing.get(), limits, false, {});
            if (!problem.empty()) {
                prepared.error_ = problem;
                return prepared;
            }
            archive.reset(mdkr_mod_source_from_zip_stream(openSource(prepared.filename_)));
        }
        MdkrModManifest manifest{};
        int isRice = 0;
        char reason[128]{};
        if (!mdkr_mod_registry_probe_source(directory ? sourceRoot.get() : archive.get(),
                                            prepared.filename_.u8string().c_str(),
                                            &manifest, &isRice, reason, sizeof reason)) {
            prepared.error_ = std::string("This is not an installable pack: ") + reason;
            return prepared;
        }
    }
    fs::create_directories(prepared.modsDir_, ec);
    if (ec) {
        prepared.error_ = fileError("Cannot create the mods folder", ec);
        return prepared;
    }

#if !MDKR_PACK_BY_PATH
    prepared.modsFd_ = open(prepared.modsDir_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (prepared.modsFd_ < 0) {
        prepared.error_ = "Cannot retain ownership of the mods folder.";
        return prepared;
    }
#endif
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        fs::path candidate = prepared.modsDir_ /
            (".install-" + std::to_string(tick) + "-" + std::to_string(attempt));
        prepared.stageName_ = candidate.filename();
        ec.clear();
#if !MDKR_PACK_BY_PATH
        if (relativeMkdir(prepared.modsFd_, candidate.filename(), ec)) {
            prepared.stage_ = std::move(candidate);
            break;
        }
        if (ec && ec != std::errc::file_exists) {
            prepared.error_ = fileError("Cannot stage the pack", ec);
            return prepared;
        }
#else
        std::string candidatePath = candidate.u8string();
        if (mdkr_mkdir_utf8(candidatePath.c_str()) != 0) {
            if (errno == EEXIST) continue;
            prepared.error_ = fileError("Cannot stage the pack",
                                        std::error_code(errno, std::generic_category()));
            return prepared;
        }
        // Hold the new folder for the whole installation. On Windows that also
        // stops anything renaming or replacing it until cleanup.
        if (mdkr_directory_hold_utf8(candidatePath.c_str(), &prepared.hold_) != 0) {
            const int error = errno;
            // Taken or replaced since it was created: it is not ours to use or
            // remove. Try another name.
            if (error == EBUSY || error == ENOENT || error == ELOOP || error == ENOTDIR)
                continue;
            // A filesystem without advisory locks cannot record the hold; the
            // installation proceeds unheld.
            if (error != ENOTSUP && error != EOPNOTSUPP && error != ENOLCK) {
                prepared.stage_ = std::move(candidate);
                prepared.stagePath_ = std::move(candidatePath);
                prepared.error_ = fileError("Cannot hold the staging folder",
                                            std::error_code(error, std::generic_category()));
                return prepared;
            }
        }
        prepared.stage_ = std::move(candidate);
        prepared.stagePath_ = std::move(candidatePath);
        break;
#endif
    }
    if (prepared.stage_.empty()) {
        prepared.error_ = "Could not find a free staging folder for this pack.";
        return prepared;
    }
#if !MDKR_PACK_BY_PATH
    prepared.stageFd_ = prepared.modsFd_ < 0 ? -1 : openat(prepared.modsFd_,
        prepared.stageName_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (prepared.stageFd_ < 0) {
        prepared.error_ = "Could not retain ownership of the staging directory.";
        return prepared;
    }
    const auto createStaged = [&](const fs::path &relative) {
        return relativeFile(prepared.stageFd_, relative, true);
    };
    const auto openStaged = [&](const fs::path &relative) {
        return relativeFile(prepared.stageFd_, relative, false);
    };
    const auto makeStagedDirectory = [&](const fs::path &relative, std::error_code &error) {
        return relativeMkdir(prepared.stageFd_, relative, error);
    };
#else
    // Exclusive creation: CREATE_NEW on Windows, O_CREAT|O_EXCL elsewhere, so
    // nothing already at a name -- a link included -- is ever written through.
    const auto createStaged = [&](const fs::path &relative) {
        return mdkr_fopen_utf8((prepared.stage_ / relative).u8string().c_str(), "wbx");
    };
    const auto openStaged = [&](const fs::path &relative) {
        return mdkr_fopen_utf8((prepared.stage_ / relative).u8string().c_str(), "rb");
    };
    const auto makeStagedDirectory = [&](const fs::path &relative, std::error_code &error) {
        if (mdkr_mkdir_utf8((prepared.stage_ / relative).u8string().c_str()) == 0) return true;
        error = std::error_code(errno, std::generic_category());
        return false;
    };
#endif
    // Bounded chunks allow cancellation between filesystem operations. The
    // prepared object's owner removes partial staging; never expose it in mods.
    std::uintmax_t totalCopied = 0;
    std::uintmax_t entries = 0;
    const auto takeEntry = [&] {
        if (entries >= limits.maxEntries) {
            prepared.error_ = "The pack exceeds the installation entry budget.";
            return false;
        }
        ++entries;
        return true;
    };
    // `relative` names the file below the selected folder (a ZIP: its own
    // name); the staged copy lands at the same place below the payload.
    const auto copyFile = [&](const fs::path &relative) {
        std::unique_ptr<FILE, decltype(&std::fclose)> input(openSource(relative), std::fclose);
        if (input && directory) {
            // Same per-entry limit as runtime directory reads. Inspect before
            // copying, then enforce again while streaming if the file grows.
            const int seek = std::fseek(input.get(), 0, SEEK_END);
            const long length = seek == 0 ? std::ftell(input.get()) : -1;
            if (length < 0 || static_cast<unsigned long long>(length) > MDKR_MOD_SOURCE_ENTRY_MAX) {
                prepared.error_ = "A pack file exceeds the supported entry size or cannot be measured.";
                return false;
            }
            if (static_cast<std::uintmax_t>(length) > limits.maxCopyBytes - totalCopied) {
                prepared.error_ = "The pack exceeds the installation byte budget.";
                return false;
            }
            if (std::fseek(input.get(), 0, SEEK_SET) != 0) {
                prepared.error_ = "Could not rewind the selected pack file.";
                return false;
            }
        }
        const fs::path staged = directory ? prepared.filename_ / relative : prepared.filename_;
        std::unique_ptr<FILE, decltype(&std::fclose)> output(
            input ? createStaged(staged) : nullptr, std::fclose);
        if (!input || !output) {
            prepared.error_ = "Could not open the pack or staging file for copying.";
            return false;
        }
        std::array<char, 256 * 1024> bytes;
        size_t copied = 0;
        for (;;) {
            if (stop()) return false;
            const size_t count = std::fread(bytes.data(), 1, bytes.size(), input.get());
            if (count == 0) break;
            // Subtraction avoids overflow, including caller-supplied budgets.
            // Check again while streaming to cover files growing after sizing.
            if (count > limits.maxCopyBytes - totalCopied) {
                prepared.error_ = "The pack exceeds the installation byte budget.";
                return false;
            }
            if (directory && count > MDKR_MOD_SOURCE_ENTRY_MAX - copied) {
                prepared.error_ = "A pack file grew beyond the supported entry size.";
                return false;
            }
            if (directory) copied += count;
            totalCopied += count;
            if (std::fwrite(bytes.data(), 1, count, output.get()) != count) {
                prepared.error_ = "Could not write the staged pack. Check available storage.";
                return false;
            }
        }
        if (std::ferror(input.get())) {
            prepared.error_ = "Could not read the complete pack.";
            return false;
        }
        if (std::fclose(output.release()) != 0)
            prepared.error_ = "Could not finish writing the staged pack.";
        return !stop() && prepared.error_.empty();
    };
    if (!directory) {
        if (!takeEntry() || !copyFile(prepared.filename_)) return prepared;
        // Inspect the completed staged bytes on the worker, not the mutable
        // source or the launcher thread. No payload inflation is needed.
        std::unique_ptr<FILE, decltype(&std::fclose)> staged(
            openStaged(prepared.filename_), std::fclose);
        const auto problem = zipDirectoryProblem(staged.get(), limits, true, stop);
        if (!problem.empty() && prepared.error_.empty()) prepared.error_ = problem;
        return prepared;
    }
    makeStagedDirectory(prepared.filename_, ec);
    if (ec) {
        prepared.error_ = fileError("Cannot stage the folder", ec);
        return prepared;
    }
    const char *const unsafeFolder =
        "Pack folders must contain only safely named regular files and folders (no links).";
#if !MDKR_PACK_BY_PATH
    fs::recursive_directory_iterator it(source, fs::directory_options::none, ec), end;
    while (!ec && it != end) {
        if (stop()) return prepared;
        if (!takeEntry()) return prepared;
        const auto status = it->symlink_status(ec);
        if (ec) break;
        const auto relative = it->path().lexically_relative(source);
        const auto name = relative.generic_u8string();
        if (!mdkr_mod_source_path_is_safe(name.c_str()) ||
            it.depth() > MDKR_MOD_SOURCE_WALK_DEPTH_MAX ||
            (!fs::is_directory(status) && !fs::is_regular_file(status))) {
            prepared.error_ = unsafeFolder;
            return prepared;
        }
        if (fs::is_directory(status)) makeStagedDirectory(prepared.filename_ / relative, ec);
        else if (!copyFile(relative)) return prepared;
        if (!ec) it.increment(ec);
    }
    if (ec) prepared.error_ = fileError("Could not copy the complete pack folder", ec);
#else
    // Depth-first, listing each folder by pathname. The listing reports links
    // and junctions as links, and a link anywhere refuses the pack.
    std::function<bool(const fs::path &, int)> copyTree =
        [&](const fs::path &relative, int depth) -> bool {
            std::vector<DirectoryEntry> listed;
            if (!listDirectory(relative.empty() ? source : source / relative, listed)) {
                prepared.error_ = "Could not read the complete pack folder.";
                return false;
            }
            for (const auto &entry : listed) {
                if (stop()) return false;
                if (!takeEntry()) return false;
                const fs::path child = relative / fs::u8path(entry.name);
                if (!mdkr_mod_source_path_is_safe(child.generic_u8string().c_str()) ||
                    depth > MDKR_MOD_SOURCE_WALK_DEPTH_MAX ||
                    (entry.kind != MDKR_PATH_DIRECTORY && entry.kind != MDKR_PATH_FILE)) {
                    prepared.error_ = unsafeFolder;
                    return false;
                }
                if (entry.kind == MDKR_PATH_DIRECTORY) {
                    if (!makeStagedDirectory(prepared.filename_ / child, ec)) {
                        prepared.error_ = fileError("Could not copy the complete pack folder", ec);
                        return false;
                    }
                    if (!copyTree(child, depth + 1)) return false;
                } else if (!copyFile(child)) {
                    return false;
                }
            }
            return true;
        };
    (void)copyTree(fs::path(), 0);
#endif
    return prepared;
}

PackInstallResult PreparedPackInstall::finalize(const std::function<bool()> &cancelled) {
    PackInstallResult result;
    const auto stop = [&] {
        if (!cancelled || !cancelled()) return false;
        result.message = "Pack installation cancelled. Nothing was installed.";
        return true;
    };
    if (stop()) return result;
    if (!error_.empty()) {
        result.message = error_;
        return result;
    }
    if (stage_.empty()) {
        result.message = "This pack installation has already finished.";
        return result;
    }

    // Do not follow an already-replaced staging-root link into somebody else's
    // pack during probing or publication. Repeat after the final callback too.
    // These pathname checks do not close concurrent check/rename races; full
    // destination capability ownership remains a separate requirement.
    const auto stageIsDirectory = [&] {
#if !MDKR_PACK_BY_PATH
        // The mods folder may be reached through a link (packs moved to
        // another drive); it must still name the folder pinned at intake.
        if (sameDirectory(stageFd_, modsFd_, stage_.filename().c_str()) &&
            sameDirectory(modsFd_, AT_FDCWD, modsDir_.c_str(), true)) return true;
#else
        // On Windows the hold refuses any rename or replacement of the stage;
        // elsewhere this at least refuses a link swapped into its place.
        if (mdkr_path_kind_utf8(stagePath_.c_str()) == MDKR_PATH_DIRECTORY) return true;
#endif
        result.message = "The staging folder changed or is unavailable. Nothing was installed.";
        return false;
    };
    if (!stageIsDirectory()) return result;

    MdkrModSource *opened = nullptr;
#if !MDKR_PACK_BY_PATH
    ScopedFd payloadFd(openat(stageFd_, filename_.c_str(),
                             O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    struct stat payloadIdentity{};
    if (payloadFd.value >= 0 && fstat(payloadFd.value, &payloadIdentity) == 0) {
        if (S_ISDIR(payloadIdentity.st_mode)) {
            opened = mdkr_mod_source_from_directory_fd(payloadFd.value);
        } else if (S_ISREG(payloadIdentity.st_mode)) {
            int duplicate = fcntl(payloadFd.value, F_DUPFD_CLOEXEC, 0);
            if (duplicate >= 0) {
                FILE *stream = fdopen(duplicate, "rb");
                if (!stream) close(duplicate);
                opened = mdkr_mod_source_from_zip_stream(stream);
            }
        }
    }
#else
    // Admission reads the staged copy through the same reader the game uses.
    const std::string payloadPath = (stage_ / filename_).u8string();
    const int payloadKind = mdkr_path_kind_utf8(payloadPath.c_str());
    if (payloadKind == MDKR_PATH_DIRECTORY || payloadKind == MDKR_PATH_FILE) {
        opened = mdkr_mod_source_open(payloadPath.c_str(), payloadKind == MDKR_PATH_FILE);
    }
#endif
    std::unique_ptr<MdkrModSource, decltype(&mdkr_mod_source_close)> source(
        opened, mdkr_mod_source_close);
    MdkrModManifest manifest{};
    int isRice = 0;
    char reason[128]{};
    if (!mdkr_mod_registry_probe_source(source.get(), filename_.u8string().c_str(),
                                        &manifest, &isRice, reason, sizeof reason)) {
        result.message = std::string("This is not an installable pack: ") + reason;
        return result;
    }
    // Release validator readers before publication: Windows cannot rename a
    // folder while a file inside it is open.
    source.reset();

    // Last cancellation boundary before the atomic publication point. Once
    // that move succeeds the operation is committed; never delete it afterward
    // in response to a late token that could race with another user's changes.
    if (stop()) return result;
    if (!stageIsDirectory()) return result;
#if !MDKR_PACK_BY_PATH
    struct stat publicationIdentity{};
    if (fstatat(stageFd_, filename_.c_str(), &publicationIdentity, AT_SYMLINK_NOFOLLOW) ||
        publicationIdentity.st_dev != payloadIdentity.st_dev ||
        publicationIdentity.st_ino != payloadIdentity.st_ino) {
        // Preserve the replacement and displaced original. Losing the payload
        // name revokes permission to recursively reclaim this staging tree.
        close(std::exchange(stageFd_, -1));
        error_ = "The staged payload changed during validation. Nothing was installed.";
        result.message = error_;
        return result;
    }
#endif
    result.destination = modsDir_ / filename_;
    // Prepare allocating success data before publication. After a successful
    // move, neither reporting nor cleanup may throw and misreport failure.
    result.message = "Pack installed. Its contents will load when you start the game.";
    std::error_code ec;
    // Staging and destination share a filesystem. Native exclusive moves
    // refuse an existing file or directory atomically. Plain POSIX rename()
    // would replace an existing destination and is not a safe fallback.
#if MDKR_PACK_BY_PATH
    const std::string destinationPath = result.destination.u8string();
    if (mdkr_rename_exclusive_utf8(payloadPath.c_str(), destinationPath.c_str()) != 0)
        ec = std::error_code(errno, std::generic_category());
#elif defined(__APPLE__)
    if (renameatx_np(stageFd_, filename_.c_str(), modsFd_, filename_.c_str(), RENAME_EXCL) != 0)
        ec = std::error_code(errno, std::generic_category());
#elif defined(__linux__) && defined(SYS_renameat2)
    // No check-then-rename fallback: plain rename can overwrite a competing
    // empty directory. Unsupported kernels/filesystems must refuse safely.
    if (syscall(SYS_renameat2, stageFd_, filename_.c_str(),
                modsFd_, filename_.c_str(), RENAME_NOREPLACE) != 0)
        ec = std::error_code(errno, std::generic_category());
#else
    fs::create_hard_link(stage_ / filename_, result.destination, ec);
#endif
    if (ec) {
        result.destination.clear();
        result.message = ec == std::errc::file_exists
            ? "A pack with that filename was installed meanwhile. Nothing was replaced."
            : fileError("Could not publish the pack", ec);
        return result;
    }
    result.installed = true;
    cleanup();
    return result;
}
