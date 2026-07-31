/*
 * os_internal_exception.h — clean-room internal exception-dispatch
 * declarations.
 *
 * Declares the private entry points the OS layer uses to install and
 * query hardware interrupt handlers, and to set or clear bits in the
 * global interrupt mask; this is a declaration-only compatibility
 * surface with no vendor implementation present.
 */

#ifndef _OS_INTERNAL_EXCEPTION_H_
#define	_OS_INTERNAL_EXCEPTION_H_

#ifdef _LANGUAGE_C_PLUS_PLUS
extern "C" {
#endif

#include "os.h"
#include "os_version.h"

#if defined(_LANGUAGE_C) || defined(_LANGUAGE_C_PLUS_PLUS)

/* Routine for HW interrupt "handler" */
#if BUILD_VERSION >= VERSION_J
extern void 		__osSetHWIntrRoutine(OSHWIntr interrupt,
					     s32 (*handler)(void), void *stackEnd);
extern void 		__osGetHWIntrRoutine(OSHWIntr interrupt,
					     s32 (**handler)(void), void **stackEnd);
#else
extern void 		__osSetHWIntrRoutine(OSHWIntr interrupt,
					     s32 (*handler)(void));
extern void 		__osGetHWIntrRoutine(OSHWIntr interrupt,
					     s32 (**handler)(void));
#endif

/* Routine for global interrupt mask */
extern void		__osSetGlobalIntMask(OSHWIntr);
extern void		__osResetGlobalIntMask(OSHWIntr);


#endif /* _LANGUAGE_C */

#ifdef _LANGUAGE_C_PLUS_PLUS
}
#endif

#endif /* !_OS_INTERNAL_EXCEPTION_H */
