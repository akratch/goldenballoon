#include "app_relaunch.h"
#include "diag_log.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

// Normally supplied by platform/main_pc.c. This focused executable links only
// the app lifecycle seam, so it owns the crash-mirror storage.
int g_diagLogRealErrFd = -1;
int g_diagLogFileFd = -1;

int main() {
    if (!DiagLog_install()) {
        std::fprintf(stderr, "FAIL: diagnostic tee did not install\n");
        return 1;
    }
    const std::string logPath = DiagLog_path();
    std::string payload(160 * 1024, 'D');  // >2x the Windows pipe capacity
    std::fputs("TEE-BEGIN\n", stdout);
    std::fwrite(payload.data(), 1, payload.size(), stdout);
    std::fputs("\nTEE-MIDDLE\n", stderr);
    std::fwrite(payload.data(), 1, payload.size(), stderr);
    std::fputs("\nTEE-END\n", stderr);
    DiagLog_shutdown();

    // A stale pipe would SIGPIPE or block here; restored stdout remains live.
    std::fputs("TEE-RESTORED\n", stdout);
    std::fflush(stdout);

    std::ifstream input(logPath, std::ios::binary);
    const std::string logged((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
    if (logged.size() < payload.size() * 2 ||
        logged.find("TEE-BEGIN") == std::string::npos ||
        logged.find("TEE-MIDDLE") == std::string::npos ||
        logged.find("TEE-END") == std::string::npos) {
        std::fprintf(stderr, "FAIL: joined tee did not drain the complete payload\n");
        return 1;
    }
    if (g_diagLogRealErrFd != -1 || g_diagLogFileFd != -1) {
        std::fprintf(stderr, "FAIL: crash mirror descriptors survived shutdown\n");
        return 1;
    }

    const int relaunchError =
        AppRelaunch_replace("/definitely/not/a/real/mdkr64-executable");
    if (relaunchError == 0) {
        std::fprintf(stderr, "FAIL: missing relaunch target reported success\n");
        return 1;
    }

    // Idempotence and repeated launcher lifecycles: a second install/shutdown
    // must also drain and restore without inheriting the first pipe.
    if (!DiagLog_install()) {
        std::fprintf(stderr, "FAIL: diagnostic tee did not reinstall\n");
        return 1;
    }
    std::fputs("TEE-SECOND-LIFECYCLE\n", stderr);
    DiagLog_shutdown();
    std::puts("app lifecycle passed: tee drained/restored twice; relaunch failure returned");
    return 0;
}
