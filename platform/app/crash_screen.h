/* crash_screen.h — what the player gets instead of a window vanishing (US-6).
 *
 * THE ONE RULE. The engine's `[CRASH]` and `[FATAL]` stdout markers are what
 * every harness in tests/ greps for. This module is STRICTLY ADDITIVE: it never
 * writes, reorders, buffers or suppresses those markers, and every line it
 * emits is written only AFTER the fatal path that owns the marker has flushed
 * it. It also leaves the process's exit disposition alone -- each handler
 * restores SIG_DFL and re-raises, exactly as platform/main_pc.c's backtrace
 * handler already does, so CI classification of a crashed run does not shift.
 *
 * WHERE IT HOOKS. Two seams, because the engine already owns one of them:
 *
 *   SIGSEGV / SIGBUS  platform/main_pc.c writes `[CRASH]` plus a backtrace and
 *                     then calls g_mdkrCrashScreenHook. Taking those signals
 *                     over instead would have replaced that marker; being
 *                     called by it is what guarantees "after the flush".
 *   SIGABRT, SIGILL,  nothing else hooks these, so this module installs its own
 *   SIGFPE            handlers. Every `[FATAL]` path in the tree ends in
 *                     abort(), which is why SIGABRT is the one that matters.
 *
 * MDKR_NO_CRASH_HANDLER turns the whole surface off, the same switch that
 * already suppresses the engine's backtrace handler.
 */
#ifndef MDKR64_CRASH_SCREEN_H
#define MDKR64_CRASH_SCREEN_H

/* SDL2 spells its window type `struct SDL_Window`, so the app shell can hand
 * one over without this header pulling SDL into every consumer. */
struct SDL_Window;

#ifdef __cplusplus
extern "C" {
#endif

/* Arm the crash surface. Idempotent, cheap, and safe to call before anything
 * else exists: every field the report names is read at fault time, not now.
 * Call it above the automation dispatch so headless runs are covered too. */
void CrashScreen_install(void);

/* Register (or clear, with NULL) the window the report may be presented in.
 * AppHost owns this pair. With no window registered the report is printed and
 * nothing is presented -- which is the whole of the headless behaviour, and a
 * structural property rather than a flag: automation never builds an AppHost. */
void CrashScreen_setWindow(struct SDL_Window *window);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_CRASH_SCREEN_H */
