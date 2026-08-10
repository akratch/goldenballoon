/** enhancement_registry.c — see enhancement_registry.h. */
#include "enhancement_registry.h"

#include <stdio.h>

static const MdkrEnhancement s_enhancements[] = {
    {
        MDKR_ENH_SPEEDOMETER,
        "Speedometer",
        "Shows your current speed in the corner of the screen while you race.",
        MDKR_ENH_PRESENTATION,
        MDKR_ENH_CAT_DISPLAY,
        "1",   /* mph readout on */
    },
    {
        MDKR_ENH_DRAW_DISTANCE,
        "Draw distance",
        "Draws scenery further ahead so the track stops popping in near the "
        "horizon.",
        MDKR_ENH_PRESENTATION,
        MDKR_ENH_CAT_DISPLAY,
        "400", /* the far end of the 100..400 range */
    },
    {
        MDKR_ENH_LOD_BIAS,
        "Model detail",
        "Keeps higher-detail models on screen further into the distance.",
        MDKR_ENH_PRESENTATION,
        MDKR_ENH_CAT_DISPLAY,
        "2",   /* hold high detail furthest out */
    },
    {
        MDKR_ENH_AI_DIFFICULTY,
        "Opponent skill",
        "Makes the other racers push harder, for a tougher replay once "
        "you've beaten the game.",
        MDKR_ENH_GAMEPLAY,
        MDKR_ENH_CAT_DIFFICULTY,
        "brutal",
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

static const char *authority_name(MdkrEnhAuthority authority) {
    return authority == MDKR_ENH_GAMEPLAY ? "gameplay" : "presentation";
}

static const char *category_name(MdkrEnhCategory category) {
    switch (category) {
    case MDKR_ENH_CAT_DISPLAY:    return "display";
    case MDKR_ENH_CAT_DIFFICULTY: return "difficulty";
    case MDKR_ENH_CAT_COSMETIC:   return "cosmetic";
    default:                      return "unknown";
    }
}

void mdkr_enhancement_dump_table(void) {
    for (int i = 0; i < MDKR_ENHANCEMENT_COUNT; i++) {
        const MdkrEnhancement *e = &s_enhancements[i];
        const MdkrVideoSchema *schema = mdkr_video_schema(e->key);
        /* The schema name, not the label: the gate has to set this key through
         * the same string the ini and --video-set accept, and a player-facing
         * label is not that string. */
        printf("[ENHTABLE] key=%s authority=%s category=%s probe=%s\n",
               schema != NULL ? schema->name : "?",
               authority_name(e->authority),
               category_name(e->category),
               e->probe_value != NULL ? e->probe_value : "");
    }
    fflush(stdout);
}
