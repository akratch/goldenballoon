// diag_log.cpp — see diag_log.h.
#include "diag_log.h"
#include "engine_entry.h"   // g_diagLogRealErrFd / g_diagLogFileFd (crash-write mirror)

#include <SDL.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace {
std::mutex  g_mtx;
std::string g_ring;
const size_t kRingCap = 256 * 1024;
std::string g_logPath;
std::FILE  *g_logFile = nullptr;
bool        g_installed = false;

void appendRing(const char *data, size_t n) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_ring.append(data, n);
    if (g_ring.size() > kRingCap) g_ring.erase(0, g_ring.size() - kRingCap);
}

std::string prefsDir() {
    char *p = SDL_GetPrefPath("mdkr64", "mdkr64");
    std::string d = p ? p : "";
    if (p) SDL_free(p);
    return d;
}
}  // namespace

#if defined(_WIN32)

#include <fcntl.h>
#include <io.h>

#include "engine_entry.h"  // g_diagLogRealErrFd/g_diagLogFileFd crash-write mirror

namespace {
int g_realErr = -1;

void readerLoop(int rfd) {
    char buf[4096];
    int n;
    while ((n = _read(rfd, buf, sizeof(buf))) > 0) {
        appendRing(buf, (size_t)n);
        if (g_logFile) {
            std::fwrite(buf, 1, (size_t)n, g_logFile);
            std::fflush(g_logFile);
        }
        if (g_realErr >= 0) {
            int w = _write(g_realErr, buf, (unsigned)n);  // write through to console
            (void)w;
        }
    }
}
}  // namespace

void DiagLog_install() {
    if (g_installed) return;
    g_installed = true;

    std::string dir = prefsDir();
    g_logPath = dir + "mdkr64.log";
    std::string prev = dir + "mdkr64.prev.log";
    std::remove(prev.c_str());                     // Windows rename() won't replace an
    std::rename(g_logPath.c_str(), prev.c_str());  // existing file, so clear it first
    g_logFile = std::fopen(g_logPath.c_str(), "w");

    g_realErr = _dup(2);  // keep the real console for write-through
    int pfd[2];
    if (_pipe(pfd, 64 * 1024, _O_BINARY | _O_NOINHERIT) != 0) return;
    _dup2(pfd[1], 1);  // stdout -> pipe (CRT-level; covers printf/fprintf)
    _dup2(pfd[1], 2);  // stderr -> pipe
    _close(pfd[1]);
    // Unbuffered, not line-buffered: the Windows CRT treats _IOLBF as full
    // buffering, which would hold diagnostics back from the pipe until the
    // buffer fills (or the process exits).
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    // Publish the crash-write mirror fds only once the tee is live (on the
    // failure return above, fd 2 still points at the real console, where the
    // engine's default crash write already lands).
    g_diagLogRealErrFd = g_realErr;
    g_diagLogFileFd = g_logFile ? _fileno(g_logFile) : -1;
    std::thread(readerLoop, pfd[0]).detach();
}

#else

#include <unistd.h>

namespace {
int g_realErr = -1;

void readerLoop(int rfd) {
    char buf[4096];
    ssize_t n;
    while ((n = read(rfd, buf, sizeof(buf))) > 0) {
        appendRing(buf, (size_t)n);
        if (g_logFile) {
            std::fwrite(buf, 1, (size_t)n, g_logFile);
            std::fflush(g_logFile);
        }
        if (g_realErr >= 0) {
            ssize_t w = write(g_realErr, buf, (size_t)n);  // write through to console
            (void)w;
        }
    }
}
}  // namespace

void DiagLog_install() {
    if (g_installed) return;
    g_installed = true;

    std::string dir = prefsDir();
    g_logPath = dir + "mdkr64.log";
    std::string prev = dir + "mdkr64.prev.log";
    std::rename(g_logPath.c_str(), prev.c_str());  // rotate previous run (ok if absent)
    g_logFile = std::fopen(g_logPath.c_str(), "w");

    g_realErr = dup(2);  // keep the real console for write-through
    int pfd[2];
    if (pipe(pfd) != 0) return;
    dup2(pfd[1], 1);  // stdout -> pipe
    dup2(pfd[1], 2);  // stderr -> pipe
    close(pfd[1]);
    // Line-buffer stdout so each line reaches the pipe promptly (a pipe is
    // block-buffered by default, which would swallow stdout on a quick exit).
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    std::thread(readerLoop, pfd[0]).detach();
}

#endif  // _WIN32

int DiagLog_snapshot(char *buf, int cap) {
    if (!buf || cap <= 0) return 0;
    std::lock_guard<std::mutex> lk(g_mtx);
    int n = (int)g_ring.size();
    const char *src = g_ring.c_str();
    if (n >= cap) {
        src += (n - (cap - 1));
        n = cap - 1;
    }
    std::memcpy(buf, src, (size_t)n);
    buf[n] = '\0';
    return n;
}

const char *DiagLog_path() { return g_logPath.c_str(); }
