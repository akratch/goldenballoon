#include "app_relaunch.h"
#include "app_restart.h"
#include "diag_log.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

// Normally supplied by platform/main_pc.c. This focused executable links only
// the app lifecycle seam, so it owns the crash-mirror storage.
int g_diagLogRealErrFd = -1;
int g_diagLogFileFd = -1;

int main() {
    const char *prefs = std::getenv("MDKR_APP_PREFS_DIR");
    if (!prefs || !prefs[0]) {
        std::fprintf(stderr, "FAIL: lifecycle test needs isolated prefs\n");
        return 1;
    }
    const std::string separator =
        (prefs[std::char_traits<char>::length(prefs) - 1] == '/' ||
         prefs[std::char_traits<char>::length(prefs) - 1] == '\\') ? "" : "/";

    std::string restartRom;
    const std::string restartPath =
        "/tmp/validated DKR "
        "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E "
        "\xF0\x9F\x8E\x88.z64";
    if (!AppRestart_stageGame(restartPath.c_str()) ||
        !AppRestart_consumeGame(restartRom) ||
        restartRom != restartPath ||
        std::getenv("MDKR_APP_RESTART_GAME") != nullptr ||
        std::getenv("MDKR_APP_AUTOPLAY") != nullptr ||
        std::getenv("MDKR_ROM") != nullptr) {
        std::fprintf(stderr, "FAIL: restart handoff was not one-shot and lossless\n");
        return 1;
    }
    if (AppRestart_stageGame("") || AppRestart_consumeGame(restartRom)) {
        std::fprintf(stderr, "FAIL: restart handoff accepted an empty ROM\n");
        return 1;
    }
#if defined(_WIN32)
    _putenv_s("MDKR_APP_TEST_RESTART_STAGE_FAILURE", "1");
#else
    setenv("MDKR_APP_TEST_RESTART_STAGE_FAILURE", "1", 1);
#endif
    if (AppRestart_stageGame("/tmp/forced-stage-failure.z64") ||
        std::getenv("MDKR_APP_RESTART_GAME") != nullptr ||
        std::getenv("MDKR_APP_AUTOPLAY") != nullptr ||
        std::getenv("MDKR_ROM") != nullptr) {
        std::fprintf(stderr,
                     "FAIL: forced restart staging failure left a one-shot marker\n");
        return 1;
    }
#if defined(_WIN32)
    _putenv_s("MDKR_APP_TEST_RESTART_STAGE_FAILURE", "");
#else
    unsetenv("MDKR_APP_TEST_RESTART_STAGE_FAILURE");
#endif
    if (!AppRestart_stageGame("/tmp/will-be-cleared.z64")) {
        std::fprintf(stderr, "FAIL: restart handoff could not be staged for clear test\n");
        return 1;
    }
    AppRestart_clear();
    if (std::getenv("MDKR_APP_RESTART_GAME") != nullptr ||
        std::getenv("MDKR_APP_AUTOPLAY") != nullptr ||
        std::getenv("MDKR_ROM") != nullptr) {
        std::fprintf(stderr, "FAIL: restart handoff clear left autoplay state\n");
        return 1;
    }
    const std::string seededLog =
        std::string(prefs) + separator + "mdkr64.log";
    {
        std::ofstream seed(seededLog, std::ios::binary | std::ios::trunc);
        seed << "PREVIOUS-ENGINE-FAILURE-MARKER\n";
    }
#if defined(_WIN32)
    _putenv_s("MDKR_APP_BOOT_RECOVERY", "test recovery");
#else
    setenv("MDKR_APP_BOOT_RECOVERY", "test recovery", 1);
#endif
    if (!DiagLog_install()) {
        std::fprintf(stderr, "FAIL: diagnostic tee did not install\n");
        return 1;
    }
    char recoverySnapshot[4096];
    const int recoveryBytes =
        DiagLog_snapshot(recoverySnapshot, sizeof(recoverySnapshot));
    if (!DiagLog_includesPreviousFailure() || recoveryBytes <= 0 ||
        std::string(recoverySnapshot).find("PREVIOUS-ENGINE-FAILURE-MARKER") ==
            std::string::npos) {
        std::fprintf(stderr,
                     "FAIL: recovery relaunch did not preserve prior diagnostics\n");
        return 1;
    }
#if defined(_WIN32)
    _putenv_s("MDKR_APP_BOOT_RECOVERY", "");
#else
    unsetenv("MDKR_APP_BOOT_RECOVERY");
#endif
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
