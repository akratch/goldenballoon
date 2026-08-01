// app_relaunch.h — the one platform seam for orderly process replacement.
#ifndef MDKR64_APP_RELAUNCH_H
#define MDKR64_APP_RELAUNCH_H

// Replace this process with a bare launcher invocation. Returns an errno-style
// nonzero value only when replacement failed; success never returns.
int AppRelaunch_replace(const char *executablePath);

#endif  // MDKR64_APP_RELAUNCH_H
