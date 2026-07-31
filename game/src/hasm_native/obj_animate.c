/*
 * hasm_native/obj_animate.c
 *
 * Native C reimplementation of obj_animate (0x80061D30), which ships as
 * handwritten MIPS assembly in game/src/hasm/obj_animate.s.
 *
 * obj_animate streams/interpolates an animated object model's vertex positions
 * for the current animation frame and writes the result into the model
 * instance's active (double-buffered) Vertex buffer.  It also interpolates the
 * per-frame model offset (offsetX/Y/Z) and head tilt.
 *
 * ---------------------------------------------------------------------------
 * Animation data layout (per model, animData = model->animations[id].animData):
 *
 *   [0x00 .. 0x0C)                 keyframe-0 offset header (see below)
 *   [0x0C .. 0x0C + nVerts*6)      frame-0 full-precision deltas: one 3xs16
 *                                  (x,y,z) entry per model vertex, indexed by
 *                                  vertex index.
 *   keyframes n>=0, each keyframeSize = nAnimVerts*3 + 12 bytes:
 *       [0 .. nAnimVerts*3)        per-animated-vertex s8 (x,y,z) deltas
 *       [nAnimVerts*3 .. +12)      12-byte offset header
 *   The frame n keyframe is addressed at animData + keyframeSize*(n+2); its
 *   trailing header is therefore at animData + keyframeSize*(n+1) - 0xC (with
 *   the frame-0 header being the special case at animData + 0).
 *
 * The 12-byte offset header holds, as s16 values:
 *   +0x0 offsetX   +0x2 offsetY   +0x4 offsetZ   +0xA headTilt
 *   (bytes 0x6..0xA are unused by this routine).
 *
 * Working buffers (all native-endian, 3xs16 entries indexed by vertex index):
 *   - scratch        = modInst->vertices[2]  (absolute animated positions)
 *   - sInterpScratch = per-call interpolation deltas.  In the original this is
 *     the global scratch pointer D_8011D644 (a 0xC00-byte pool allocation used
 *     ONLY by obj_animate); here it is a file-static buffer of the same size,
 *     which is written in full before it is read within each call.
 *
 * ---------------------------------------------------------------------------
 * Endianness (IMPORTANT):
 *   Per the port's asset-load normalization policy, all in-memory model asset
 *   data is native-endian by the time this runs.  The original N64 code read
 *   the frame-0 s16 delta table and the header s16 fields from raw big-endian
 *   ROM data (the header fields via explicit (hi<<8)|lo byte assembly).  Here
 *   those are natural native s16 reads.  This REQUIRES the asset-load byteswap
 *   step to normalize the animation blob's s16 quantities (the frame-0 delta
 *   table and every keyframe's 12-byte offset header) to native-endian, while
 *   leaving the per-vertex s8 delta bytes untouched (single bytes need no swap).
 *   The base mesh (model->vertices) is already native because the renderer uses
 *   it directly, and the frame-0 deltas are summed with it, so they must share
 *   the same (native) endianness.
 *
 * Fixed-point notes:
 *   - "<<4 / &0xF" split of obj->animFrame: high bits = keyframe index, low 4
 *     bits = sub-keyframe interpolation weight (0..15).
 *   - interpolation term `(delta * weight) >> 4` uses the MIPS multu/srl (low
 *     32 bits, logical shift); because every such result is stored back into an
 *     s16, a logical or arithmetic >>4 yields identical low-16-bit results, but
 *     we keep the faithful (u32)>>4 form.
 */

#include <string.h>
#include "objects.h"

/* Global scratch pool (0xC00 bytes) that the original references as D_8011D644;
 * used exclusively within a single obj_animate call, so a file-static buffer is
 * an exact functional stand-in and sidesteps the N64 32-bit-pointer slot. */
static s16 sInterpScratch[0xC00 / sizeof(s16)];

/* Alignment-safe native-endian s16 read (keyframe headers may be odd-aligned
 * because keyframeSize can be odd). */
static inline s16 read_s16(const void *p) {
    s16 v;
    memcpy(&v, p, sizeof(v));
    return v;
}

