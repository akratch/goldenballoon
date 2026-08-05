// diag_log.cpp — see diag_log.h.
#include "diag_log.h"
#include "engine_entry.h"   // crash-write mirror descriptors
#include "fs_utf8.h"

#include <SDL.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

std::mutex  g_ringMutex;
std::string g_ring;
constexpr size_t kRingCap = 256 * 1024;
std::string g_logPath;
std::FILE  *g_logFile = nullptr;
std::thread g_reader;
int         g_realOut = -1;
int         g_realErr = -1;
bool        g_installed = false;
bool        g_includesPreviousFailure = false;

void appendRing(const char *data, size_t n) {
    std::lock_guard<std::mutex> lock(g_ringMutex);
    g_ring.append(data, n);
    if (g_ring.size() > kRingCap) {
        g_ring.erase(0, g_ring.size() - kRingCap);
    }
}

std::string prefsDir() {
    const char *overrideDir = std::getenv("MDKR_APP_PREFS_DIR");
    if (overrideDir && overrideDir[0]) {
        std::string result = overrideDir;
        if (result.back() != '/' && result.back() != '\\') result += '/';
        return result;
    }
    char *path = SDL_GetPrefPath("mdkr64", "mdkr64");
    std::string result = path ? path : "";
    if (path) SDL_free(path);
    return result;
}

#if defined(_WIN32)

int duplicateFd(int fd) { return _dup(fd); }
int replaceFd(int source, int target) { return _dup2(source, target); }
void closeFd(int fd) { if (fd >= 0) _close(fd); }
int fileFd(std::FILE *file) { return file ? _fileno(file) : -1; }

