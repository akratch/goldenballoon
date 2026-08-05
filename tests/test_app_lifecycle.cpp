#include "app_relaunch.h"
#include "app_restart.h"
#include "diag_log.h"
#include "fs_utf8.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

#if defined(_WIN32)
#include <io.h>
static int dupFd(int fd) { return _dup(fd); }
static int replaceFd(int source, int target) { return _dup2(source, target); }
static void closeFd(int fd) { if (fd >= 0) _close(fd); }
#else
#include <unistd.h>
static int dupFd(int fd) { return dup(fd); }
static int replaceFd(int source, int target) { return dup2(source, target); }
static void closeFd(int fd) { if (fd >= 0) close(fd); }
#endif

// Normally supplied by platform/main_pc.c. This focused executable links only
// the app lifecycle seam, so it owns the crash-mirror storage.
int g_diagLogRealErrFd = -1;
int g_diagLogFileFd = -1;

// The Windows CRT's _exec/_spawn family joins argv into one command line and
// quotes nothing, so every element the relaunch passes has to arrive already
// quoted. These are the cases that decide whether a replacement started from
// an ordinary install path is one argument or several.
static int checkWindowsQuoting() {
    static const struct {
        const char *argument;
        const char *expected;
    } cases[] = {
        {"--ui", "\"--ui\""},
        {"C:\\Golden Balloon\\GoldenBalloon.exe",
         "\"C:\\Golden Balloon\\GoldenBalloon.exe\""},
        {"C:\\Program Files\\Golden Balloon\\GoldenBalloon.exe",
         "\"C:\\Program Files\\Golden Balloon\\GoldenBalloon.exe\""},
        // A trailing backslash precedes the closing quote, so it must double
        // or the parser eats the quote and swallows the next argument.
        {"C:\\dir\\", "\"C:\\dir\\\\\""},
        {"a\"b", "\"a\\\"b\""},
        {"a\\\\\"b", "\"a\\\\\\\\\\\"b\""},
        {"", "\"\""},
    };
    for (const auto &item : cases) {
        char quoted[512];
        const int written = mdkr_windows_quote_argument_utf8(
            item.argument, quoted, sizeof(quoted));
        if (written < 0 || std::string(quoted) != item.expected) {
            std::fprintf(stderr,
                         "FAIL: quoting %s produced %s, expected %s\n",
                         item.argument, written < 0 ? "(error)" : quoted,
                         item.expected);
            return 1;
        }
    }
    char tight[4];
    if (mdkr_windows_quote_argument_utf8("abc", tight, sizeof(tight)) >= 0 ||
        mdkr_windows_quote_argument_utf8(nullptr, tight, sizeof(tight)) >= 0) {
        std::fprintf(stderr, "FAIL: quoting accepted an impossible request\n");
        return 1;
    }
    return 0;
}

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

    if (checkWindowsQuoting() != 0) return 1;

    // The shipped Windows executable links the GUI subsystem (-mwindows), and
    // such a process started from Explorer has NO standard handles: fd 1 and 2
    // are not open. The tee used to abort in exactly that state, but only
    // AFTER rotating and truncating mdkr64.log -- so every shipped Windows
    // launch produced an empty log and destroyed the previous one, while CI,
    // which always supplies real pipes, saw a healthy log.
    //
    // Closing fd 1 and 2 reproduces that condition portably: dup() of a closed
    // descriptor fails identically on Windows and POSIX, so this arm guards
    // the defect on the Windows ctest lane and here.
    {
        const int savedOut = dupFd(1);
        const int savedErr = dupFd(2);
        if (savedOut < 0 || savedErr < 0) {
            std::fprintf(stderr, "FAIL: could not save the standard streams\n");
            return 1;
        }
        closeFd(1);
        closeFd(2);
        const bool installed = DiagLog_install();
        if (installed) std::fputs("HEADLESS-TEE-MARKER\n", stderr);
        DiagLog_shutdown();
        (void)replaceFd(savedOut, 1);
        (void)replaceFd(savedErr, 2);
        closeFd(savedOut);
        closeFd(savedErr);
        if (!installed) {
            std::fprintf(stderr,
                         "FAIL: tee refused to install without standard streams\n");
            return 1;
        }
        std::ifstream headless(logPath, std::ios::binary);
        const std::string headlessLog(
            (std::istreambuf_iterator<char>(headless)),
            std::istreambuf_iterator<char>());
        if (headlessLog.find("HEADLESS-TEE-MARKER") == std::string::npos) {
            std::fprintf(stderr,
                         "FAIL: log was empty for a process with no standard "
                         "streams (%zu bytes)\n", headlessLog.size());
            return 1;
        }
    }

    std::puts("app lifecycle passed: tee drained/restored twice, survived a "
              "process with no standard streams, quoted relaunch arguments; "
              "relaunch failure returned");
    return 0;
}