s32 obj_animate(Object *obj) {
    ModelInstance *mi;
    ObjectModel *model;
    s32 modelIndex;
    s32 numModelIds;
    s32 animID;
    s32 animFrame;    /* raw frame counter (may be reset to 0 past the end) */
    s32 frameHi;      /* keyframe index = animFrame >> 4 */
    s32 interp;       /* sub-keyframe weight = animFrame & 0xF */
    s32 prevKeyframe; /* keyframe index we are advancing from */
    s32 prevAnimID;
    s32 maxFrame;
    s32 numAnims;
    s32 nVerts;
    s32 nAnimVerts;
    s32 keyframeSize;
    s16 *scratch;
    s16 *indices;
    Vertex *baseVerts;
    u8 *animData;
    s32 i;

    /* Invalid/empty model arrays are malformed content, not a usable frame. */
    if (obj == NULL || obj->header == NULL || obj->modelInstances == NULL) {
        return 0;
    }
    numModelIds = obj->header->numberOfModelIds;
    if (numModelIds <= 0) {
        return 0;
    }

    /* Clamp the model index to the final valid element. */
    modelIndex = obj->modelIndex;
    if (modelIndex < 0) {
        modelIndex = 0;
    }
    if (modelIndex >= numModelIds) {
        modelIndex = numModelIds - 1;
    }

    mi = obj->modelInstances[modelIndex];
    if (mi == NULL || mi->objModel == NULL) {
        return 0;
    }
    model = mi->objModel;
    if (model->animations == 0) {
        return 0;
    }

    animFrame = obj->animFrame;
    animID = obj->animationID;

    /* Nothing changed since the previous update -> skip. */
    if (animFrame == mi->animationFrameCount && animID == mi->animationID) {
        return 0;
    }

    /* Clamp the animation id, and compute the last usable keyframe. */
    if (animID < 0) {
        animID = 0;
    }
    numAnims = model->numberOfAnimations;
    if (animID >= numAnims) {
        animID = numAnims - 1;
    }
    maxFrame = 0;
    if (numAnims > 0) {
        maxFrame = DKR_PTR(ObjectModel_44, model->animations)[animID].animLength - 2;
    }

    frameHi = (s32) (((u32) animFrame) >> 4);
    /* Ran past the end of the animation: snap back to the start. */
    if (maxFrame < frameHi) {
        animFrame = 0;
        frameHi = 0;
        mi->animationID = -1;
    }
    prevAnimID = mi->animationID;

    scratch = (s16 *) mi->vertices[2];

    /* Previous keyframe for this animation, or -1 if the animation changed
     * (which forces a full rebuild from the base pose in Phase A). */
    if (animID == prevAnimID) {
        prevKeyframe = mi->animationFrame;
    } else {
        prevKeyframe = -1;
    }

    mi->animationID = animID;
    mi->animationFrameCount = animFrame;
    mi->animationFrame = frameHi;
    interp = animFrame & 0xF;

    nVerts = model->numberOfVertices;
    nAnimVerts = model->numberOfAnimatedVertices;
    keyframeSize = nAnimVerts * 3 + 12;
    animData = (u8 *) DKR_PTR(ObjectModel_44, model->animations)[animID].animData;
    /* animatedVertexIndices is a dkrptr32 slot (ObjectModel +0x4C) — reconstruct the
     * arena pointer like model->vertices/animations below. Casting the raw token to
     * s16* (as the N64 code does) truncates it on LP64 -> wild read at indices[i]. */
    indices = DKR_PTR(s16, model->animatedVertexIndices);
    baseVerts = DKR_PTR(Vertex, model->vertices);

    /* ---- Phase A: rebuild the working buffer from base pose + frame-0 deltas.
     * Runs when we are at keyframe 0, or when the animation just changed. ---- */
    if (frameHi == 0 || prevKeyframe == -1) {
        s16 *frame0 = (s16 *) (animData + 0xC);
        for (i = 0; i < nVerts; i++) {
            s16 idx = indices[i];
            if (idx != -1) {
                s16 *dst = scratch + idx * 3;
                s16 *d = frame0 + idx * 3;
                dst[0] = baseVerts[i].x + d[0];
                dst[1] = baseVerts[i].y + d[1];
                dst[2] = baseVerts[i].z + d[2];
            }
        }
        prevKeyframe = 0;
    }

    /* ---- Phase B: walk keyframes from prevKeyframe to frameHi, accumulating
     * (forward) or removing (backward) the per-vertex s8 deltas. ---- */
    if (prevKeyframe < frameHi) {
        u8 *kf = animData + keyframeSize * (prevKeyframe + 2);
        do {
            const s8 *delta = (const s8 *) kf;
            kf += keyframeSize;
            for (i = 0; i < nAnimVerts; i++) {
                scratch[i * 3 + 0] += delta[i * 3 + 0];
                scratch[i * 3 + 1] += delta[i * 3 + 1];
                scratch[i * 3 + 2] += delta[i * 3 + 2];
            }
            prevKeyframe++;
        } while (prevKeyframe < frameHi);
    } else if (frameHi < prevKeyframe) {
        u8 *kf = animData + keyframeSize * (prevKeyframe + 2);
        do {
            const s8 *delta;
            kf -= keyframeSize;
            delta = (const s8 *) kf;
            prevKeyframe--;
            for (i = 0; i < nAnimVerts; i++) {
                scratch[i * 3 + 0] -= delta[i * 3 + 0];
                scratch[i * 3 + 1] -= delta[i * 3 + 1];
                scratch[i * 3 + 2] -= delta[i * 3 + 2];
            }
        } while (frameHi < prevKeyframe);
    }
    /* prevKeyframe == frameHi from here on. */

    /* ---- Phase C: interpolation deltas toward the next keyframe, weighted by
     * the low 4 bits of the frame counter. ---- */
    {
        const s8 *delta = (const s8 *) (animData + keyframeSize * (frameHi + 2));
        for (i = 0; i < nAnimVerts; i++) {
            sInterpScratch[i * 3 + 0] = (s16) (((u32) ((s32) delta[i * 3 + 0] * interp)) >> 4);
            sInterpScratch[i * 3 + 1] = (s16) (((u32) ((s32) delta[i * 3 + 1] * interp)) >> 4);
            sInterpScratch[i * 3 + 2] = (s16) (((u32) ((s32) delta[i * 3 + 2] * interp)) >> 4);
        }
    }

    /* ---- Interpolate the per-frame offset header and head tilt. ---- */
    {
        u8 *hdr0;
        u8 *hdr1;
        s32 offX0, offY0, offZ0, tilt0;
        s32 offX1, offY1, offZ1, tilt1;

        if (frameHi != 0) {
            hdr0 = animData + keyframeSize * (frameHi + 1) - 0xC;
            hdr1 = hdr0 + keyframeSize;
        } else {
            hdr0 = animData;
            hdr1 = animData + keyframeSize * 2 - 0xC;
        }

        offX0 = read_s16(hdr0 + 0x0);
        offY0 = read_s16(hdr0 + 0x2);
        offZ0 = read_s16(hdr0 + 0x4);
        tilt0 = read_s16(hdr0 + 0xA);
        offX1 = read_s16(hdr1 + 0x0);
        offY1 = read_s16(hdr1 + 0x2);
        offZ1 = read_s16(hdr1 + 0x4);
        tilt1 = read_s16(hdr1 + 0xA);

        mi->offsetX = offX0 + (s32) (((u32) ((offX1 - offX0) * interp)) >> 4);
        mi->offsetY = offY0 + (s32) (((u32) ((offY1 - offY0) * interp)) >> 4);
        mi->offsetZ = offZ0 + (s32) (((u32) ((offZ1 - offZ0) * interp)) >> 4);
        mi->headTilt = tilt0 + (s32) (((u32) ((tilt1 - tilt0) * interp)) >> 4);
    }

    /* ---- Final phase: sum absolute positions and interpolation deltas into the
     * newly-selected active render buffer. ---- */
    {
        s32 taskNum = mi->animationTaskNum ^ 1;
        Vertex *dst;

        mi->animationTaskNum = taskNum;
        dst = mi->vertices[taskNum];
        for (i = 0; i < nVerts; i++) {
            s16 idx = indices[i];
            if (idx != -1) {
                s16 *sc = scratch + idx * 3;
                s16 *ip = sInterpScratch + idx * 3;
                dst[i].x = sc[0] + ip[0];
                dst[i].y = sc[1] + ip[1];
                dst[i].z = sc[2] + ip[2];
            }
        }
    }

    return 1;
}
