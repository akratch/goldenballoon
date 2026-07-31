// arg_triage.h — decide whether a CLI invocation must bypass the launcher.
//
// The app shell owns main(). Automation/diagnostic invocations (the whole
// tests/ + tools/ harness surface) must run the unchanged engine path so their
// behavior stays byte-identical to the pre-shell binary.
//
// DKR ADAPTATION — inverted relative to mgb64. mgb64 maintains an ALLOW-LIST of
// automation flags and routes everything else (including --rom) into the
// launcher, which it then seeds from a parsed LaunchIntent. That is the riskier
// direction here: mdkr64's harness is CLI-first, with ~197 `--rom` call sites
// across tests/, tools/ and .github/, many of which combine --rom with flags
// that are NOT obviously "automation" (--video-list, --video-set, --pure,
// --window-size, --renderer). One missed flag in an allow-list silently opens a
// window inside a headless gate and hangs it.
//
// So mdkr64 uses a DENY-BY-DEFAULT rule instead: the launcher opens only for a
// bare invocation (no arguments at all) or an explicit `--ui`. Every other argv
// — one flag, one positional ROM, anything — delegates verbatim to
// mdkr64_headless_main(). This makes "no existing invocation changes behavior" a
// property of the rule rather than a property of the list being complete.
//
// The cost is that mgb64's direct-launch seeding (`--rom X` opening a launcher
// pre-filled with X) is deliberately NOT ported; on mdkr64 `--rom X` still means
// "boot this ROM now", which is what every user and script already expects.
#ifndef MDKR64_ARG_TRIAGE_H
#define MDKR64_ARG_TRIAGE_H

#ifdef __cplusplus
extern "C" {
#endif

// Returns 1 when this argv must bypass the launcher and delegate to
// mdkr64_headless_main(); 0 when the interactive shell should open.
int mdkr_is_automation_invocation(int argc, char **argv);

// Returns 1 if argv contains the explicit `--ui` opt-in (which is stripped
// before the engine ever sees it). Exposed for the unit test.
int mdkr_argv_requests_ui(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif  // MDKR64_ARG_TRIAGE_H
