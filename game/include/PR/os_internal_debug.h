/*
 * os_internal_debug.h — clean-room internal debug-port declarations.
 *
 * Declares the private debug-port hooks the OS layer uses internally for
 * synchronized character output and an atomic decrement primitive; this
 * is a declaration-only compatibility surface with no vendor
 * implementation present.
 */

#ifndef _OS_INTERNAL_DEBUG_H_
#define	_OS_INTERNAL_DEBUG_H_

#ifdef _LANGUAGE_C_PLUS_PLUS
extern "C" {
#endif

#include <PR/os.h>

#if defined(_LANGUAGE_C) || defined(_LANGUAGE_C_PLUS_PLUS)

/* Debug port */
extern void		__osSyncPutChars(int, int, const char *);
extern int		__osAtomicDec(unsigned int *p);


#endif /* _LANGUAGE_C */

#ifdef _LANGUAGE_C_PLUS_PLUS
}
#endif

#endif /* !_OS_INTERNAL_DEBUG_H */
