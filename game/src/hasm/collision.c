#include "collision.h"

#include "macros.h"
#include "PR/R4300.h"
#include "textures_sprites.h"
#include "types.h"

/*******************************/

extern LevelModel *gCurrentLevelModel;

extern f32 gCollisionNormalX;
extern f32 gCollisionNormalY;
extern f32 gCollisionNormalZ;
extern s32 gHitWall;
extern s32 gCollisionMode;

extern s32 *gCollisionCandidates;
extern s8 *gCollisionSurfaces;
extern s32 gNumCollisionCandidates;

#ifdef NATIVE_PORT
/* platform/stubs_dkr.c -- see the comments at each use site. Declared as plain
 * externs (not via a platform header) because this translation unit includes the
 * N64 SDK headers, whose <PR/os_libc.h> conflicts with libc's. */
extern int  mdkr_gridmask_legacy(void);
extern void mdkr_coll_candidates(int count, int truncated);
/* The effective candidate cap: MAX_COLLISION_CANDIDATES unless MDKR_COLLCAP
 * lowers it (to reach the boundary) or sets `legacy` (to remove the guards). */
extern int  mdkr_coll_cap(int romCap);
/* platform/hasm_stubs_temp.c -- the untextured-batch A/B toggle and its counter. */
extern int  mdkr_colltex_legacy(void);
extern int  mdkr_colltex_force(void);
extern void mdkr_coll_untextured(void);
#endif

// All handwritten assembly, below.

#ifdef NON_MATCHING
/**
 * Populates a list of triangle candidates that will be tested for collision in resolve_collisions.
 * Calculates a single bounding rectangle that contains all origin and target points.
 * Then, terrain segments overlapping this rectangle are identified, and eligible triangles within them are selected.
 * Triangle batches are filtered based on visibility, surface type, and collision flags.
 */
