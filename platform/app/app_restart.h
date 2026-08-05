// One-shot environment handoff for an orderly Restart & Apply process replace.
#ifndef MDKR64_APP_RESTART_H
#define MDKR64_APP_RESTART_H

#include <string>

// `autoplaySession` carries the mode of the session being replaced. A CI
// autoplay run stages another autoplay run; a player's Restart & Apply stages
// an interactive-capable session and scrubs the automation-only controls so an
// inherited tick budget, input script, or video override cannot steer it.
bool AppRestart_stageGame(const char *romPath, bool autoplaySession = false);
// True when this process was started as a replacement, whether or not the
// handoff it carries turns out to be usable.
bool AppRestart_pendingGame();
bool AppRestart_consumeGame(std::string &romPath);
void AppRestart_clear();

// The app's single environment seam. Windows needs the wide environment for
// UTF-8 values (ROM paths, recovery messages naming them); every one-shot
// handoff the shell writes or reads goes through these.
bool AppRestart_setEnv(const char *name, const char *value);
bool AppRestart_getEnv(const char *name, std::string &value);

#endif  // MDKR64_APP_RESTART_H
