#ifndef _COLLISION_H_
#define _COLLISION_H_

#include "types.h"
#include "structs.h"

#define MAX_COLLISION_CANDIDATES 500

#ifdef NATIVE_PORT
/*
 * LP64 collision-candidate packing.
 *
 * The candidate list (s32 *gCollisionCandidates) mixes two kinds of entry: a
 * terrain LevelModelSegment and a CollisionFacetPlanes (triangle). On the N64
 * the encoder packed the segment as its PHYSICAL address (a small POSITIVE s32,
 * via K0_TO_PHYS) and the facet as its raw K0 pointer (0x80xxxxxx, NEGATIVE as
 * s32); the decoders tell them apart by SIGN and rebuild the segment with
 * PHYS_TO_K0. Neither survives on a 64-bit host: the arena lives above 4 GB, so
 * K0_TO_PHYS/truncation discards the high pointer bits, and a truncated arena
 * pointer's bit-31 (the "type" sign) is ASLR-dependent — so a facet could read
 * back as a segment and crash.
 *
 * Native encoding: store the 24-bit ARENA OFFSET (the arena is 16 MB, so offsets
 * fit well under 2^31) and use bit 31 as the type tag — 0 = segment (stays
 * POSITIVE, satisfying the decoders' `>= 0` / `> 0` segment test), 1 = facet
 * (NEGATIVE). Both are reconstructed against the real arena base. This keeps the
 * sign-based dispatch in resolve_collisions() and the tracks.c camera-clip code
 * working unchanged.
 */
#define DKR_COLL_ENCODE_SEG(seg)   ((s32) ((uintptr_t) (seg) - (uintptr_t) g_dkrArenaBase))
#define DKR_COLL_ENCODE_FACET(f)   ((s32) (((uintptr_t) (f) - (uintptr_t) g_dkrArenaBase) | 0x80000000u))
#define DKR_COLL_DECODE_SEG(c)     ((char *) g_dkrArenaBase + (u32) (c))
#define DKR_COLL_DECODE_FACET(c)   ((char *) g_dkrArenaBase + ((u32) (c) & 0x7FFFFFFFu))
#endif

enum CollisionModes { COLLISION_MODE_DEFAULT, COLLISION_MODE_1, COLLISION_MODE_NO_WALLS };

void generate_collision_candidates(s32 numPoints, Vec3f *origins, Vec3f *targets, s32 vehicleID);
s32 compute_grid_overlap_mask(LevelModelSegmentBoundingBox *bbox, s32 x1, s32 z1, s32 x2, s32 z2);
s32 resolve_collisions(Vec3f *origin, Vec3f *target, f32 *radius, s8 *surface, s32 numEntries, s32 *numCollisions);

#endif