void generate_collision_candidates(s32 numPoints, Vec3f *origins, Vec3f *targets, s32 vehicleID) {
    s32 minX, maxX, minZ, maxZ;
    s32 counter;
    LevelModelSegment *segments[10];
    s16 grid_mask[10];
    s32 i, j, batchIndex, triIndex;
    s32 surface;
    s32 untextured;

    minX = 100000;
    maxX = -100000;
    minZ = 100000;
    maxZ = -100000;

    // Calculate a single bounding rectangle containing all origin and target points
    for (i = 0; i < numPoints; i++) {
        s32 x1 = origins[i].x;
        s32 z1 = origins[i].z;
        s32 x2 = targets[i].x;
        s32 z2 = targets[i].z;

        if (maxX < x1) {
            maxX = x1;
        }
        if (minX > x1) {
            minX = x1;
        }

        if (maxZ < z1) {
            maxZ = z1;
        }
        if (minZ > z1) {
            minZ = z1;
        }

        if (maxX < x2) {
            maxX = x2;
        }
        if (minX > x2) {
            minX = x2;
        }

        if (maxZ < z2) {
            maxZ = z2;
        }
        if (minZ > z2) {
            minZ = z2;
        }
    }

    // Add padding to ensure slight overlap with nearby geometry
    minX -= 20;
    maxX += 20;
    minZ -= 20;
    maxZ += 20;

    // Swap values if necessary (should never happen)
    if (maxX < minX) {
        s32 temp;

        temp = minX;
        minX = maxX;
        maxX = temp;
    }
    if (maxZ < minZ) {
        s32 temp;

        temp = minZ;
        minZ = maxZ;
        maxZ = temp;
    }

    // Select up to 10 terrain segments whose bounding boxes overlap the search rectangle (with 5 units tolerance)
    counter = 0;
    for (i = 0; i < gCurrentLevelModel->numberOfSegments; i++) {
        if (minX <= DKR_PTR(LevelModelSegmentBoundingBox, gCurrentLevelModel->segmentsBoundingBoxes)[i].x2 + 5 &&
            maxX >= DKR_PTR(LevelModelSegmentBoundingBox, gCurrentLevelModel->segmentsBoundingBoxes)[i].x1 - 5 &&
            minZ <= DKR_PTR(LevelModelSegmentBoundingBox, gCurrentLevelModel->segmentsBoundingBoxes)[i].z2 + 5 &&
            maxZ >= DKR_PTR(LevelModelSegmentBoundingBox, gCurrentLevelModel->segmentsBoundingBoxes)[i].z1 - 5) {

            grid_mask[counter] =
                compute_grid_overlap_mask(&DKR_PTR(LevelModelSegmentBoundingBox, gCurrentLevelModel->segmentsBoundingBoxes)[i], minX, minZ, maxX, maxZ);
            segments[counter] = &DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[i];
            counter++;
            if (counter == 10) {
                break;
            }
        }
    }

    // Populate the list of collision candidates
    j = 0;
    for (i = 0; i < counter; i++) {
        LevelModelSegment *seg = segments[i];

#ifdef NATIVE_PORT
        /* NATIVE_PORT: the cap must be checked BEFORE this insert, not only after
         * the facet insert below, because THIS insert is the one that can step
         * over it.
         *
         * The ROM increments $s3 here with no test at all --
         * `generate_collision_candidates`, game/src/hasm/collision.s .L80031368:
         *      8003137C  sw    $v0, 0x0($s2)      # gCollisionCandidates[j] = seg
         *      80031388  addiu $s3, $s3, 0x1      # j++          <-- NO cap test
         * and the only cap test in the function guards the FACET insert:
         *      80031464  addiu $at, $zero, 0x1F4  # 500
         *      80031470  beq   $s3, $at, .L800314A8
         * So enter this loop body with j == 499, write slot 499, and j becomes 500
         * without exiting. The facet insert then writes slot 500 -- one past the
         * `mempool_alloc_safe(MAX_COLLISION_CANDIDATES * 4)` allocation in
         * tracks.c -- makes j 501, and `j == 500` is never true again, so the
         * overrun continues for every remaining triangle of every remaining
         * segment. An EQUALITY test against an insert that can skip the boundary
         * is unrecoverable by construction.
         *
         * This is a host adaptation, not a transcription correction: the assembly
         * really has no guard here, so it is gated. It cannot change behaviour
         * before the boundary -- j is unchanged on every path where the ROM does
         * not already write out of bounds -- and at the boundary it stops with
         * gNumCollisionCandidates == MAX_COLLISION_CANDIDATES, which is exactly
         * what the ROM's own facet-side test produces. What it removes is only
         * the out-of-bounds writes.
         *
         * NOT REACHED as of this commit: measured peak 416 of 500 (boss levels 41
         * and 54, 13000 frames, autopilot), 0 truncations across all 20 main and
         * all 10 boss tracks plus the Adventure hub and the attract demo. It is in
         * because 416 is only 84 short, because the truncation itself was reached
         * before the grid-mask fix (73 truncations on level 38), and because a
         * saturating list is what threw the ground away and dropped racers through
         * Tricky's volcano. MDKR_COLLCAP=<n> lowers the cap so a check can reach
         * the boundary, and MDKR_COLLCAP=legacy restores the unguarded form.
         */
        if (j >= mdkr_coll_cap(MAX_COLLISION_CANDIDATES)) {
            goto out;
        }
#endif
        // Insert the segment pointer encoded with MSB = 0 to differentiate from collision facets
#ifdef NATIVE_PORT
        gCollisionCandidates[j] = DKR_COLL_ENCODE_SEG(seg);
#else
        gCollisionCandidates[j] = (s32) K0_TO_PHYS(seg);
#endif
        j++;

        for (batchIndex = 0; batchIndex < seg->numberOfBatches; batchIndex++) {
            // Skip batches marked as non-collidable
            if (DKR_PTR(TriangleBatchInfo, seg->batches)[batchIndex].flags & RENDER_NO_COLLISION) {
                continue;
            }

            // Skip hidden surfaces for non-vehicle objects (e.g., projectiles, pickups)
            if (vehicleID == -1 && (DKR_PTR(TriangleBatchInfo, seg->batches)[batchIndex].flags & RENDER_HIDDEN)) {
                continue;
            }

            /*
             * An untextured batch is COLLIDABLE, with SURFACE_DEFAULT -- it is not
             * skipped.
             *
             * The upstream NON_MATCHING body did `continue` here, which drops the
             * batch's triangles from the candidate list altogether and makes
             * untextured level geometry non-solid. The ROM instead zeroes the
             * surface and falls into the triangle loop.
             * `generate_collision_candidates`, game/src/hasm/collision.s:
             *      800313D0  addiu $at, $zero, 0xFF
             *      800313D4  or    $t2, $zero, $zero   # surface = SURFACE_DEFAULT
             *      800313D8  beql  $v1, $at, .L8003141C
             *      800313DC   lh   $t0, 0x4($t5)       # (branch-likely delay slot)
             *      800313E0  sll   $v1, $v1, 3         # else &textures[textureIndex]
             *      800313E4  add   $v1, $v1, $a0
             *      800313E8  lb    $t2, 0x7($v1)       #      surface = surfaceType
             * and `.L8003141C` (collision.s:234) is the TRIANGLE-LOOP setup
             * (`lh $t1, 0x4($t4)` ...), *not* the batch-skip labels `.L80031488` /
             * `.L8003148C` that the four genuine `continue`s in this loop branch to.
             *
             * The `or $t2, $zero, $zero` at 800313D4 is the decisive evidence, and
             * it is independent of reading the branch target: if 0xFF meant "skip
             * the batch" that instruction would be dead, because every path that
             * falls through overwrites $t2 at 800313E8. It exists only to give the
             * untextured case SURFACE_DEFAULT.
             *
             * Mirroring the ROM's structure, the three surface tests below are
             * bypassed on this path rather than run against 0. That is
             * behaviour-identical -- SURFACE_DEFAULT is 0 while WATER_CALM is 11,
             * INVIS_WALL 17 and UNK12 18 (game/include/enums.h:153) -- and kept
             * this way so the control flow reads the same as the assembly.
             *
             * Like the grid-mask fix above this is a transcription correction, not
             * a host adaptation, so it carries no NATIVE_PORT gate.
             */
            untextured = (DKR_PTR(TriangleBatchInfo, seg->batches)[batchIndex].textureIndex == 255);
#ifdef NATIVE_PORT
            /* TEST HOOKS, all no-ops unless set.
             *   MDKR_COLLTEX=legacy  restores the `continue`, so both arms come
             *                        from one binary.
             *   MDKR_COLLTEX_FORCE=1 treats EVERY batch as untextured. No measured
             *                        level reaches the real case (0 across all 20
             *                        main and all 10 boss tracks), so without this
             *                        the two arms are necessarily identical and a
             *                        check comparing them would prove nothing.
             *                        Forcing it makes the mechanism observable:
             *                        the legacy arm then skips every batch and
             *                        generates almost no candidates at all. */
            if (mdkr_colltex_force()) {
                untextured = TRUE;
            }
            if (untextured) {
                mdkr_coll_untextured();
            }
#endif
            if (untextured) {
#ifdef NATIVE_PORT
                if (mdkr_colltex_legacy()) {
                    continue;
                }
#endif
                surface = SURFACE_DEFAULT;
            } else {
                // Skip surfaces that are non-solid or selectively passable depending on the vehicle type:
                // - SURFACE_WATER_CALM is passable by all objects (treated like water surface).
                // - SURFACE_INVIS_WALL can be passed through only by planes.
                // - SURFACE_UNK12 is impassable only for cars, but passable by other vehicle types.
                surface = DKR_PTR(TextureInfo, gCurrentLevelModel->textures)[DKR_PTR(TriangleBatchInfo, seg->batches)[batchIndex].textureIndex].surfaceType;
                if (surface == SURFACE_WATER_CALM || (vehicleID == VEHICLE_PLANE && surface == SURFACE_INVIS_WALL) ||
                    (vehicleID != VEHICLE_CAR && surface == SURFACE_UNK12)) {
                    continue;
                }
            }

            // Iterate over triangles in the batch and add eligible ones based on grid mask intersection
            for (triIndex = DKR_PTR(TriangleBatchInfo, seg->batches)[batchIndex].facesOffset; triIndex < DKR_PTR(TriangleBatchInfo, seg->batches)[batchIndex + 1].facesOffset;
                 triIndex++) {
                CollisionFacetPlanes *facet = &DKR_PTR(CollisionFacetPlanes, seg->collisionFacets)[triIndex];

                // Check if the triangle's grid mask intersects with the bounding rectangle's grid mask
                if (((DKR_PTR(s16, seg->unk10)[triIndex] & grid_mask[i]) & 0x00FF) == 0 ||
                    ((DKR_PTR(s16, seg->unk10)[triIndex] & grid_mask[i]) & 0xFF00) == 0) {
                    continue;
                }

#ifdef NATIVE_PORT
                gCollisionCandidates[j] = DKR_COLL_ENCODE_FACET(facet);
#else
                gCollisionCandidates[j] = (s32) facet;
#endif
#ifdef NATIVE_PORT
                /* NATIVE_PORT: the same pre-check as at the segment insert above,
                 * for the same reason. The ROM's post-increment equality test
                 * below is left exactly as it transcribes -- with this guard in
                 * place j can never exceed the cap, so `==` and `>=` are the same
                 * test, and keeping the ROM's form keeps the diff to an addition.
                 * Without a guard HERE, the segment insert can still hand this
                 * insert a j that is already AT the cap. */
                if (j >= mdkr_coll_cap(MAX_COLLISION_CANDIDATES)) {
                    goto out;
                }
#endif
                gCollisionSurfaces[j] = surface;
                j++;

                if (j == MAX_COLLISION_CANDIDATES) {
                    goto out;
                }
            }
        }
    }

out:
    gNumCollisionCandidates = j;
#ifdef NATIVE_PORT
    /* Publish the candidate-list high-water mark and a truncation count. This is
     * the MECHANISM behind the "racer falls through the level" defect above --
     * a saturated list silently discards the tail, ground included -- so a
     * headless check can assert on it directly instead of only on the fall. */
    mdkr_coll_candidates(j, j >= mdkr_coll_cap(MAX_COLLISION_CANDIDATES));
#endif
}
#else
GLOBAL_ASM("asm/collision/generate_collision_candidates.s")
#endif

