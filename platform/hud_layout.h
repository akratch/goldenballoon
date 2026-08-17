#ifndef MDKR_HUD_LAYOUT_H
#define MDKR_HUD_LAYOUT_H

typedef enum MdkrHudAnchor {
    MDKR_HUD_ANCHOR_CENTER = 0,
    MDKR_HUD_ANCHOR_LEFT = -1,
    MDKR_HUD_ANCHOR_RIGHT = 1
} MdkrHudAnchor;

float mdkr_hud_horizontal_offset(float presentation_aspect,
                                 int widescreen_hud_enabled,
                                 MdkrHudAnchor anchor);

#endif
