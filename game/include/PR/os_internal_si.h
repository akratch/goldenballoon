/*
 * os_internal_si.h — clean-room internal serial-interface declarations.
 *
 * Declares the private entry points the OS layer uses to poll the
 * serial interface's status and issue raw register I/O and DMA
 * transfers over it; this is a declaration-only compatibility surface
 * with no vendor implementation present.
 */

#ifndef _OS_INTERNAL_SI_H_
#define	_OS_INTERNAL_SI_H_

#ifdef _LANGUAGE_C_PLUS_PLUS
extern "C" {
#endif

#include <PR/os.h>

#if defined(_LANGUAGE_C) || defined(_LANGUAGE_C_PLUS_PLUS)

/* Serial interface (Si) */

extern u32 		__osSiGetStatus(void);
extern s32		__osSiRawWriteIo(u32, u32);
extern s32		__osSiRawReadIo(u32, u32 *);
extern s32		__osSiRawStartDma(s32, void *);


#endif /* _LANGUAGE_C */

#ifdef _LANGUAGE_C_PLUS_PLUS
}
#endif

#endif /* !_OS_INTERNAL_SI_H */
