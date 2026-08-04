#include "taj_select_layout.h"

#include <string.h>

static const f32 sTopX4[4] = {-33.0f, -5.0f, 20.0f, 48.0f};
static const f32 sTopX5[5] = {-37.0f, -13.0f, 8.0f, 31.0f, 57.0f};
static const f32 sBottomX5[5] = {-27.0f, -11.0f, 8.0f, 26.0f, 43.0f};
static const f32 sBottomX6[6] = {-36.0f, -21.0f, -5.0f, 11.0f, 28.0f, 45.0f};

static const f32 *taj_select_row_positions(TajSelectRow row, s32 count) {
    if (row == TAJ_SELECT_ROW_TOP) {
        if (count == 4) {
            return sTopX4;
        }
        if (count == 5) {
            return sTopX5;
        }
    } else if (row == TAJ_SELECT_ROW_BOTTOM) {
        if (count == 5) {
            return sBottomX5;
        }
        if (count == 6) {
            return sBottomX6;
        }
    }
    return NULL;
}

static void taj_select_append(s8 *row, s8 *count, s32 characterIndex) {
    if (*count < TAJ_SELECT_MAX_ROW) {
        row[*count] = (s8)characterIndex;
        (*count)++;
    }
}

s32 taj_select_layout_build(TajSelectLayout *layout, s32 baseCount,
                            s32 drumstickUnlocked, s32 ttUnlocked) {
    s32 expectedBaseCount;

    if (layout == NULL) {
        return FALSE;
    }
    memset(layout, -1, sizeof(*layout));
    layout->topCount = 0;
    layout->bottomCount = 0;
    layout->characterCount = 0;
    layout->tajIndex = -1;

    drumstickUnlocked = drumstickUnlocked != FALSE;
    ttUnlocked = ttUnlocked != FALSE;
    expectedBaseCount = 8 + drumstickUnlocked + ttUnlocked;
    if (baseCount != expectedBaseCount || baseCount < 8 || baseCount > 10) {
        return FALSE;
    }

    layout->tajIndex = (s8)baseCount;
    layout->characterCount = (s8)(baseCount + 1);

    taj_select_append(layout->top, &layout->topCount, 0); /* Krunch */
    taj_select_append(layout->top, &layout->topCount, 1); /* Diddy */
    if (drumstickUnlocked) {
        taj_select_append(layout->top, &layout->topCount, 8);
    }
    taj_select_append(layout->top, &layout->topCount, 2); /* Bumper */
    taj_select_append(layout->top, &layout->topCount, 3); /* Banjo */

    taj_select_append(layout->bottom, &layout->bottomCount, 4); /* Conker */
    taj_select_append(layout->bottom, &layout->bottomCount, 5); /* Tiptup */
    if (ttUnlocked) {
        taj_select_append(layout->bottom, &layout->bottomCount,
                          drumstickUnlocked ? 9 : 8);
    }
    /* Taj is a real member of the lower secret-character row. */
    taj_select_append(layout->bottom, &layout->bottomCount, baseCount);
    taj_select_append(layout->bottom, &layout->bottomCount, 6); /* Pipsy */
    taj_select_append(layout->bottom, &layout->bottomCount, 7); /* Timber */

    if (layout->topCount + layout->bottomCount != layout->characterCount ||
        taj_select_row_positions(TAJ_SELECT_ROW_TOP, layout->topCount) ==
            NULL ||
        taj_select_row_positions(TAJ_SELECT_ROW_BOTTOM, layout->bottomCount) ==
            NULL) {
        memset(layout, -1, sizeof(*layout));
        return FALSE;
    }
    return TRUE;
}

