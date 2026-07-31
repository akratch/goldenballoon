/**
 * dkr_native_ptr.h — LP64 ROM-overlaid-struct pointer slots (PLAN decision 8).
 *
 * DKR casts several of its C structs directly onto ROM/asset bytes and reads
 * their fields in place (LevelHeader, TextureHeader, TextureInfo, ObjectModel,
 * LevelModel(+segments), ObjectHeader, …). On the N64 those structs' embedded
 * pointer/offset fields are 4 bytes; on a 64-bit host a real C pointer is 8
 * bytes, which inflates the struct and shifts every field after the first
 * pointer off the on-disk layout that asset_swap normalizes — so any read past
 * that point is garbage.
 *
 * Fix: under NATIVE_PORT those embedded pointer fields become 4-byte `dkrptr32`
 * token slots, preserving the exact on-disk layout. A token holds the low 32
 * bits of an arena host pointer (the RDRAM stand-in is size-aligned, so the high
 * bits are constant — see dkr_lo32_to_ptr / g_dkrArenaHi). Derefs reconstruct
 * the full host pointer with DKR_PTR(); patch sites (offset→pointer) store the
 * truncated token with DKR_TOK().
 *
 * This header is included from structs.h ONLY under NATIVE_PORT; the N64 build
 * keeps the original real-pointer struct fields untouched.
 */
#ifndef DKR_NATIVE_PTR_H
#define DKR_NATIVE_PTR_H

#include "types.h"    /* u32 */

#ifdef NATIVE_PORT

#include <stddef.h>   /* offsetof */
#include <stdint.h>   /* uintptr_t */

/* A 4-byte on-disk/N64 pointer-or-offset slot. Same width+alignment as the N64
 * field it replaces, so the overlaid struct keeps its exact on-disk layout. */
typedef u32 dkrptr32;

/* Reconstruct a full host pointer from a 32-bit arena token (stubs_dkr.c). */
void *dkr_lo32_to_ptr(u32 lo32);

/* RDRAM stand-in arena bounds (stubs_dkr.c) — for native-only sanity checks. */
extern void    *g_dkrArenaBase;
extern u32      g_dkrArenaSize;

/* Typed deref of a dkrptr32 slot: DKR_PTR(Vertex, model->vertices)[i]. NULL/0
 * token maps to NULL. */
#define DKR_PTR(T, x)   ((T *) dkr_lo32_to_ptr((u32)(x)))

/* Truncate a host pointer to its arena low-32 token for storage in a dkrptr32
 * slot (round-trips through DKR_PTR by construction; see g_dkrArenaHi). */
#define DKR_TOK(p)      ((dkrptr32)(uintptr_t)(p))

/* Native-only struct-layout locks. Guarded so the N64 build never sees them. */
#define DKR_ASSERT_OFFSET(S, F, O) \
    _Static_assert(offsetof(S, F) == (O), #S "." #F " must be at offset " #O)
#define DKR_ASSERT_SIZE(S, N) \
    _Static_assert(sizeof(S) == (N), #S " must be " #N " bytes on disk")

#else /* !NATIVE_PORT — original N64 build: fields stay real pointers */

/* On N64 an embedded pointer field is a real pointer, so DKR_PTR/DKR_TOK reduce
 * to plain casts and the site source is identical to the original decomp. */
#define DKR_PTR(T, x)   ((T *) (x))
#define DKR_TOK(p)      (p)

#endif /* NATIVE_PORT */

#endif /* DKR_NATIVE_PTR_H */
