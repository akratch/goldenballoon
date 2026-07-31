
/*
 * os_libc.h — clean-room freestanding C library subset declarations.
 *
 * Declares the small set of freestanding libc-style helpers the original
 * runtime relied on: raw byte-buffer copy/compare/clear (with a
 * MODERN_CC-gated signature switch between size_t and int), plus sprintf
 * and the buffered debug-print entry point. Declaration-only compatibility
 * surface for this port; no vendor implementation is present.
 */

#ifndef _OS_LIBC_H_
#define	_OS_LIBC_H_

#include "os_pfs.h"

#ifdef _LANGUAGE_C_PLUS_PLUS
extern "C" {
#endif

#include <PR/ultratypes.h>

#if defined(_LANGUAGE_C) || defined(_LANGUAGE_C_PLUS_PLUS)

/**************************************************************************
 *
 * Type definitions
 *
 */


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

/* byte string operations */

#ifndef MODERN_CC
extern void     bcopy(const void *, void *, int);
extern int      bcmp(const void *, const void *, int);
extern void     bzero(void *, int);
#else
extern void     bcopy(const void *, void *, size_t);
extern int      bcmp(const void *, const void *, size_t);
extern void     bzero(void *, size_t);
#endif

/* Printf */

extern int sprintf(char *s, const char *format, ...);
extern void		osSyncPrintf(const char *fmt, ...);


#endif  /* defined(_LANGUAGE_C) || defined(_LANGUAGE_C_PLUS_PLUS) */

#ifdef _LANGUAGE_C_PLUS_PLUS
}
#endif

#endif /* !_OS_LIBC_H_ */
