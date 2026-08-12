#ifndef _NETWORK_PLAYER_AUTHORITY_H_
#define _NETWORK_PLAYER_AUTHORITY_H_

#include "types.h"

/* Gameplay participant count. Callers supply their exact retail/local value;
 * a frozen online roster may override it only in the native port. Matching
 * builds preprocess back to the original expression. */
#ifdef NATIVE_PORT
s32 mdkr_authoritative_player_count(s32 localPlayerCount);
#else
#define mdkr_authoritative_player_count(localPlayerCount) (localPlayerCount)
#endif

#endif
