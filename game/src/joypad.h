#ifndef _JOYPAD_H_
#define _JOYPAD_H_

#include "types.h"
#include "PR/os_message.h"
#ifdef NATIVE_PORT
#include "input_tick_queue.h"
#include <stddef.h>
#endif

#define CONTROLLER_MISSING -1
#define CONTROLLER_EXISTS   0

#define JOYSTICK_DEADZONE 8
#define JOYSTICK_MAX_RANGE 70

OSMesgQueue *si_mesg(void);
s32 input_init(void);
s32 input_update(s32 saveDataFlags, s32 updateRate);
void input_assign_players(void);
void charselect_assign_players(s8 *activePlayers);
u8 input_player_id(s32 player);
void input_swap_id(void);
u16 input_held(s32 player);
u32 input_pressed(s32 player);
u16 input_released(s32 player);
s32 input_clamp_stick_x(s32 player);
s32 input_clamp_stick_y(s32 player);
s8 input_clamp_stick_mag(s8 stickMag);
void drm_disable_input(void);
#ifdef NATIVE_PORT
#define MDKR_INPUT_ROLLBACK_STATE_SPAN_COUNT 8u
typedef struct MdkrInputRollbackSpan {
    void *address;
    size_t size;
} MdkrInputRollbackSpan;
void input_rollback_capture(MdkrInputSample out[MDKR_INPUT_PORTS]);
void input_rollback_apply(const MdkrInputSample input[MDKR_INPUT_PORTS]);
/* Pure canonical edge calculation shared by live injection and ROM-free
 * regression tests. Presence transitions are neutral-pad transitions. */
void input_rollback_compute_edges(
    const MdkrInputSample *current, const MdkrInputSample *previous,
    u16 button_mask, u16 *pressed, u16 *released);
/* Rebuild the edge calculator after host polling has overwritten the engine's
 * current pad buffer. `previous` is the canonical sample from the completed
 * prior authored tick. */
void input_rollback_apply_from_previous(
    const MdkrInputSample input[MDKR_INPUT_PORTS],
    const MdkrInputSample previous[MDKR_INPUT_PORTS]);
s32 input_rollback_state_spans(
    MdkrInputRollbackSpan spans[MDKR_INPUT_ROLLBACK_STATE_SPAN_COUNT]);
#endif

#endif