#ifdef NON_MATCHING
/**
 * Computes the overlap between a rectangle and a segment's bounding box, returning a 16-bit grid mask.
 * The bounding box is divided into an 8x8 grid. The function determines which grid columns (X) and rows (Z)
 * the input rectangle intersects.
 * The resulting 16-bit mask encodes this information:
 * - lower 8 bits represent intersections along the X-axis (1 bit per column),
 * - upper 8 bits represent intersections along the Z-axis (1 bit per row).
 */
s32 compute_grid_overlap_mask(LevelModelSegmentBoundingBox *bbox, s32 x1, s32 z1, s32 x2, s32 z2) {
    s32 mask = 0;
    s32 v1;
    s32 i;
    s32 cell_width;
    s32 cell_height;
    s32 cell_x, cell_z;
    s32 bbox_x1, bbox_z1, bbox_x2, bbox_z2;

    if (bbox == NULL) {
        return mask;
    }

    bbox_x1 = bbox->x1;
    bbox_z1 = bbox->z1;
    bbox_x2 = bbox->x2;
    bbox_z2 = bbox->z2;

    if (x2 < bbox_x1) {
        x2 = bbox_x1;
    }

    if (x1 < bbox_x1) {
        x1 = bbox_x1;
    }

    if (z2 < bbox_z1) {
        z2 = bbox_z1;
    }

    if (z1 < bbox_z1) {
        z1 = bbox_z1;
    }

    if (x2 > bbox_x2) {
        x2 = bbox_x2;
    }

    if (x1 > bbox_x2) {
        x1 = bbox_x2;
    }

    if (z2 > bbox_z2) {
        z2 = bbox_z2;
    }

    if (z1 > bbox_z2) {
        z1 = bbox_z2;
    }

    v1 = 1;

    cell_width = ((bbox_x2 - bbox_x1) >> 3) + 1;
    cell_x = bbox_x1;

    for (i = 0; i < 8; i++) {
        if (cell_x + cell_width >= x1 && x2 >= cell_x) {
            mask |= v1;
        }

        v1 <<= 1;
        cell_x += cell_width;
    }

    cell_height = ((bbox_z2 - bbox_z1) >> 3) + 1;
    cell_z = bbox_z1;

    for (i = 0; i < 8; i++) {
        /*
         * `z2 >= cell_z`, NOT `z2 >= bbox_z1`.
         *
         * The upstream NON_MATCHING body compares the query rectangle's far Z
         * edge against the segment bounding box's NEAR edge, which the clamping
         * above has already guaranteed (`if (z2 < bbox_z1) z2 = bbox_z1;`) --
         * so the second half of the condition is a tautology and every Z row
         * from the first accepted one through row 7 gets set. The Z half of the
         * mask is then useless as a filter.
         *
         * The ROM tests the per-row value, exactly mirroring the X loop above.
         * `compute_grid_overlap_mask`, 0x800315D0:
         *      slt   $at, $t5, $t0     # at = (z2 < cell_z)
         *      bnel  $at, $zero, ...   # skip this row if so
         * with $t5 = the clamped z2 and $t0 = cell_z, initialised to bbox_z1 at
         * 0x800315C4 (`or $t0, $t1, $zero`) and advanced by cell_height in the
         * loop's branch delay slot at 0x800315F4 (`add $t0, $t0, $t2`) -- i.e.
         * the same rolling per-row value the X loop uses in $t0 at 0x80031590.
         *
         * Why it matters, and why it is silent: the mask is only a coarse
         * pre-filter, so an over-permissive one still produces CORRECT
         * collisions -- just far too many candidates. Every extra triangle that
         * passes consumes a slot in the 500-entry gCollisionCandidates list, and
         * generate_collision_candidates() TRUNCATES at MAX_COLLISION_CANDIDATES
         * (`goto out`). Where the truncated tail happens to hold the facets a
         * racer is standing on, the ground silently ceases to exist and the
         * racer falls through the level. See docs/OPEN_ITEMS.md (wave
         * "gridmask") and tests/check_collision_gridmask.py.
         */
        s32 zRowNear = cell_z;
#ifdef NATIVE_PORT
        /* TEST HOOK -- MDKR_GRIDMASK=off reproduces the tautology above exactly,
         * so tests/check_collision_gridmask.py can render both arms from one
         * binary and prove the broken one really does drop the ground. No-op
         * unless set (same contract as MDKR_NEARCLIP / MDKR_LINESWAP). */
        if (mdkr_gridmask_legacy()) {
            zRowNear = bbox_z1;
        }
#endif
        if (cell_z + cell_height >= z1 && z2 >= zRowNear) {
            mask |= v1;
        }

        v1 <<= 1;
        cell_z += cell_height;
    }

    return mask;
}
#else
GLOBAL_ASM("asm/collision/compute_grid_overlap_mask.s")
#endif

