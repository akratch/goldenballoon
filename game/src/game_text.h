#ifndef _GAME_TEXT_H_
#define _GAME_TEXT_H_

#include "types.h"

#define SET_TEXTBOX_BOUNDARY(leftVal, topVal, rightVal, bottomVal) \
    textBox.left = leftVal;                                        \
    textBox.top = topVal;                                          \
    textBox.right = rightVal;                                      \
    textBox.bottom = bottomVal

#define SET_TEXTBOX_TEXT_POS_AND_LINEHEIGHT(x, y, height) \
    textBox.textX = x;                                    \
    textBox.textY = y;                                    \
    textBox.lineHeight = height

enum TextProperties {
    TEXT_NONE,
    TEXT_COL_R,
    TEXT_COL_G,
    TEXT_COL_B,
    TEXT_ALPHA,
    TEXT_FONT,
    TEXT_FLAGS
};

typedef struct GameTextTableStruct  {
#ifdef NATIVE_PORT
 /* NATIVE_PORT: these are NOT host pointers. `entries` is overlaid directly on
  * ASSET_GAME_TEXT_TABLE bytes by set_current_text()'s 16-byte asset_load(), and
  * each element is a big-endian 32-bit word (flags<<24 | ROM offset) that the
  * code only ever masks with 0xFF000000 / 0x00FFFFFF. Declaring them `char *`
  * makes the stride 8 bytes on LP64, which (a) mis-parses every entry and (b)
  * doubles the byte offset of &entries[32], which load_game_text_table() uses as
  * the base of the two 960-byte message buffers inside a 0x800 allocation
  * (128+960+960 == 0x800 exactly; 256+960+960 overruns it by 128 bytes).
  * See docs/OPEN_ITEMS.md "P3.5". */
 u32 entries[128];
 u32 somethingElse;
#else
 char *entries[128];
 s32 *somethingElse;
#endif
} GameTextTableStruct;

#ifdef NATIVE_PORT
/* The N64 layout this struct must keep: 128 4-byte slots + one 4-byte slot. */
_Static_assert(sizeof(GameTextTableStruct) == 128 * 4 + 4,
               "GameTextTableStruct must keep the N64 on-disk stride (4-byte entries)");
_Static_assert(__builtin_offsetof(GameTextTableStruct, entries[32]) == 128,
               "&entries[32] must land at byte 128 — it is the base of the message buffers");
#endif

typedef struct TextBox {
    s32 font;
    s32 left;
    s32 top;
    s32 right;
    s32 bottom;
    s32 textColRed;
    s32 textColGreen;
    s32 textColBlue;
    s32 textColAlpha;
    s32 textFlags;
    s32 textX;
    s32 textY;
    s32 lineHeight;
} TextBox;

void init_dialogue_text(void);
void free_message_box(void);
void set_subtitles(s32 setting);
void process_subtitles(s32 updateRate);
void load_game_text_table(void);
void free_game_text_table(void);
void set_textbox_display_value(s32 num);
void reset_delayed_text(void);
void set_delayed_text(s32 textID, f32 delay);
s32 textbox_visible(void);
void process_onscreen_textbox(s32 updateRate);
void find_next_subtitle(void);
s32 func_800C38B4(s32 arg0, TextBox *textbox);
void render_subtitles(void);
void set_current_text(s32 textID);
s32 dialogue_challenge_loop(void);

#endif
