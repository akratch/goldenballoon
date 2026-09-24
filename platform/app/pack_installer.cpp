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

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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

#ifndef _WIN32
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

bool sameDirectory(int fd, int parent, const char *name) {
    struct stat owned{}, named{};
    return fd >= 0 && fstat(fd, &owned) == 0 &&
        fstatat(parent, name, &named, AT_SYMLINK_NOFOLLOW) == 0 &&
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
struct ScopedDirectoryHandle {
    intptr_t value;
    explicit ScopedDirectoryHandle(intptr_t handle) : value(handle) {}
    ScopedDirectoryHandle(const ScopedDirectoryHandle &) = delete;
    ScopedDirectoryHandle &operator=(const ScopedDirectoryHandle &) = delete;
    ~ScopedDirectoryHandle() { mdkr_directory_handle_close(value); }
    intptr_t release() { return std::exchange(value, -1); }
};

intptr_t relativeParent(intptr_t root, const fs::path &path) {
    if (!mdkr_mod_source_path_is_safe(path.generic_u8string().c_str())) {
        errno = EINVAL;
        return -1;
    }
    ScopedDirectoryHandle parent(mdkr_directory_handle_child_utf8(root, ".", 1));
    for (const auto &part : path.parent_path()) {
        if (parent.value == -1) return -1;
        const auto name = part.u8string();
        intptr_t next = mdkr_directory_handle_child_utf8(parent.value, name.c_str(), 1);
        mdkr_directory_handle_close(std::exchange(parent.value, next));
    }
    return parent.release();
}

FILE *relativeFile(intptr_t root, const fs::path &path, bool write) {
    const auto name = path.filename().u8string();
    ScopedDirectoryHandle parent(relativeParent(root, path));
    if (parent.value == -1) return nullptr;
    intptr_t child = write
        ? mdkr_directory_handle_create_utf8(parent.value, name.c_str(), 0)
        : mdkr_directory_handle_child_utf8(parent.value, name.c_str(), 0);
    return write ? mdkr_write_handle_stream(child) : mdkr_read_handle_stream(child);
}

bool relativeMkdir(intptr_t root, const fs::path &path, std::error_code &error) {
    const auto name = path.filename().u8string();
    ScopedDirectoryHandle parent(relativeParent(root, path));
    ScopedDirectoryHandle child(parent.value == -1 ? -1 :
        mdkr_directory_handle_create_utf8(parent.value, name.c_str(), 1));
    if (child.value == -1) {
        error = std::error_code(errno, std::generic_category());
        return false;
    }
    return true;
}

bool clearDirectory(intptr_t handle, unsigned depth = 0) {
    if (depth > MDKR_MOD_SOURCE_WALK_DEPTH_MAX + 2u) return false;
    struct Context { intptr_t handle; unsigned depth; bool okay; } context{handle, depth, true};
    const auto visit = [](const char *name, int directory, void *opaque) -> int {
        auto &state = *static_cast<Context *>(opaque);
        /* The directory listing includes "." and "..". Opening either is
         * rejected, and treating that as a walk failure leaves the stage. */
        if (!name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
        intptr_t child = mdkr_directory_handle_owned_child_utf8(state.handle, name, directory);
        if (child == -1) { state.okay = false; return 0; }
        if ((directory && !clearDirectory(child, state.depth + 1)) || mdkr_handle_mark_delete(child))
            state.okay = false;
        mdkr_directory_handle_close(child);
        return 0;
    };
    return mdkr_directory_handle_visit(handle, visit, &context) == 0 && context.okay;
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

}  // namespace

PreparedPackInstall::PreparedPackInstall(PreparedPackInstall &&other) noexcept
    : stage_(std::exchange(other.stage_, {})),
      stageName_(std::move(other.stageName_)),
      modsDir_(std::move(other.modsDir_)),
      filename_(std::move(other.filename_)),
      error_(std::move(other.error_))
#ifndef _WIN32
      , modsFd_(std::exchange(other.modsFd_, -1)),
      stageFd_(std::exchange(other.stageFd_, -1))
#else
      , modsHandle_(std::exchange(other.modsHandle_, -1)),
      stageHandle_(std::exchange(other.stageHandle_, -1))
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
#ifndef _WIN32
        modsFd_ = std::exchange(other.modsFd_, -1);
        stageFd_ = std::exchange(other.stageFd_, -1);
#else
        modsHandle_ = std::exchange(other.modsHandle_, -1);
        stageHandle_ = std::exchange(other.stageHandle_, -1);
#endif
    }
    return *this;
}

