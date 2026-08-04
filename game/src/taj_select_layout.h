#ifndef _TAJ_SELECT_LAYOUT_H_
#define _TAJ_SELECT_LAYOUT_H_

#include "types.h"

#define TAJ_SELECT_MAX_ROW 6

typedef enum TajSelectRow {
    TAJ_SELECT_ROW_NONE   = -1,
    TAJ_SELECT_ROW_TOP    = 0,
    TAJ_SELECT_ROW_BOTTOM = 1,
} TajSelectRow;

/* One source of truth for both the cursor graph and the actors on screen. */
typedef struct TajSelectLayout {
    s8 top[TAJ_SELECT_MAX_ROW];
    s8 bottom[TAJ_SELECT_MAX_ROW];
    s8 topCount;
    s8 bottomCount;
    s8 characterCount;
    s8 tajIndex;
} TajSelectLayout;

s32  taj_select_layout_build(TajSelectLayout *layout, s32 baseCount, s32 drumstickUnlocked, s32 ttUnlocked);
s32  taj_select_layout_position(const TajSelectLayout *layout,
                                s32                    characterIndex,
                                TajSelectRow          *row,
                                s32                   *slot,
                                f32                   *xPosition);
f32  taj_select_layout_scale(const TajSelectLayout *layout,
                             s32                    characterIndex);
void taj_select_layout_navigation(const TajSelectLayout *layout,
                                  s32                    characterIndex,
                                  s8                     up[2],
                                  s8                     down[2],
                                  s8                     physicalLeft[4],
                                  s8                     physicalRight[4]);

#endif
