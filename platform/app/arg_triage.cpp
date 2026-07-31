// arg_triage.cpp — see arg_triage.h.
#include "arg_triage.h"

#include <cstring>

int mdkr_argv_requests_ui(int argc, char **argv) {
    if (argv == nullptr) return 0;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] != nullptr && std::strcmp(argv[i], "--ui") == 0) return 1;
    }
    return 0;
}

int mdkr_is_automation_invocation(int argc, char **argv) {
    // Explicit opt-in always wins: `mdkr64 --ui` opens the shell even though it
    // has arguments. Nothing else in the harness passes --ui.
    if (mdkr_argv_requests_ui(argc, argv)) return 0;

    // Deny by default: any argument at all means "the caller already told the
    // engine what to do". Only a bare invocation (the .app double-click, or a
    // user typing `mdkr64`) opens the launcher.
    return (argc > 1) ? 1 : 0;
}
