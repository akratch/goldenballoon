/*
 * os_internal_error.h — clean-room internal error-reporting declarations.
 *
 * Declares the private hooks the OS layer uses to report fatal errors
 * and to walk the list of threads that have taken a CPU fault; this is
 * a declaration-only compatibility surface with no vendor implementation
 * present.
 */

#ifndef _OS_INTERNAL_ERROR_H_
#define	_OS_INTERNAL_ERROR_H_

#ifdef _LANGUAGE_C_PLUS_PLUS
extern "C" {
#endif

#include <PR/os.h>

#if defined(_LANGUAGE_C) || defined(_LANGUAGE_C_PLUS_PLUS)

/* Error handling */

extern void		__osError(s16, s16, ...);
extern OSThread *	__osGetCurrFaultedThread(void);
extern OSThread *	__osGetNextFaultedThread(OSThread *);


#endif /* _LANGUAGE_C */

#ifdef _LANGUAGE_C_PLUS_PLUS
}
#endif

#endif /* !_OS_INTERNAL_ERROR_H */
