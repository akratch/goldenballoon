#include "taj_select_layout.h"

#include <stdio.h>
#include <string.h>

static int sFailures;

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "FAIL: %s\n", message);                            \
            sFailures++;                                                       \
        }                                                                      \
    } while (0)

static void check_candidates(const TajSelectLayout *layout, s32 source,
                             const s8 *candidates, s32 count,
                             TajSelectRow expectedRow,
                             s32 horizontalDirection) {
    TajSelectRow sourceRow;
    f32 sourceX;
    s32 seen[11] = {0};
    s32 ended = FALSE;
    s32 i;

    CHECK(
        taj_select_layout_position(layout, source, &sourceRow, NULL, &sourceX),
        "candidate source must have a position");
    for (i = 0; i < count; i++) {
        TajSelectRow row;
        f32 x;
        s32 candidate = candidates[i];
        if (candidate < 0) {
            ended = TRUE;
            continue;
        }
        CHECK(!ended, "candidate lists must be densely terminated");
        CHECK(candidate < layout->characterCount,
              "candidate index must remain inside the runtime roster");
        if (candidate >= layout->characterCount || candidate >= 11) {
            continue;
        }
        CHECK(candidate != source, "a direction must not select itself");
        CHECK(!seen[candidate], "a direction must not repeat candidates");
        seen[candidate] = TRUE;
        CHECK(taj_select_layout_position(layout, candidate, &row, NULL, &x),
              "every candidate must have a physical position");
        CHECK(row == expectedRow,
              "direction candidate must use the expected row");
        if (horizontalDirection < 0) {
            CHECK(x < sourceX, "left candidates must be physically left");
        } else if (horizontalDirection > 0) {
            CHECK(x > sourceX, "right candidates must be physically right");
        } else {
            CHECK(row != sourceRow, "vertical candidates must cross rows");
        }
    }
}

static void check_connected(const TajSelectLayout *layout) {
    s32 visited[11] = {0};
    s32 progress = TRUE;
    s32 character;
    visited[0] = TRUE;
    while (progress) {
        progress = FALSE;
        for (character = 0; character < layout->characterCount; character++) {
            s8 up[2], down[2], left[4], right[4];
            const s8 *directions[4] = {up, down, left, right};
            const s32 counts[4] = {2, 2, 4, 4};
            s32 direction;
            s32 i;
            if (!visited[character]) {
                continue;
            }
            taj_select_layout_navigation(layout, character, up, down, left,
                                         right);
            for (direction = 0; direction < 4; direction++) {
                for (i = 0; i < counts[direction]; i++) {
                    s32 candidate = directions[direction][i];
                    CHECK(candidate < layout->characterCount,
                          "connected graph candidate must remain in bounds");
                    if (candidate >= 0 && candidate < layout->characterCount &&
                        !visited[candidate]) {
                        visited[candidate] = TRUE;
                        progress = TRUE;
                    }
                }
            }
        }
    }
    for (character = 0; character < layout->characterCount; character++) {
        CHECK(visited[character],
              "the complete roster graph must be connected");
    }
}