s32 taj_select_layout_position(const TajSelectLayout *layout,
                               s32 characterIndex, TajSelectRow *row, s32 *slot,
                               f32 *xPosition) {
    s32 i;
    const f32 *positions;

    if (layout == NULL) {
        return FALSE;
    }
    for (i = 0; i < layout->topCount; i++) {
        if (layout->top[i] == characterIndex) {
            positions =
                taj_select_row_positions(TAJ_SELECT_ROW_TOP, layout->topCount);
            if (positions == NULL) {
                return FALSE;
            }
            if (row != NULL) {
                *row = TAJ_SELECT_ROW_TOP;
            }
            if (slot != NULL) {
                *slot = i;
            }
            if (xPosition != NULL) {
                *xPosition = positions[i];
            }
            return TRUE;
        }
    }
    for (i = 0; i < layout->bottomCount; i++) {
        if (layout->bottom[i] == characterIndex) {
            positions = taj_select_row_positions(TAJ_SELECT_ROW_BOTTOM,
                                                 layout->bottomCount);
            if (positions == NULL) {
                return FALSE;
            }
            if (row != NULL) {
                *row = TAJ_SELECT_ROW_BOTTOM;
            }
            if (slot != NULL) {
                *slot = i;
            }
            if (xPosition != NULL) {
                *xPosition = positions[i];
            }
            return TRUE;
        }
    }
    return FALSE;
}

f32 taj_select_layout_scale(const TajSelectLayout *layout, s32 characterIndex) {
    TajSelectRow row;
    if (!taj_select_layout_position(layout, characterIndex, &row, NULL, NULL)) {
        return 1.0f;
    }
    /* Eleven racers require six actors on the lower row. A subtle, uniform
     * reduction keeps Conker's tail and Timber's feet inside the authored
     * 4:3 safe area without making any one character look demoted. */
    return row == TAJ_SELECT_ROW_BOTTOM && layout->bottomCount == 6 ? 0.90f
                                                                    : 1.0f;
}

static void taj_select_fill(s8 *values, s32 count) {
    memset(values, -1, (size_t)count);
}

static void taj_select_cross_row(const TajSelectLayout *layout,
                                 TajSelectRow sourceRow, f32 sourceX,
                                 s8 output[2]) {
    const s8 *characters;
    const f32 *positions;
    s32 count;
    s32 result;
    s32 i;

    if (sourceRow == TAJ_SELECT_ROW_TOP) {
        characters = layout->bottom;
        count = layout->bottomCount;
        positions = taj_select_row_positions(TAJ_SELECT_ROW_BOTTOM, count);
    } else {
        characters = layout->top;
        count = layout->topCount;
        positions = taj_select_row_positions(TAJ_SELECT_ROW_TOP, count);
    }
    for (result = 0; result < 2; result++) {
        s32 best = -1;
        f32 bestDistance = 100000.0f;
        for (i = 0; i < count; i++) {
            f32 distance;
            if ((result > 0 && characters[i] == output[0]) ||
                positions == NULL) {
                continue;
            }
            distance = positions[i] - sourceX;
            if (distance < 0.0f) {
                distance = -distance;
            }
            if (distance < bestDistance) {
                bestDistance = distance;
                best = i;
            }
        }
        if (best >= 0) {
            output[result] = characters[best];
        }
    }
}

void taj_select_layout_navigation(const TajSelectLayout *layout,
                                  s32 characterIndex, s8 up[2], s8 down[2],
                                  s8 physicalLeft[4], s8 physicalRight[4]) {
    TajSelectRow row;
    const s8 *characters;
    f32 xPosition;
    s32 slot;
    s32 count;
    s32 i;
    s32 output;

    taj_select_fill(up, 2);
    taj_select_fill(down, 2);
    taj_select_fill(physicalLeft, 4);
    taj_select_fill(physicalRight, 4);
    if (!taj_select_layout_position(layout, characterIndex, &row, &slot,
                                    &xPosition)) {
        return;
    }

    characters = row == TAJ_SELECT_ROW_TOP ? layout->top : layout->bottom;
    count = row == TAJ_SELECT_ROW_TOP ? layout->topCount : layout->bottomCount;
    for (i = slot - 1, output = 0; i >= 0 && output < 4; i--) {
        physicalLeft[output++] = characters[i];
    }
    for (i = slot + 1, output = 0; i < count && output < 4; i++) {
        physicalRight[output++] = characters[i];
    }
    if (row == TAJ_SELECT_ROW_TOP) {
        taj_select_cross_row(layout, row, xPosition, down);
    } else {
        taj_select_cross_row(layout, row, xPosition, up);
    }
}
