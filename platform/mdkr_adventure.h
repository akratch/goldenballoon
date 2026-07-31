#ifndef MDKR_ADVENTURE_H
#define MDKR_ADVENTURE_H

#include "structs.h"

void mdkr_adventure_drive(Object *obj, Object_Racer *racer, s32 updateRate);
void mdkr_autopilot_unstick(Object *obj, Object_Racer *racer, s32 updateRate);
void mdkr_adventure_force_verdict(Object *humanObj, Object **racers,
                                  s32 numRacers);
void mdkr_trophy_control_order(Settings *settings, s32 round, s32 racerCount);
void mdkr_trophy_control_menu_option(s32 completedRound, s32 *menuOption);
void mdkr_trophy_control_world(Settings *settings);
void mdkr_trophy_control_collision(Object *cabinet, Object *player);
void mdkr_trophy_complete_race(Object **racers, s32 racerCount,
                               s32 requiredLaps);

#endif
