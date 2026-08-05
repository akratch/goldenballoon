#include "app_relaunch.h"
#include "fs_utf8.h"

#include <cerrno>

int AppRelaunch_replace(const char *executablePath) {
    /* Say what the replacement is for. A bare relaunch is indistinguishable
     * from `mdkr64 <whatever the shell passed>`: arg_triage denies by default,
     * so ANY argument the replacement receives means "the caller already told
     * the engine what to do" and the process runs headless -- no window, no
     * launcher. To the player, Return to Launcher closed the whole app.
     *
     * The Windows exec seam now quotes argv, so a space in the install path no
     * longer manufactures those arguments. `--ui` is the second half of the
     * same guarantee: it is the existing explicit launcher opt-in, it survives
     * any residual mis-split of the command line, and it makes the intent of a
     * relaunched process assertable from the outside. */
    static const char *const launcherArguments[] = {"--ui", nullptr};
    return mdkr_exec_replace_utf8(executablePath, launcherArguments);
}
