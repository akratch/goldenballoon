#ifndef _RCP_DKR_H_
#define _RCP_DKR_H_

#include <ultra64.h>
#include "structs.h"
#include "sched.h"
#include "game_ui.h"

// Recommended size is around 100KB, or 0x19000. This is unused though so it doesn't matter.
#define OUTPUT_BUFFER_SIZE 0x1800

enum TextureRectangleFlags {
    TEXRECT_BILERP,
    TEXRECT_POINT = (1 << 0),
    TEXRECT_FLIP_X = (1 << 12),
    TEXRECT_FLIP_Y = (1 << 13),
};

typedef s32 (*BackgroundFunction)(Gfx **, Mtx **);

typedef struct DKR_OSTask {
    struct DKR_OSTask *next;
    u32 state;
    u32 flags;
    void *frameBuffer;
    OSTask_t task; // Size: 0x40 bytes
    OSMesgQueue *mesgQueue;
    OSMesg mesg;
#ifdef NATIVE_PORT
    /* Immutable identity of the game pass that authored task.data_ptr. The
     * host may not reconstruct this from its live simulation counter because
     * the double-buffered list is submitted one pass after it was built. */
    u64 presentationAuthoredTick;
#endif
    s32 unused58;
    s32 unused5C;
    s32 unused60;
    s32 unused64;
    u32 unk68;
    s32 unused6C;
} DKR_OSTask;

extern s16 gGfxTaskMesgNums[16];

extern u32 sBackgroundFillColour;

extern TextureHeader *gTexBGTex1;
extern TextureHeader *gTexBGTex2;
extern s32 gChequerBGEnabled;

extern s32 gGfxBufCounter;
extern s32 gGfxBufCounter2;
extern s32 gGfxTaskIsRunning;

extern long long int	rspF3DDKRDramStart[], rspF3DDKRDramEnd[];
extern long long int	rspF3DDKRXbusStart[], rspF3DDKRXbusEnd[];
extern long long int	rspF3DDKRDataXbusStart[], rspF3DDKRDataXbusEnd[];
extern long long int	rspF3DDKRFifoStart[], rspF3DDKRFifoEnd[];
extern long long int	rspF3DDKRDataFifoStart[], rspF3DDKRDataFifoEnd[];

s32 gfxtask_wait(void);
void bgdraw_primcolour(u8 red, u8 green, u8 blue);
void bgdraw_fillcolour(s32 red, s32 green, s32 blue);
void rdp_init(Gfx **dList);
void rsp_init(Gfx **dList);
#ifdef NATIVE_PORT
/* Mark the one main display list whose immutable authored tick will be carried
 * by the next submitted graphics task. Sub-display-list state resets must not
 * start a new authoring lifetime. */
void presentation_task_authoring_begin(Gfx *cursor);
#endif
void gfxtask_init(OSSched *sc);
void bgdraw_texture_init(TextureHeader *tex1, TextureHeader *tex2, u32 shiftX);
void bgdraw_texture(Gfx **dList);
s32 gfxtask_run_xbus(Gfx* dlBegin, Gfx* dlEnd, s32 recvMesg);
void gfxtask_run_fifo(Gfx* dlBegin, Gfx* dlEnd, s32 recvMesg);
void texrect_draw(Gfx **dList, DrawTexture *element, s32 xPos, s32 yPos, u8 red, u8 green, u8 blue,
                               u8 alpha);
void bgdraw_chequer(Gfx** dList);
void bgdraw_render(Gfx **dList, Mtx **mtx, s32 drawBG);
void bgdraw_set_func(BackgroundFunction func);
void texrect_draw_scaled(Gfx **dList, DrawTexture *element, f32 xPos, f32 yPos, f32 xScale, f32 yScale, u32 colour, s32 flags);


#endif