#ifdef NON_MATCHING
/**
 * Resolves collisions for a list of moving objects against previously identified collision candidates.
 * For each object, the function checks whether the path from `origin` to `target` intersects any collision plane.
 * If so, it adjusts the `target` position to resolve the collision, updates surface type, and sets output flags.
 * Returns a bitmask indicating which objects experienced a collision.
 */
s32 resolve_collisions(Vec3f *origin, Vec3f *target, f32 *radius, s8 *surface, s32 numEntries, s32 *numCollisions) {
    s32 i, j;
    f32 *collisionPlanes;
    f32 A, B, C, D;
    f32 A1, B1, C1, D1;
    CollisionFacetPlanes *facet;
    f32 targetDist;
    f32 originDist;
    f32 diffX, diffY, diffZ;
    f32 outX, outY, outZ;
    f32 intersectionFactor;
    f32 intersectionPointX, intersectionPointY, intersectionPointZ;
    s32 insideTriangle;
    u8 bitMask;
    s8 collisionMask;

    bitMask = 1;
    collisionMask = 0;
    gHitWall = FALSE;

    if (gNumCollisionCandidates == 0) {
        return 0;
    }

    // Step 1: forward collision resolution
    while (numEntries > 0) {
        s32 numCollidedSurfaces = 0;
        s32 continueSearch;

        do {
            continueSearch = FALSE;
            collisionPlanes = NULL;

            for (i = 0; i < gNumCollisionCandidates; i++) {
                if (gCollisionCandidates[i] >= 0) {
#ifdef NATIVE_PORT
                    collisionPlanes = DKR_PTR(f32, ((LevelModelSegment *) DKR_COLL_DECODE_SEG(gCollisionCandidates[i]))->collisionPlanes);
#else
                    collisionPlanes = DKR_PTR(f32, ((LevelModelSegment *) (PHYS_TO_K0(gCollisionCandidates[i])))->collisionPlanes);
#endif
                } else {
                    /*
                     * Candidate streams are authored as one segment anchor
                     * followed by its facets. Reject an orphan facet instead
                     * of reusing an old/uninitialized plane table.
                     */
                    if (collisionPlanes == NULL) {
                        continue;
                    }
#ifdef NATIVE_PORT
                    facet = (CollisionFacetPlanes *) DKR_COLL_DECODE_FACET(gCollisionCandidates[i]);
#else
                    facet = (CollisionFacetPlanes *) gCollisionCandidates[i];
#endif
                    A = collisionPlanes[facet->basePlaneIndex * 4 + 0];
                    B = collisionPlanes[facet->basePlaneIndex * 4 + 1];
                    C = collisionPlanes[facet->basePlaneIndex * 4 + 2];
                    D = collisionPlanes[facet->basePlaneIndex * 4 + 3];

                    targetDist = (target->x * A + target->y * B + target->z * C + D) - *radius;
                    originDist = (origin->x * A + origin->y * B + origin->z * C + D) - *radius;

                    // Process only one-sided collisions (above -> below plane), with tolerance
                    if (targetDist < -0.1 && originDist >= -0.1) {
                        diffX = target->x - origin->x;
                        diffY = target->y - origin->y;
                        diffZ = target->z - origin->z;

                        if (targetDist != originDist) {
                            intersectionFactor = originDist / (originDist - targetDist);
                        } else {
                            intersectionFactor = 0.0f;
                        }

                        intersectionPointX = origin->x + diffX * intersectionFactor;
                        intersectionPointY = origin->y + diffY * intersectionFactor;
                        intersectionPointZ = origin->z + diffZ * intersectionFactor;

                        // Check if intersection point lies within triangle bounds
                        insideTriangle = TRUE;
                        for (j = 0; j < 3 && insideTriangle; j++) {
                            s32 flipSide = FALSE;
                            s32 edgeBisectorPlane = facet->edgeBisectorPlane[j];
                            f32 dist;

                            if (edgeBisectorPlane & 0x8000) {
                                flipSide = TRUE;
                                edgeBisectorPlane &= 0x7FFF;
                            }

                            A1 = collisionPlanes[edgeBisectorPlane * 4 + 0];
                            B1 = collisionPlanes[edgeBisectorPlane * 4 + 1];
                            C1 = collisionPlanes[edgeBisectorPlane * 4 + 2];
                            D1 = collisionPlanes[edgeBisectorPlane * 4 + 3];

                            dist = intersectionPointX * A1 + intersectionPointY * B1 + intersectionPointZ * C1 + D1;
                            if (flipSide) {
                                dist = -dist;
                            }

                            if (dist >= 4.0f) {
                                insideTriangle = FALSE;
                            }
                        }

                        if (insideTriangle) {
                            if (B >= 0.707f && gCollisionSurfaces[i] != SURFACE_STONE &&
                                gCollisionMode == COLLISION_MODE_DEFAULT) {
                                // Push up vertically if slope is gentle and not stone
                                outX = target->x;
                                outY = (*radius - (target->x * A + target->z * C + D)) / B;
                                outZ = target->z;
                            } else {
                                if (B < 0.45 && gCollisionMode != COLLISION_MODE_NO_WALLS) {
                                    // Wall-like surface: store collision normal
                                    gHitWall = TRUE;
                                    gCollisionNormalX = A;
                                    gCollisionNormalY = B;
                                    gCollisionNormalZ = C;
                                }

                                // Push out along surface normal
                                outX = target->x - A * targetDist;
                                outY = target->y - B * targetDist;
                                outZ = target->z - C * targetDist;
                            }

                            if (!gHitWall) {
                                gCollisionNormalX = A;
                                gCollisionNormalY = B;
                                gCollisionNormalZ = C;
                            }

                            // Store surface type with highest priority (higher ID wins)
                            if (*surface < gCollisionSurfaces[i]) {
                                *surface = gCollisionSurfaces[i];
                            }

                            continueSearch = TRUE;
                            numCollidedSurfaces++;
                            if (numCollidedSurfaces > 10) {
                                continueSearch = FALSE;
                                outX = origin->x;
                                outY = origin->y;
                                outZ = origin->z;
                            }

                            // Update target and recheck collisions
                            target->x = outX;
                            target->y = outY;
                            target->z = outZ;
                            break;
                        }
                    }
                }
            }
        } while (continueSearch);

        if (numCollidedSurfaces != 0) {
            (*numCollisions)++;
            collisionMask |= bitMask;
        }
        bitMask <<= 1;

        // Step 2: ensure object is fully pushed out from underneath
        numCollidedSurfaces = 0;
        do {
            continueSearch = FALSE;
            collisionPlanes = NULL;

            for (i = 0; i < gNumCollisionCandidates; i++) {
                if (gCollisionCandidates[i] >= 0) {
#ifdef NATIVE_PORT
                    collisionPlanes = DKR_PTR(f32, ((LevelModelSegment *) DKR_COLL_DECODE_SEG(gCollisionCandidates[i]))->collisionPlanes);
#else
                    collisionPlanes = DKR_PTR(f32, ((LevelModelSegment *) (PHYS_TO_K0(gCollisionCandidates[i])))->collisionPlanes);
#endif
                } else {
                    if (collisionPlanes == NULL) {
                        continue;
                    }
#ifdef NATIVE_PORT
                    facet = (CollisionFacetPlanes *) DKR_COLL_DECODE_FACET(gCollisionCandidates[i]);
#else
                    facet = (CollisionFacetPlanes *) gCollisionCandidates[i];
#endif
                    A = collisionPlanes[facet->basePlaneIndex * 4 + 0];
                    B = collisionPlanes[facet->basePlaneIndex * 4 + 1];
                    C = collisionPlanes[facet->basePlaneIndex * 4 + 2];
                    D = collisionPlanes[facet->basePlaneIndex * 4 + 3];

                    targetDist = (target->x * A + target->y * B + target->z * C + D) - *radius;
                    if (targetDist < -0.1 && targetDist > -(*radius + 3.0f)) {
                        insideTriangle = TRUE;
                        for (j = 0; j < 3 && insideTriangle; j++) {
                            s32 flipSide = FALSE;
                            s32 edgePlaneIndex = facet->edgeBisectorPlane[j];
                            f32 dist;

                            if (edgePlaneIndex & 0x8000) {
                                flipSide = TRUE;
                                edgePlaneIndex &= 0x7FFF;
                            }

                            A1 = collisionPlanes[edgePlaneIndex * 4 + 0];
                            B1 = collisionPlanes[edgePlaneIndex * 4 + 1];
                            C1 = collisionPlanes[edgePlaneIndex * 4 + 2];
                            D1 = collisionPlanes[edgePlaneIndex * 4 + 3];

                            dist = target->x * A1 + target->y * B1 + target->z * C1 + D1;
                            if (flipSide) {
                                dist = -dist;
                            }

                            if (dist >= 4.0f) {
                                insideTriangle = FALSE;
                            }
                        }

                        if (insideTriangle) {
                            outX = target->x - A * targetDist;
                            outY = target->y - B * targetDist;
                            outZ = target->z - C * targetDist;

                            continueSearch = TRUE;
                            numCollidedSurfaces++;
                            if (numCollidedSurfaces > 10) {
                                continueSearch = FALSE;
                                outX = origin->x;
                                outY = origin->y;
                                outZ = origin->z;
                            }

                            target->x = outX;
                            target->y = outY;
                            target->z = outZ;
                            break;
                        }
                    }
                }
            }
        } while (continueSearch);

        // Advance to next entry
        numEntries--;
        origin++;
        target++;
        radius++;
        surface++;
    }

    return collisionMask;
}
#else
GLOBAL_ASM("asm/collision/resolve_collisions.s")
#endif
