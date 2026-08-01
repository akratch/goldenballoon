#include "app_relaunch.h"

#include <cerrno>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

int AppRelaunch_replace(const char *executablePath) {
    if (!executablePath || !executablePath[0]) return EINVAL;
#if defined(_WIN32)
    const char *arguments[] = {executablePath, nullptr};
    _execv(executablePath, arguments);
#else
    char *arguments[] = {const_cast<char *>(executablePath), nullptr};
    // Preserve PATH lookup for Linux invocations made as plain `mdkr64`; the
    // macOS caller supplies an absolute _NSGetExecutablePath result.
    execvp(executablePath, arguments);
#endif
    return errno ? errno : EIO;
}
