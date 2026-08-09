// crash_screen.cpp — see crash_screen.h.
//
// WHY THE FIELD LIST IS THIS ONE. It is the same list tool_diagnostics.cpp
// arrived at for US-1, minus everything that needs a live frame: which build,
// which renderer, which track, which tick, and where the rest of the evidence
// went. A crash report that names those five is actionable; one that says "it
// crashed" is not.
//
// WHY IT READS PUBLISHED RECORDS AND NOTHING ELSE. Same rule as the diagnostics
// window: every value comes from a record some other subsystem already
// publishes (g_simTickCounter, level_id(), mdkr_render_backend_name(),
// AppVersion(), DiagLog_path()). A crash handler that re-derived any of them
// would be inventing state at the exact moment state is least trustworthy.
//
// WHAT RUNS INSIDE A SIGNAL HANDLER. The report is built with snprintf into a
// fixed stack buffer and written with write()/_write() -- no allocation, no
// std::string, no ImGui. That is deliberate: at abort() time the heap may be
// held and, on a SIGSEGV, the ImGui context may be mid-frame and the GPU device
// gone, so re-entering the UI to draw a panel is the one thing most likely to
// turn a diagnosable crash into a silent double fault. The windowed
// presentation therefore goes through SDL_ShowMessageBox -- the same surface
// main_app.cpp already uses for fatal graphics errors -- which gives the player
// a real screen and a real Copy Details button without a render pass.
//
// THE TWO ENGINE READS. level_id()/level_name() are declared here for the same
// reason, and under the same caveats, as in tool_diagnostics.cpp: they live in
// game/src/game.c behind a header the app shell cannot include. Both are pure
// reads and both fail closed. See the note in that file; the duplication is
// called out in the sprint report rather than hidden behind a new shared
// header that only two callers would ever use.
#include "crash_screen.h"

#include "app_brand.h"
#include "app_version.h"
#include "diag_log.h"
#include "engine_entry.h"   // g_mdkrCrashScreenHook, the crash-write mirror fds

#include "platform_os.h"    // mdkr_render_backend_name
#include "present_sched.h"  // g_simTickCounter — the authoritative tick

#include <SDL.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <io.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

extern "C" {
/* game/src/game.c. level_id() returns gMapId; level_name() bounds-checks its
 * argument against gNumberOfLevelHeaders and returns NULL outside it, which is
 * what makes it safe to ask before any level has loaded. */
int   level_id(void);
char *level_name(int levelId);
}

