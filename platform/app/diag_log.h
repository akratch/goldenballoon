// diag_log.h — capture engine stdout/stderr into a ring buffer + rotating file.
//
// Installed by the app shell on the interactive path only (never the automation
// path), so it tees engine output for the in-app console + bug-report export
// without changing the headless/console behavior the validation harness relies
// on. POSIX: dup2/pipe/reader thread; Windows: the same tee over the CRT fds
// (_pipe/_dup2), which covers everything this codebase prints (printf/fprintf).
#ifndef MDKR64_DIAG_LOG_H
#define MDKR64_DIAG_LOG_H

// Redirect stdout+stderr through a tee: everything is appended to an in-memory
// ring buffer and to mdkr64.log (rotating the previous run to mdkr64.prev.log)
// AND written through to the real console. Safe to call once.
void DiagLog_install();

// Copy the most-recent ring text (NUL-terminated) into buf; returns byte count.
int DiagLog_snapshot(char *buf, int cap);

// Absolute path to mdkr64.log (empty before install / on unsupported platforms).
const char *DiagLog_path();

#endif  // MDKR64_DIAG_LOG_H