void writeAll(int fd, const char *data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        const int written =
            _write(fd, data + offset, static_cast<unsigned>(size - offset));
        if (written > 0) {
            offset += static_cast<size_t>(written);
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

void readerLoop(int readFd) {
    char buffer[4096];
    int count = 0;
    while ((count = _read(readFd, buffer, sizeof(buffer))) > 0) {
        appendRing(buffer, static_cast<size_t>(count));
        if (g_logFile) {
            std::fwrite(buffer, 1, static_cast<size_t>(count), g_logFile);
            std::fflush(g_logFile);
        }
        if (g_realErr >= 0) {
            writeAll(g_realErr, buffer, static_cast<size_t>(count));
        }
    }
    closeFd(readFd);
}

bool createPipe(int pipeFds[2]) {
    return _pipe(pipeFds, 64 * 1024, _O_BINARY | _O_NOINHERIT) == 0;
}

#else

int duplicateFd(int fd) {
    const int copy = dup(fd);
    if (copy >= 0) (void)fcntl(copy, F_SETFD, FD_CLOEXEC);
    return copy;
}
int replaceFd(int source, int target) { return dup2(source, target); }
void closeFd(int fd) { if (fd >= 0) close(fd); }
int fileFd(std::FILE *file) { return file ? fileno(file) : -1; }

void writeAll(int fd, const char *data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        const ssize_t written = write(fd, data + offset, size - offset);
        if (written > 0) {
            offset += static_cast<size_t>(written);
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

void readerLoop(int readFd) {
    char buffer[4096];
    ssize_t count = 0;
    while ((count = read(readFd, buffer, sizeof(buffer))) > 0) {
        appendRing(buffer, static_cast<size_t>(count));
        if (g_logFile) {
            std::fwrite(buffer, 1, static_cast<size_t>(count), g_logFile);
            std::fflush(g_logFile);
        }
        if (g_realErr >= 0) {
            writeAll(g_realErr, buffer, static_cast<size_t>(count));
        }
    }
    closeFd(readFd);
}

bool createPipe(int pipeFds[2]) {
    if (pipe(pipeFds) != 0) return false;
    (void)fcntl(pipeFds[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(pipeFds[1], F_SETFD, FD_CLOEXEC);
    return true;
}

#endif

void closeSavedDescriptors() {
    closeFd(g_realOut);
    closeFd(g_realErr);
    g_realOut = -1;
    g_realErr = -1;
}

void closeLogFile() {
    if (!g_logFile) return;
    std::fflush(g_logFile);
    std::fclose(g_logFile);
    g_logFile = nullptr;
}

void importPreviousFailure(const std::string &path) {
    const char *recovery = std::getenv("MDKR_APP_BOOT_RECOVERY");
    if (recovery == nullptr || recovery[0] == '\0') return;
    std::FILE *file = mdkr_fopen_utf8(path.c_str(), "rb");
    if (!file) return;
    static const char heading[] =
        "\n--- previous engine attempt (recovered safely) ---\n";
    appendRing(heading, sizeof(heading) - 1u);
    char bytes[4096];
    size_t count;
    while ((count = std::fread(bytes, 1, sizeof(bytes), file)) != 0u) {
        appendRing(bytes, count);
    }
    std::fclose(file);
    g_includesPreviousFailure = true;
}

}  // namespace

bool DiagLog_install() {
    if (g_installed) return true;

    {
        std::lock_guard<std::mutex> lock(g_ringMutex);
        g_ring.clear();
    }
    g_includesPreviousFailure = false;

    const std::string directory = prefsDir();
    g_logPath = directory.empty() ? "" : directory + "mdkr64.log";
    if (!g_logPath.empty()) {
        const std::string previous = directory + "mdkr64.prev.log";
        // Replacement is a single operation in the UTF-8 filesystem layer.
        // Do not unlink the prior log first: if rotation itself fails, the
        // current failed-engine log is still the best recovery evidence and
        // must remain available for Diagnostics to import.
        const int rotated =
            mdkr_move_utf8(g_logPath.c_str(), previous.c_str(), 1, 0);
        importPreviousFailure(rotated == 0 ? previous : g_logPath);
        /* Binary: the tee copies pipe bytes through verbatim, and the Windows
         * text-mode newline translation would make the file disagree with both
         * the in-app ring and the "rb" import above. */
        g_logFile = mdkr_fopen_utf8(g_logPath.c_str(), "wb");
    }

    g_realOut = duplicateFd(1);
    g_realErr = duplicateFd(2);
    int pipeFds[2] = {-1, -1};
    if (g_realOut < 0 || g_realErr < 0 || !createPipe(pipeFds)) {
        closeSavedDescriptors();
        closeFd(pipeFds[0]);
        closeFd(pipeFds[1]);
        closeLogFile();
        return false;
    }

    std::fflush(stdout);
    std::fflush(stderr);
    if (replaceFd(pipeFds[1], 1) < 0 || replaceFd(pipeFds[1], 2) < 0) {
        // Restore both descriptors even when only the first dup succeeded.
        (void)replaceFd(g_realOut, 1);
        (void)replaceFd(g_realErr, 2);
        closeFd(pipeFds[0]);
        closeFd(pipeFds[1]);
        closeSavedDescriptors();
        closeLogFile();
        return false;
    }
    closeFd(pipeFds[1]);

    // A pipe is block-buffered by default. Prompt flushing is important both
    // for the in-app diagnostics view and for orderly relaunch.
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    try {
        g_reader = std::thread(readerLoop, pipeFds[0]);
    } catch (...) {
        (void)replaceFd(g_realOut, 1);
        (void)replaceFd(g_realErr, 2);
        closeFd(pipeFds[0]);
        closeSavedDescriptors();
        closeLogFile();
        return false;
    }

    g_diagLogRealErrFd = g_realErr;
    g_diagLogFileFd = fileFd(g_logFile);
    g_installed = true;
    return true;
}

void DiagLog_shutdown() {
    if (!g_installed) return;

    std::fflush(stdout);
    std::fflush(stderr);

    // Stop asynchronous crash writes before either target descriptor changes.
    g_diagLogRealErrFd = -1;
    g_diagLogFileFd = -1;

    // Restoring fd 1 and 2 closes the pipe's final write references. The reader
    // drains every queued byte, observes EOF, and can then be joined safely.
    (void)replaceFd(g_realOut, 1);
    (void)replaceFd(g_realErr, 2);
    if (g_reader.joinable()) g_reader.join();

    closeLogFile();
    closeSavedDescriptors();
    g_installed = false;
}

int DiagLog_snapshot(char *buffer, int capacity) {
    if (!buffer || capacity <= 0) return 0;
    std::lock_guard<std::mutex> lock(g_ringMutex);
    int count = static_cast<int>(g_ring.size());
    const char *source = g_ring.c_str();
    if (count >= capacity) {
        source += count - (capacity - 1);
        count = capacity - 1;
    }
    std::memcpy(buffer, source, static_cast<size_t>(count));
    buffer[count] = '\0';
    return count;
}

bool DiagLog_includesPreviousFailure() {
    return g_includesPreviousFailure;
}

const char *DiagLog_path() { return g_logPath.c_str(); }