PreparedPackInstall::~PreparedPackInstall() { cleanup(); }

void PreparedPackInstall::cleanup() noexcept {
#ifndef _WIN32
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
    if (stageHandle_ != -1 && clearDirectory(stageHandle_))
        (void)mdkr_handle_mark_delete(stageHandle_);
    mdkr_directory_handle_close(std::exchange(stageHandle_, -1));
    mdkr_directory_handle_close(std::exchange(modsHandle_, -1));
    stage_.clear();
#endif
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
    const auto sourceStatus = fs::symlink_status(source, ec);
    const bool directory = fs::is_directory(sourceStatus);
    if (ec || (!directory && (!fs::is_regular_file(sourceStatus) || !zipExtension(source)))) {
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
    // Pin the source before traversal/cancellation callbacks. A pathname check
    // followed by ifstream can follow a replaced parent into unrelated files.
    const auto readRoot = directory ? source :
        (source.parent_path().empty() ? fs::path(".") : source.parent_path());
    std::unique_ptr<MdkrModSource, decltype(&mdkr_mod_source_close)> sourceRoot(
        mdkr_mod_source_open(readRoot.u8string().c_str(), 0), mdkr_mod_source_close);
    if (!sourceRoot) {
        prepared.error_ = "Cannot open the selected pack's source folder.";
        return prepared;
    }
    fs::create_directories(prepared.modsDir_, ec);
    if (ec) {
        prepared.error_ = fileError("Cannot create the mods folder", ec);
        return prepared;
    }
    const fs::path destination = prepared.modsDir_ / prepared.filename_;
    if (fs::exists(destination, ec) || ec) {
        prepared.error_ = ec ? fileError("Cannot check for an existing pack", ec)
                             : "A pack with that filename is already installed. "
                               "Nothing was replaced.";
        return prepared;
    }

#ifndef _WIN32
    prepared.modsFd_ = open(prepared.modsDir_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (prepared.modsFd_ < 0) {
        prepared.error_ = "Cannot retain ownership of the mods folder.";
        return prepared;
    }
#else
    prepared.modsHandle_ = mdkr_directory_handle_open_utf8(prepared.modsDir_.u8string().c_str());
    if (prepared.modsHandle_ == -1) {
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
#ifdef _WIN32
        prepared.stageHandle_ = mdkr_directory_handle_create_utf8(
            prepared.modsHandle_, candidate.filename().u8string().c_str(), 1);
        if (prepared.stageHandle_ == -1) ec = std::error_code(errno, std::generic_category());
#endif
        if (
#ifndef _WIN32
            relativeMkdir(prepared.modsFd_, candidate.filename(), ec)
#else
            prepared.stageHandle_ != -1
#endif
        ) {
            prepared.stage_ = std::move(candidate);
            break;
        }
        if (ec && ec != std::errc::file_exists) {
            prepared.error_ = fileError("Cannot stage the pack", ec);
            return prepared;
        }
    }
    if (prepared.stage_.empty()) {
        prepared.error_ = "Could not find a free staging folder for this pack.";
        return prepared;
    }
#ifndef _WIN32
    prepared.stageFd_ = prepared.modsFd_ < 0 ? -1 : openat(prepared.modsFd_,
        prepared.stageName_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (prepared.stageFd_ < 0) {
        prepared.error_ = "Could not retain ownership of the staging directory.";
        return prepared;
    }
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
    const auto copyFile = [&](const fs::path &from, const fs::path &to) {
        const auto relative = directory ? from.lexically_relative(source) : prepared.filename_;
        std::unique_ptr<FILE, decltype(&std::fclose)> input(
            mdkr_mod_source_open_directory_file(sourceRoot.get(), relative.generic_u8string().c_str()),
            std::fclose);
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
        std::unique_ptr<FILE, decltype(&std::fclose)> output(
#ifndef _WIN32
            input ? relativeFile(prepared.stageFd_, to.lexically_relative(prepared.stage_), true) : nullptr,
#else
            input ? relativeFile(prepared.stageHandle_, to.lexically_relative(prepared.stage_), true) : nullptr,
#endif
            std::fclose);
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
    const auto target = prepared.stage_ / prepared.filename_;
    if (!directory) {
        if (!takeEntry() || !copyFile(source, target)) return prepared;
        // Inspect the completed staged bytes on the worker, not the mutable
        // source or the launcher thread. No payload inflation is needed.
        std::unique_ptr<FILE, decltype(&std::fclose)> staged(
#ifndef _WIN32
            relativeFile(prepared.stageFd_, prepared.filename_, false), std::fclose);
#else
            relativeFile(prepared.stageHandle_, prepared.filename_, false), std::fclose);
#endif
        BudgetZip reader(limits.maxZipMetadataBytes);
        auto &zip = reader.archive;
        if (!staged || !mz_zip_reader_init_cfile(&zip, staged.get(), 0,
                MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY)) {
            prepared.error_ = reader.refused
                ? "The ZIP exceeds the installation metadata budget."
                : "Could not read the staged ZIP directory.";
            return prepared;
        }
        const auto members = mz_zip_reader_get_num_files(&zip);
        std::uintmax_t expanded = 0;
        if (members > limits.maxEntries)
            prepared.error_ = "The ZIP exceeds the installation entry budget.";
        for (mz_uint i = 0; i < members && prepared.error_.empty(); ++i) {
            if (stop()) break;
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
                prepared.error_ = "Could not read a staged ZIP member.";
                break;
            }
            if (stat.m_uncomp_size > MDKR_MOD_SOURCE_ENTRY_MAX) {
                prepared.error_ = "A ZIP member exceeds the supported entry size.";
                break;
            }
            if (stat.m_uncomp_size > limits.maxExpandedBytes - expanded) {
                prepared.error_ = "The ZIP exceeds the installation expanded-byte budget.";
                break;
            }
            expanded += stat.m_uncomp_size;
        }
        return prepared;
    }
#ifndef _WIN32
    relativeMkdir(prepared.stageFd_, prepared.filename_, ec);
#else
    relativeMkdir(prepared.stageHandle_, prepared.filename_, ec);
#endif
    if (ec) {
        prepared.error_ = fileError("Cannot stage the folder", ec);
        return prepared;
    }
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
            prepared.error_ = "Pack folders must contain only safely named regular files and folders (no links).";
            return prepared;
        }
        if (fs::is_directory(status)) {
#ifndef _WIN32
            relativeMkdir(prepared.stageFd_, prepared.filename_ / relative, ec);
#else
            relativeMkdir(prepared.stageHandle_, prepared.filename_ / relative, ec);
#endif
        }
        else if (!copyFile(it->path(), target / relative)) return prepared;
        if (!ec) it.increment(ec);
    }
    if (ec) prepared.error_ = fileError("Could not copy the complete pack folder", ec);
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
#ifndef _WIN32
        if (sameDirectory(stageFd_, modsFd_, stage_.filename().c_str()) &&
            sameDirectory(modsFd_, AT_FDCWD, modsDir_.c_str())) return true;
#else
        std::error_code error;
        const auto status = fs::symlink_status(stage_, error);
        if (!error && fs::is_directory(status)) return true;
#endif
        result.message = "The staging folder changed or is unavailable. Nothing was installed.";
        return false;
    };
    if (!stageIsDirectory()) return result;

    MdkrModSource *opened = nullptr;
