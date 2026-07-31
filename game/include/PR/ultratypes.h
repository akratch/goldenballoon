#ifndef _ULTRATYPES_H_
#define _ULTRATYPES_H_


/*
 * ultratypes.h — clean-room fixed-width scalar typedefs (u8..s64, f32, f64)
 * for the whole SDK surface.
 *
 * Declares the fixed-width integer, volatile-integer, and floating-point
 * typedefs that every other header in this compatibility layer is expressed
 * in, along with TRUE/FALSE/NULL. It is a declaration-only compatibility
 * surface: no vendor implementation is present, only the type names the
 * rest of the port relies on.
 */



/**********************************************************************
 * General data types for R4300
 */
#if defined(_LANGUAGE_C) || defined(_LANGUAGE_C_PLUS_PLUS)

#if defined(NATIVE_PORT)
/* Native (LP64) host: the decomp requires EXACT 32-bit u32/s32 semantics, but
 * `unsigned long` is 64-bit here — so pin the widths with <stdint.h>. u64/s64
 * stay 64-bit; pointers keep the host's real 64-bit uintptr_t (see types.h). */
#include <stdint.h>
#include <stddef.h>   /* real size_t */
typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;
typedef int8_t    s8;
typedef int16_t   s16;
typedef int32_t   s32;
typedef int64_t   s64;
typedef volatile uint8_t   vu8;
typedef volatile uint16_t  vu16;
typedef volatile uint32_t  vu32;
typedef volatile uint64_t  vu64;
typedef volatile int8_t    vs8;
typedef volatile int16_t   vs16;
typedef volatile int32_t   vs32;
typedef volatile int64_t   vs64;
typedef float   f32;
typedef double  f64;
#else
typedef unsigned char       u8;     /* unsigned  8-bit */
typedef unsigned short      u16;    /* unsigned 16-bit */
typedef unsigned long       u32;    /* unsigned 32-bit */
typedef unsigned long long  u64;    /* unsigned 64-bit */

typedef signed char         s8;     /* signed  8-bit */
typedef short               s16;    /* signed 16-bit */
typedef long                s32;    /* signed 32-bit */
typedef long long           s64;    /* signed 64-bit */

typedef volatile unsigned char      vu8;    /* unsigned  8-bit */
typedef volatile unsigned short     vu16;   /* unsigned 16-bit */
typedef volatile unsigned long      vu32;   /* unsigned 32-bit */
typedef volatile unsigned long long vu64;   /* unsigned 64-bit */

typedef volatile signed char        vs8;    /* signed  8-bit */
typedef volatile short              vs16;   /* signed 16-bit */
typedef volatile long               vs32;   /* signed 32-bit */
typedef volatile long long          vs64;   /* signed 64-bit */

typedef float   f32;    /* single prec floating point */
typedef double  f64;    /* double prec floating point */
#endif  /* NATIVE_PORT */

#if !defined(NATIVE_PORT)   /* native: use the host's <stddef.h> size_t */
#if !defined(_SIZE_T) && !defined(_SIZE_T_) && !defined(_SIZE_T_DEF)
#define _SIZE_T
#define _SIZE_T_DEF         /* exeGCC size_t define label */
#if (_MIPS_SZLONG == 32)
typedef unsigned int    size_t;
#endif
#if (_MIPS_SZLONG == 64)
typedef unsigned long   size_t;
#endif
#endif
#endif  /* !NATIVE_PORT */

#endif  /* _LANGUAGE_C */

#ifndef TRUE
#define TRUE    1
#endif

#ifndef FALSE
#define FALSE   0
#endif

#ifndef NULL
#define NULL    0
#endif

#endif  /* _ULTRATYPES_H_ */
