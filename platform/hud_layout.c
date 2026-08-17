#include "hud_layout.h"

#define MDKR_HUD_LOGICAL_WIDTH 320.0f
#define MDKR_HUD_LOGICAL_HEIGHT 240.0f
#define MDKR_HUD_AUTHORED_ASPECT (4.0f / 3.0f)

float mdkr_hud_horizontal_offset(float presentation_aspect,
                                 int widescreen_hud_enabled,
                                 MdkrHudAnchor anchor) {
    float expanded_width;

    if (!widescreen_hud_enabled || anchor == MDKR_HUD_ANCHOR_CENTER ||
        presentation_aspect <= MDKR_HUD_AUTHORED_ASPECT) {
        return 0.0f;
    }
    expanded_width = MDKR_HUD_LOGICAL_HEIGHT * presentation_aspect;
    return (float)anchor *
           (expanded_width - MDKR_HUD_LOGICAL_WIDTH) * 0.5f;
}