#ifndef _WIN32
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
    const auto payloadName = filename_.u8string();
    ScopedDirectoryHandle payloadHandle(mdkr_directory_handle_owned_child_utf8(
        stageHandle_, payloadName.c_str(), 1));
    if (payloadHandle.value != -1) {
        opened = mdkr_mod_source_from_directory_handle(payloadHandle.value);
    } else {
        payloadHandle.value = mdkr_directory_handle_owned_child_utf8(
            stageHandle_, payloadName.c_str(), 0);
        HANDLE duplicate = INVALID_HANDLE_VALUE;
        if (payloadHandle.value != -1 && DuplicateHandle(GetCurrentProcess(),
                reinterpret_cast<HANDLE>(payloadHandle.value), GetCurrentProcess(),
                &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            opened = mdkr_mod_source_from_zip_stream(
                mdkr_read_handle_stream(reinterpret_cast<intptr_t>(duplicate)));
        }
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
    // Release validator readers before publication. On Windows the separate
    // DELETE-capable payload handle retains exact identity across this boundary.
    source.reset();

    // Last cancellation boundary before the atomic publication point. Once
    // that move succeeds the operation is committed; never delete it afterward
    // in response to a late token that could race with another user's changes.
    if (stop()) return result;
    if (!stageIsDirectory()) return result;
#ifndef _WIN32
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
#ifdef _WIN32
    if (mdkr_handle_rename_child_utf8(payloadHandle.value, modsHandle_, payloadName.c_str())) {
        ec = std::error_code(errno, std::generic_category());
    }
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
