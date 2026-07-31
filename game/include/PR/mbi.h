#ifndef _MBI_H_
#define	_MBI_H_

/*
 * mbi.h — clean-room media binary interface umbrella header.
 *
 * Pulls together the graphics (gbi.h) and audio (abi.h) binary interfaces
 * into one media binary interface, plus the segmented-address helpers and
 * task-list constants shared by both. RSP microcode source includes this
 * file too, so anything C-only stays guarded behind #ifdef _LANGUAGE_C.
 * This is a declaration-only compatibility surface; no vendor code is here.
 */


/*
 * _SHIFTL/_SHIFTR pack or unpack a bit-field of width w at bit offset s
 * within a 32-bit value, used throughout the display-list command encoders.
 * _SHIFTL masks the low w bits of v before shifting them up to s; _SHIFTR
 * shifts v down by s first and then masks off the low w bits.
 *
 * A full 32-bit width doesn't work with _SHIFTL (the mask overflows) — just
 * assign the value directly in that case.
 */
#define _SHIFTL(v, s, w)	\
    ((unsigned int) (((unsigned int)(v) & ((0x01 << (w)) - 1)) << (s)))
#define _SHIFTR(v, s, w)	\
    ((unsigned int)(((unsigned int)(v) >> (s)) & ((0x01 << (w)) - 1)))

#define _SHIFT _SHIFTL	/* old, for compatibility only */

#define G_ON	(1)
#define G_OFF	(0)

/**************************************************************************
 *
 * Graphics Binary Interface
 *
 **************************************************************************/

#include <PR/gbi.h>

/**************************************************************************
 *
 * Audio Binary Interface
 *
 **************************************************************************/

#include <PR/abi.h>

/**************************************************************************
 *
 * Task list
 *
 **************************************************************************/

#define	M_GFXTASK	1
#define	M_AUDTASK	2
#define	M_VIDTASK	3
#define M_HVQTASK	6
#define M_HVQMTASK	7

/**************************************************************************
 *
 * Segment macros and definitions
 *
 **************************************************************************/

#define	NUM_SEGMENTS		(16)
#define	SEGMENT_OFFSET(a)	((unsigned int)(a) & 0x00ffffff)
#define	SEGMENT_NUMBER(a)	(((unsigned int)(a) << 4) >> 28)
#define	SEGMENT_ADDR(num, off)	(((num) << 24) + (off))

#ifndef NULL
#define NULL 0
#endif

#endif /* !_MBI_H_ */
