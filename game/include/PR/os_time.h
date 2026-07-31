
/*
 * os_time.h — clean-room system timer declarations.
 *
 * Declares the 64-bit time value, the countdown-timer record linked
 * into a pending-timer list, and the calls this port uses to read the
 * clock and arm or cancel a timer. This is a declaration-only
 * compatibility surface — no vendor implementation is present.
 */

#ifndef _OS_TIME_H_
#define	_OS_TIME_H_

#ifdef _LANGUAGE_C_PLUS_PLUS
extern "C" {
#endif

#include <PR/ultratypes.h>
#include "os_message.h"


#if defined(_LANGUAGE_C) || defined(_LANGUAGE_C_PLUS_PLUS)

/**************************************************************************
 *
 * Type definitions
 *
 */

/*
 * Structure for time value 
 */
typedef u64	OSTime;

/*
 * Structure for interval timer
 */
typedef struct OSTimer_s {
	struct OSTimer_s	*next;	/* point to next timer in list */
	struct OSTimer_s	*prev;	/* point to previous timer in list */
	OSTime			interval;	/* duration set by user */
	OSTime			value;		/* time remaining before */
						/* timer fires           */
	OSMesgQueue		*mq;		/* Message Queue */
	OSMesg			msg;		/* Message to send */
} OSTimer;


#endif /* defined(_LANGUAGE_C) || defined(_LANGUAGE_C_PLUS_PLUS) */

/**************************************************************************
 *
 * Global definitions
 *
 */


#if defined(_LANGUAGE_C) || defined(_LANGUAGE_C_PLUS_PLUS)

/**************************************************************************
 *
 * Macro definitions
 *
 */


/**************************************************************************
 *
 * Extern variables
 *
 */


/**************************************************************************
 *
 * Function prototypes
 *
 */

/* Timer interface */

extern OSTime		osGetTime(void);
extern void		osSetTime(OSTime);
extern int		osSetTimer(OSTimer *, OSTime, OSTime,
				   OSMesgQueue *, OSMesg);
extern int		osStopTimer(OSTimer *);


#endif  /* defined(_LANGUAGE_C) || defined(_LANGUAGE_C_PLUS_PLUS) */

#ifdef _LANGUAGE_C_PLUS_PLUS
}
#endif

#endif /* !_OS_TIME_H_ */
