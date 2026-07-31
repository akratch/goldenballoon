/*
 * os_internal_gio.h — clean-room internal dev-board GIO declarations.
 *
 * Declares the private general-purpose I/O hooks used on development
 * hardware for initialization and for handling raw and processed
 * interrupts; this is a declaration-only compatibility surface with no
 * vendor implementation present.
 */

#ifndef _OS_INTERNAL_GIO_H_
#define	_OS_INTERNAL_GIO_H_

#ifdef _LANGUAGE_C_PLUS_PLUS
extern "C" {
#endif

#include <PR/os.h>

#if defined(_LANGUAGE_C) || defined(_LANGUAGE_C_PLUS_PLUS)

/* Development board functions */

extern void		__osGIOInit(s32);
extern void		__osGIOInterrupt(s32);
extern void		__osGIORawInterrupt(s32);


#endif /* _LANGUAGE_C */

#ifdef _LANGUAGE_C_PLUS_PLUS
}
#endif

#endif /* !_OS_INTERNAL_GIO_H */
