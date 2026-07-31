
/*
 * os_convert.h — clean-room timer/clock unit conversion declarations.
 *
 * Declares the CPU counter rate and the macros that convert between CPU
 * cycle counts and wall-clock nanoseconds/microseconds, plus the virtual
 * segment (K0/K1) to physical address translation helpers. Declaration-only
 * compatibility surface for this port; no vendor implementation is present.
 */

#ifndef _OS_CONVERT_H_
#define	_OS_CONVERT_H_

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

#define	OS_CLOCK_RATE		62500000LL
#define	OS_CPU_COUNTER		(OS_CLOCK_RATE*3/4)


#if defined(_LANGUAGE_C) || defined(_LANGUAGE_C_PLUS_PLUS)

/**************************************************************************
 *
 * Macro definitions
 *
 */

#define OS_NSEC_TO_CYCLES(n)	(((u64)(n)*(OS_CPU_COUNTER/15625000LL))/(1000000000LL/15625000LL))
#define OS_USEC_TO_CYCLES(n)	(((u64)(n)*(OS_CPU_COUNTER/15625LL))/(1000000LL/15625LL))
#define OS_CYCLES_TO_NSEC(c)	(((u64)(c)*(1000000000LL/15625000LL))/(OS_CPU_COUNTER/15625000LL))
#define OS_CYCLES_TO_USEC(c)	(((u64)(c)*(1000000LL/15625LL))/(OS_CPU_COUNTER/15625LL))

/* OS_K?_TO_PHYSICAL macro bug fix for CodeWarrior */
#if defined(NATIVE_PORT)
/* mdkr64 (LP64): the DL word is 32-bit, so this conversion truncates a 64-bit
 * host pointer. For non-arena pointers (game globals / rodata display lists) the
 * host high bits are then unrecoverable by arithmetic, so the conversion also
 * registers the full pointer in the renderer's pointer registry (dkr_resolve
 * looks it up). See platform/stubs_dkr.c dkr_k0_to_physical. */
extern u32 dkr_k0_to_physical(const void *x);
#define	OS_K0_TO_PHYSICAL(x)	dkr_k0_to_physical((const void *)(x))
/*
 * Serialized runtime pointer fields retain only the low 32-bit arena token.
 * They are already in the original K0 address domain and must not be presented
 * to dkr_k0_to_physical() as if they were full host pointers (that would also
 * trigger an invalid integer-to-pointer conversion on LP64).
 */
#define OS_K0_TOKEN_TO_PHYSICAL(x) ((u32)(x) - 0x80000000u)
#define	OS_K1_TO_PHYSICAL(x)	(u32)(((char *)(x)-0xa0000000))
#elif !defined(__MWERKS__)
#define	OS_K0_TO_PHYSICAL(x)	(u32)(((char *)(x)-0x80000000))
#define OS_K0_TOKEN_TO_PHYSICAL(x) OS_K0_TO_PHYSICAL(x)
#define	OS_K1_TO_PHYSICAL(x)	(u32)(((char *)(x)-0xa0000000))
#else
#define	OS_K0_TO_PHYSICAL(x)	((char *)(x)-0x80000000)
#define OS_K0_TOKEN_TO_PHYSICAL(x) OS_K0_TO_PHYSICAL(x)
#define	OS_K1_TO_PHYSICAL(x)	((char *)(x)-0xa0000000)
#endif

#define	OS_PHYSICAL_TO_K0(x)	(void *)(((u32)(x)+0x80000000))
#define	OS_PHYSICAL_TO_K1(x)	(void *)(((u32)(x)+0xa0000000))


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

/* Address translation routines and macros */

extern u32		 osVirtualToPhysical(void *);
extern void *		 osPhysicalToVirtual(u32);


#endif  /* defined(_LANGUAGE_C) || defined(_LANGUAGE_C_PLUS_PLUS) */

#ifdef _LANGUAGE_C_PLUS_PLUS
}
#endif

#endif /* !_OS_CONVERT_H_ */