static void check_layout(s32 baseCount, s32 drumstick, s32 tt, const s8 *top,
                         const f32 *topX, s32 topCount, const s8 *bottom,
                         const f32 *bottomX, s32 bottomCount) {
    TajSelectLayout layout;
    s8 up[2], down[2], left[4], right[4];
    TajSelectRow row;
    s32 slot;
    f32 x;
    s32 character;

    CHECK(taj_select_layout_build(&layout, baseCount, drumstick, tt),
          "supported layout must build");
    CHECK(layout.characterCount == baseCount + 1,
          "Taj must extend the retail roster contiguously");
    CHECK(layout.tajIndex == baseCount, "Taj index must follow retail racers");
    CHECK(layout.topCount == topCount &&
              !memcmp(layout.top, top, (size_t)topCount),
          "top visual row must match expected roster");
    CHECK(layout.bottomCount == bottomCount &&
              !memcmp(layout.bottom, bottom, (size_t)bottomCount),
          "bottom visual row must match expected roster");

    for (character = 0; character < layout.characterCount; character++) {
        CHECK(taj_select_layout_position(&layout, character, &row, &slot, &x),
              "every selectable index must have one physical position");
        CHECK(x == (row == TAJ_SELECT_ROW_TOP ? topX[slot] : bottomX[slot]),
              "every actor must retain its approved row position");
        taj_select_layout_navigation(&layout, character, up, down, left, right);
        if (row == TAJ_SELECT_ROW_TOP) {
            CHECK(down[0] >= 0, "every top-row racer must navigate downward");
            CHECK(up[0] < 0,
                  "top-row racers must not navigate above the roster");
        } else {
            CHECK(up[0] >= 0, "every bottom-row racer must navigate upward");
            CHECK(down[0] < 0,
                  "bottom-row racers must not navigate below the roster");
        }
        check_candidates(&layout, character, left, 4, row, -1);
        check_candidates(&layout, character, right, 4, row, 1);
        check_candidates(&layout, character,
                         row == TAJ_SELECT_ROW_TOP ? down : up, 2,
                         row == TAJ_SELECT_ROW_TOP ? TAJ_SELECT_ROW_BOTTOM
                                                   : TAJ_SELECT_ROW_TOP,
                         0);
        if (slot > 0) {
            CHECK(left[0] >= 0, "non-edge racer must navigate left");
        }
    }

    taj_select_layout_navigation(&layout, layout.tajIndex, up, down, left,
                                 right);
    CHECK(up[0] >= 0 && left[0] >= 0 && right[0] >= 0,
          "Taj must be integrated into both axes of the roster graph");
    CHECK((bottomCount == 6 &&
           taj_select_layout_scale(&layout, layout.tajIndex) == 0.90f) ||
              (bottomCount == 5 &&
               taj_select_layout_scale(&layout, layout.tajIndex) == 1.0f),
          "only the six-actor row should receive safe-area scaling");
    check_connected(&layout);
}

int main(void) {
    static const s8 baseTop[] = {0, 1, 2, 3};
    static const f32 topX4[] = {-33.0f, -5.0f, 20.0f, 48.0f};
    static const s8 baseBottom[] = {4, 5, 8, 6, 7};
    static const f32 bottomX5[] = {-27.0f, -11.0f, 8.0f, 26.0f, 43.0f};
    static const s8 drumTop[] = {0, 1, 8, 2, 3};
    static const f32 topX5[] = {-37.0f, -13.0f, 8.0f, 31.0f, 57.0f};
    static const s8 drumBottom[] = {4, 5, 9, 6, 7};
    static const s8 ttTop[] = {0, 1, 2, 3};
    static const s8 ttBottom[] = {4, 5, 8, 9, 6, 7};
    static const f32 bottomX6[] = {-36.0f, -21.0f, -5.0f, 11.0f, 28.0f, 45.0f};
    static const s8 completeTop[] = {0, 1, 8, 2, 3};
    static const s8 completeBottom[] = {4, 5, 9, 10, 6, 7};
    TajSelectLayout invalid;

    check_layout(8, FALSE, FALSE, baseTop, topX4, 4, baseBottom, bottomX5, 5);
    check_layout(9, TRUE, FALSE, drumTop, topX5, 5, drumBottom, bottomX5, 5);
    check_layout(9, FALSE, TRUE, ttTop, topX4, 4, ttBottom, bottomX6, 6);
    check_layout(10, TRUE, TRUE, completeTop, topX5, 5, completeBottom,
                 bottomX6, 6);
    CHECK(!taj_select_layout_build(NULL, 8, FALSE, FALSE),
          "NULL layout must fail closed");
    CHECK(!taj_select_layout_build(&invalid, 9, FALSE, FALSE),
          "inconsistent unlock metadata must fail closed");

    if (sFailures != 0) {
        return 1;
    }
    puts("taj_select_layout: PASS");
    return 0;
}
