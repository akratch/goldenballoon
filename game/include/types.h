#ifndef _TYPES_H_
#define _TYPES_H_

#include <PR/ultratypes.h>

typedef float MtxF[4][4];
typedef s32 MtxS[4][4];
typedef s16 VertexList;
typedef u8 TriangleList;
#if defined(NATIVE_PORT)
#include <stdint.h>   /* real 64-bit uintptr_t so pointer round-trips are lossless */
#else
typedef u32 uintptr_t;
#endif

#if defined(__sgi)
#define stubbed_printf
#else
#define stubbed_printf(...)
#endif

#endif
