/*
 * os_internal_tlb.h — clean-room internal TLB-query declarations.
 *
 * Declares the private accessors the OS layer uses to read back TLB
 * entry fields — the current ASID, and per-index page mask and hi/lo
 * halves; this is a declaration-only compatibility surface with no
 * vendor implementation present.
 */

#ifndef _OS_INTERNAL_TLB_H_
#define	_OS_INTERNAL_TLB_H_

#ifdef _LANGUAGE_C_PLUS_PLUS
extern "C" {
#endif

#include <PR/os.h>

#if defined(_LANGUAGE_C) || defined(_LANGUAGE_C_PLUS_PLUS)

/* Routines for fetch TLB info */

extern u32		__osGetTLBASID(void);
extern u32		__osGetTLBPageMask(s32);
extern u32		__osGetTLBHi(s32);
extern u32		__osGetTLBLo0(s32);
extern u32		__osGetTLBLo1(s32);


#endif /* _LANGUAGE_C */

#ifdef _LANGUAGE_C_PLUS_PLUS
}
#endif

#endif /* !_OS_INTERNAL_TLB_H */
