// One-shot environment handoff for an orderly Restart & Apply process replace.
#ifndef MDKR64_APP_RESTART_H
#define MDKR64_APP_RESTART_H

#include <string>

bool AppRestart_stageGame(const char *romPath);
bool AppRestart_consumeGame(std::string &romPath);
void AppRestart_clear();

#endif  // MDKR64_APP_RESTART_H
