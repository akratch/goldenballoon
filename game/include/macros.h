#ifndef _MACROS_H_
#define _MACROS_H_

#include <PRinternal/macros.h>
#include "types.h"

#ifndef __sgi
#define GLOBAL_ASM(...)
#endif

#if !defined(__sgi) && (!defined(NON_MATCHING) || !defined(AVOID_UB))
// asm-process isn't supported outside of IDO, and undefined behavior causes
// crashes.
// #error Matching build is only possible on IDO; please build with NON_MATCHING=1.
#endif

#define ARRAY_COUNT(arr) (s32)(sizeof(arr) / sizeof(arr[0]))

#define GLUE(a, b) a ## b
#define GLUE2(a, b) GLUE(a, b)

// Avoid undefined behaviour for non-returning functions
#ifdef __GNUC__
#define NORETURN __attribute__((noreturn))
#else
#define NORETURN
#endif

// Static assertions
#ifdef __GNUC__
#define STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
#define STATIC_ASSERT(cond, msg) typedef char GLUE2(static_assertion_failed, __LINE__)[(cond) ? 1 : -1]
#endif

// Align to 8-byte boundary for DMA requirements
#ifdef __GNUC__
#define ALIGNED8 __attribute__((aligned(8)))
#else
#define ALIGNED8
#endif

// Align to 16-byte boundary for audio lib requirements
#ifdef __GNUC__
#define ALIGNED16 __attribute__((aligned(16)))
#else
#define ALIGNED16
#endif

// Use built-in pseudocode where possible in NON_MATCHING builds
#if defined(__GNUC__) && !defined(CC_CHECK)
#define abs(x)      __builtin_abs(x)
#define fabsf(x)    __builtin_fabsf(x)
#define sqrtf(f)    __builtin_sqrtf(f)
#undef ABS
#undef ABSF
#define ABS(x)      __builtin_abs(x)
#define ABSF(x)     __builtin_fabsf(x)
#else
#define inline
#define ABSF(x) (x < 0.f ? -x : x)
#define ABS(x) ((x) >= 0 ? (x) : -(x))
#endif


// convert a virtual address to physical.
#define VIRTUAL_TO_PHYSICAL(addr)   ((uintptr_t)(addr) & 0x1FFFFFFF)

// convert a physical address to virtual.
#define PHYSICAL_TO_VIRTUAL(addr)   ((uintptr_t)(addr) | 0x80000000)

// another way of converting virtual to physical
#define VIRTUAL_TO_PHYSICAL2(addr)  ((u8 *)(addr) - 0x80000000U)

// Used to suppress warnings in the ./generate_ctx.sh script.
#define INCONSISTENT 

// Used to make a u32 colour value look clearer. Transforms 0xFF0000FF to 255, 0, 0, 255.
// Cast and mask before shifting: the default integer promotions otherwise make
// 255 << 24 overflow signed int, which is undefined on every native target.
#define COLOUR_RGBA32(r, g, b, a) \
    ((((u32) (r) & 0xFFu) << 24) | (((u32) (g) & 0xFFu) << 16) | \
     (((u32) (b) & 0xFFu) << 8) | ((u32) (a) & 0xFFu))

// A few systems in the game use an array as a cache table. This gives you the asset ID
#define ASSETCACHE_ID(x)    ((x << 1) + 0)
// A few systems in the game use an array as a cache table. This gives you the pointer to the asset
#define ASSETCACHE_PTR(x)   ((x << 1) + 1)

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/*
 * DKR_SHL32(x, n) -- a 32-bit left shift with MIPS `sllv` semantics.
 *
 * Several progress-flag sites in the ROM are written as `FLAG << (i + 31)`,
 * because MIPS `sllv`/`sll` take the shift amount from the low FIVE bits of the
 * register: `<< 32` is `<< 0`, `<< 33` is `<< 1`, and so on. That is what the
 * original code means, and on the N64 it is what the hardware does.
 *
 * In C a shift count >= the operand width is UNDEFINED BEHAVIOUR, and these
 * sites are exactly the shape an optimiser can see through: the surrounding
 * comparisons pin the index to a small range (e.g. `world > 0 && world < 5`), so
 * clang can prove the count is always >= 32, treat the result as poison and fold
 * it to 0. It does: measured on the port's own Release build, `8 << (worldId +
 * 31)` in level_load() emits no shift instruction at all. A folded 0 makes
 * `!(flags & 0)` always true and `flags |= 0` a no-op, so the guarded cutscene
 * replays for ever and is never recorded -- see docs/OPEN_ITEMS.md wave
 * "keyshift". At -O0 the same source is correct, because clang then emits a real
 * register shift and arm64/wasm mask the count modulo 32 like MIPS.
 *
 * NATIVE_PORT only. Under the ROM toolchain the expression is left exactly as
 * the decomp writes it, so a matching build is unaffected; the port makes the
 * masking explicit so the meaning no longer depends on UB surviving -O2.
 */
#if defined(NATIVE_PORT) && !defined(MDKR_SHL32_CONTROL)
#define DKR_SHL32(x, n) ((u32) (x) << ((u32) (n) & 31))
#else
/* MDKR_SHL32_CONTROL: the reverted form, for the both-direction control run in
 * tests/check_key_cutscene_once.py. Never define this in a shipping build. */
#define DKR_SHL32(x, n) ((x) << (n))
#endif

/*
 * Convert through the N64's 32-bit integer result and keep the low 16 bits.
 * C's direct float-to-s16 conversion is undefined when the value is outside
 * s16, even though the original MIPS sequence deliberately consumed the low
 * halfword. Invalid/non-finite 32-bit conversions use the MIPS indefinite
 * result, whose low halfword is zero.
 */
static inline s16 dkr_f32_to_s16_wrap(f32 value) {
    s32 whole;
    u16 bits;

    if (value != value || value >= 2147483648.0f || value < -2147483648.0f) {
        return 0;
    }
    whole = (s32)value;
    bits = (u16)(u32)whole;
    if (bits <= 0x7FFFu) {
        return (s16)bits;
    }
    return (s16)((s32)bits - 0x10000);
}

static inline s16 dkr_s16_from_bits(u16 bits) {
    if (bits <= 0x7FFFu) {
        return (s16)bits;
    }
    return (s16)((s32)bits - 0x10000);
}

#endif
