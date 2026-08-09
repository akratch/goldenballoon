/** enhancement_registry.c — see enhancement_registry.h. */
#include "enhancement_registry.h"

static const MdkrEnhancement s_enhancements[] = {
    {
        MDKR_ENH_SPEEDOMETER,
        "Speedometer",
        "Shows your current speed in the corner of the screen while you race.",
        MDKR_ENH_PRESENTATION,
        MDKR_ENH_CAT_DISPLAY,
    },
    {
        MDKR_ENH_DRAW_DISTANCE,
        "Draw distance",
        "Draws scenery further ahead so the track stops popping in near the "
        "horizon.",
        MDKR_ENH_PRESENTATION,
        MDKR_ENH_CAT_DISPLAY,
    },
    {
        MDKR_ENH_LOD_BIAS,
        "Model detail",
        "Keeps higher-detail models on screen further into the distance.",
        MDKR_ENH_PRESENTATION,
        MDKR_ENH_CAT_DISPLAY,
    },
    {
        MDKR_ENH_AI_DIFFICULTY,
        "Opponent skill",
        "Makes the other racers push harder, for a tougher replay once "
        "you've beaten the game.",
        MDKR_ENH_GAMEPLAY,
        MDKR_ENH_CAT_DIFFICULTY,
    },
};

#define MDKR_ENHANCEMENT_COUNT \
    ((int)(sizeof(s_enhancements) / sizeof(s_enhancements[0])))

int mdkr_enhancement_count(void) { return MDKR_ENHANCEMENT_COUNT; }

const MdkrEnhancement *mdkr_enhancement_at(int index) {
    if (index < 0 || index >= MDKR_ENHANCEMENT_COUNT)
        return NULL;
    return &s_enhancements[index];
}

const MdkrEnhancement *mdkr_enhancement_for_key(MdkrVideoKey key) {
    for (int i = 0; i < MDKR_ENHANCEMENT_COUNT; i++) {
        if (s_enhancements[i].key == key)
            return &s_enhancements[i];
    }
    return NULL;
}
