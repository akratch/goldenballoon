#include "app_relaunch.h"
#include "fs_utf8.h"

#include <cerrno>

int AppRelaunch_replace(const char *executablePath) {
    return mdkr_exec_replace_utf8(executablePath);
}