namespace {

SDL_Window *g_window = nullptr;
bool        g_installed = false;

/* Set before a single field is read, so a fault inside the report cannot
 * re-enter it, and cleared by nothing: one crash gets one report. */
volatile sig_atomic_t g_reported = 0;

// --- output ------------------------------------------------------------------
// Written to the descriptor the fatal marker itself just went to, so the two
// cannot interleave out of order. On Windows the tee publishes the pre-tee
// console fd and the raw log fd (engine_entry.h): a crash-time write into the
// tee pipe can be lost when the reader thread dies with the process, which is
// the whole reason that mirror exists.
void crashWrite(const char *text, size_t length) {
    if (text == nullptr || length == 0u) return;
#if defined(_WIN32)
    if (g_diagLogRealErrFd >= 0 || g_diagLogFileFd >= 0) {
        if (g_diagLogRealErrFd >= 0) {
            (void)_write(g_diagLogRealErrFd, text, static_cast<unsigned>(length));
        }
        if (g_diagLogFileFd >= 0) {
            (void)_write(g_diagLogFileFd, text, static_cast<unsigned>(length));
        }
        return;
    }
    (void)_write(2, text, static_cast<unsigned>(length));
#else
    size_t offset = 0;
    while (offset < length) {
        const ssize_t written = write(2, text + offset, length - offset);
        if (written > 0) {
            offset += static_cast<size_t>(written);
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
#endif
}

void crashWriteLine(const char *text) {
    if (text != nullptr) crashWrite(text, std::strlen(text));
}

// --- the fields --------------------------------------------------------------

/* Deliberately not "SIGABRT"/"SIGSEGV"/"Segmentation fault": tests/harness_utils
 * treats those spellings as abort markers, and this line is additive output that
 * must not look like a second fault to a harness scanning for one. */
const char *kindName(int signo) {
    switch (signo) {
        case SIGABRT: return "abort";
        case SIGILL:  return "illegal-instruction";
        case SIGFPE:  return "arithmetic-fault";
        case SIGSEGV: return "memory-fault";
#ifdef SIGBUS
        case SIGBUS:  return "bus-error";
#endif
        default:      return "signal";
    }
}

/* Before the first authoritative tick the game has not finished init_game() and
 * gMapId means nothing, so do not ask. Same guard, same reason, as
 * tool_diagnostics.cpp's currentTrackId(). */
int currentTrackId() {
    return g_simTickCounter > 0 ? level_id() : -1;
}

const char *currentTrackName(int trackId) {
    if (trackId < 0) return "not in a level yet";
    const char *name = level_name(trackId);
    return (name != nullptr && name[0] != '\0') ? name : "unnamed";
}

/* Presented the way tool_diagnostics.cpp presents it: the real path when the
 * tee wrote one, and an honest sentence when it did not. Automation never
 * installs the tee, so a headless run legitimately has no log to name. */
const char *logPath() {
    const char *path = DiagLog_path();
    return (path != nullptr && path[0] != '\0') ? path
                                                : "not written on this platform";
}

const char *versionStamp() {
    const char *stamp = AppBuildStamp();
    return (stamp != nullptr && stamp[0] != '\0') ? stamp : "";
}

// --- the report --------------------------------------------------------------

/* The plain-text block a player copies into an issue. Same field order and the
 * same padded-label shape as ToolDiagnostics_copyBlock(), so a crash report and
 * a Diagnostics report read as one format. */
int formatReport(int signo, char *buffer, int capacity) {
    if (buffer == nullptr || capacity <= 0) return 0;
    const int   trackId = currentTrackId();
    const char *stamp   = versionStamp();
    return std::snprintf(
        buffer, static_cast<size_t>(capacity),
        "%s stopped and could not continue.\n"
        "\n"
        "fault     %s\n"
        "app       %s%s%s%s\n"
        "renderer  %s\n"
        "tick      %d\n"
        "track     %d (%s)\n"
        "log       %s\n"
        "\n"
        "Paste this into a bug report. The log file holds the rest.\n",
        MDKR_BRAND_NAME, kindName(signo), AppVersion(),
        stamp[0] != '\0' ? " (" : "", stamp, stamp[0] != '\0' ? ")" : "",
        mdkr_render_backend_name(), g_simTickCounter, trackId,
        currentTrackName(trackId), logPath());
}

/* One machine-readable line carrying every field, because that is what
 * tests/check_crash_screen.py greps and what a triager pulls out of a log.
 * `present` says whether a screen will be shown at all; with no window there is
 * nothing to present into and the printed block IS the crash screen. */
void emitFields(int signo) {
    char line[2048];
    const int trackId = currentTrackId();
    const int written = std::snprintf(
        line, sizeof line,
        "[CRASHSCREEN] kind=%s tick=%d track=%d track-name=\"%s\" "
        "renderer=%s version=%s present=%d log=\"%s\"\n",
        kindName(signo), g_simTickCounter, trackId, currentTrackName(trackId),
        mdkr_render_backend_name(), AppVersion(),
        g_window != nullptr ? 1 : 0, logPath());
    if (written <= 0) return;
    const size_t length = static_cast<size_t>(written) < sizeof line
                              ? static_cast<size_t>(written)
                              : sizeof line - 1u;
    crashWrite(line, length);
}

void presentPanel(int signo) {
    if (g_window == nullptr) return;

    char report[2048];
    if (formatReport(signo, report, static_cast<int>(sizeof report)) <= 0) {
        return;
    }
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Copy Details"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 1, "Close"},
    };
    SDL_MessageBoxData data;
    std::memset(&data, 0, sizeof data);
    data.flags      = SDL_MESSAGEBOX_ERROR;
    data.window     = g_window;
    data.title      = MDKR_BRAND_NAME " — Stopped";
    data.message    = report;
    data.numbuttons = static_cast<int>(SDL_arraysize(buttons));
    data.buttons    = buttons;

    int pressed = -1;
    int copied  = 0;
    if (SDL_ShowMessageBox(&data, &pressed) == 0 && pressed == 0) {
        copied = SDL_SetClipboardText(report) == 0 ? 1 : 0;
    }
    char line[96];
    const int written = std::snprintf(
        line, sizeof line, "[CRASHSCREEN] presented=1 copied=%d\n", copied);
    if (written > 0) {
        crashWrite(line, static_cast<size_t>(written) < sizeof line
                             ? static_cast<size_t>(written)
                             : sizeof line - 1u);
    }
}

void report(int signo) {
    /* The fatal path that owns the marker has already flushed it; this drains
     * anything else stdout was still holding so the additive lines below cannot
     * appear above output that was produced before the fault. */
    (void)std::fflush(stdout);
    emitFields(signo);
    crashWriteLine("[CRASHSCREEN] The line above is the whole bug report. "
                   "Copy it, and say what you were doing.\n");
    presentPanel(signo);
}

extern "C" {

/* SIGABRT / SIGILL / SIGFPE. Restoring SIG_DFL first means a fault inside the
 * report kills the process with the ORIGINAL fault rather than looping; the
 * re-raise then reproduces exactly the exit disposition the run would have had
 * without this module. */
static void crashScreenSignalHandler(int signo) {
    if (g_reported == 0) {
        g_reported = 1;
        (void)std::signal(signo, SIG_DFL);
        report(signo);
    } else {
        (void)std::signal(signo, SIG_DFL);
    }
    (void)std::raise(signo);
}

/* SIGSEGV / SIGBUS, called by platform/main_pc.c's handler after it has written
 * and flushed `[CRASH]` and the backtrace. That handler owns the re-raise, so
 * this one only reports. */
static void crashScreenEngineHook(int signo) {
    if (g_reported != 0) return;
    g_reported = 1;
    report(signo);
}

}  // extern "C"

}  // namespace

void CrashScreen_install(void) {
    if (g_installed) return;
    g_installed = true;
    /* The same switch that already suppresses the engine's backtrace handler,
     * so "turn the crash surface off" stays one variable and not two. */
    if (std::getenv("MDKR_NO_CRASH_HANDLER") != nullptr) return;

    g_mdkrCrashScreenHook = &crashScreenEngineHook;
    (void)std::signal(SIGABRT, &crashScreenSignalHandler);
    (void)std::signal(SIGILL, &crashScreenSignalHandler);
    (void)std::signal(SIGFPE, &crashScreenSignalHandler);
}

void CrashScreen_setWindow(struct SDL_Window *window) {
    g_window = window;
}
