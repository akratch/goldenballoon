/*
 * hasm_native/obj_shade_fast.c
 *
 * Native C reimplementation of the two per-vertex object lighting routines that
 * ship as handwritten MIPS assembly in game/src/hasm/obj_shade_fast.s:
 *
 *   - obj_shade_fast                     (0x800245F0)
 *   - calc_dynamic_lighting_for_object_2 (0x80024744)
 *
 * Both compute a per-vertex grayscale shade value from the dot product of the
 * model's vertex normals with a light/shadow direction, then write it into the
 * object's active (10-byte) Vertex buffer as r = g = b = shade, a = 0xFF.
 *
 * Fixed-point / wrapping semantics are preserved exactly from the asm:
 *   - normal.xyz are s16, direction components are s32; products are taken as
 *     32-bit (matching MIPS mult/mflo low-word) and summed.
 *   - the float->int conversions truncate toward zero (asm sets round-to-zero
 *     rounding mode around each cvt.w.s), which is what a plain C (s32) cast of
 *     a float does.
 *   - the `(x * factor) >> N` steps use MIPS `mult`/`srl`, i.e. the LOW 32 bits
 *     of the product interpreted as unsigned then logically shifted.  We do the
 *     multiply through u32 so the low word matches bit-for-bit and the shift is
 *     logical.  (In the taken branch the operands are non-negative anyway.)
 *
 * Endianness: per the port's asset-load normalization policy, all in-memory
 * model data (Vertex xyz, Vec3s normals) is native-endian by the time these run,
 * so this is written as natural C with no byteswaps.
 */

#include "objects.h"
#include "textures_sprites.h" /* RENDER_ENVMAP */
#include "camera.h"           /* get_projection_matrix_f32 */
#include "math_util.h"        /* mtxf_transform_dir, mtxf_from_inverse_transform */

/*
 * Fast single-light grayscale shading.
 *
 * a0 = model, a1 = obj, a2 = intensity (passed as a float; on N64 O32 it arrives
 * in an integer register and is bit-copied into an FPR, but the C signature is a
 * plain f32 which is the natural native calling convention).
 */
void obj_shade_fast(ObjectModel *model, Object *obj, f32 intensity) {
    ShadeProperties *shading = obj->shading;
    TriangleBatchInfo *batch;
    TriangleBatchInfo *endBatch;
    Vertex *vertices;
    Vec3s *normals;
    s32 scale;
    s16 dirX, dirY, dirZ;

    if (shading == NULL) {
        return;
    }

    /* Intensity scale factor, truncated toward zero (matches cvt.w.s). */
    scale = (s32) (shading->unk0 * intensity * 160.0f);

    /* obj_shade_fast lights from the object's shadow direction. */
    dirX = shading->shadowDirX;
    dirY = shading->shadowDirY;
    dirZ = shading->shadowDirZ;

    batch = DKR_PTR(TriangleBatchInfo, model->batches);
    endBatch = &DKR_PTR(TriangleBatchInfo, model->batches)[model->numberOfBatches];
    vertices = obj->curVertData;
    normals = DKR_PTR(Vec3s, model->normals);

    for (; batch < endBatch; batch++) {
        s32 numVerts = batch[1].verticesOffset - batch->verticesOffset;

        if (batch->miscData == BATCH_VTX_COL) {
            /* Vertex-color batch: no lighting written, but the running vertex
             * pointer always advances, and the normal pointer only advances if
             * this batch also consumes normals for environment mapping. */
            vertices += numVerts;
            if (batch->flags & RENDER_ENVMAP) {
                normals += numVerts;
            }
        } else {
            s32 k;
            for (k = 0; k < numVerts; k++) {
                s32 raw = ((s32) normals->x * dirX + (s32) normals->y * dirY + (s32) normals->z * dirZ) >> 11;
                s32 val;

                normals++;
                if (raw > 0) {
                    val = (s32) (((u32) raw * (u32) scale) >> 16) + scale;
                    if (val >= 0x100) {
                        val = 0xFF;
                    }
                } else {
                    val = scale;
                }

                vertices->r = val;
                vertices->g = val;
                vertices->b = val;
                vertices->a = 0xFF;
                vertices++;
            }
        }
    }
}

/*
 * Dynamic ambient+diffuse lighting (used for racers, the Rare logo, Wizpig
 * face, etc.).  The shadow direction is rotated into object space (and optionally
 * projected) before the per-vertex dot product.
 *
 * a0 = object, a1 = model, a2 = arg2 (project flag), a3 = intensity (f32).
 */
void calc_dynamic_lighting_for_object_2(Object *object, ObjectModel *model, s16 arg2, f32 intensity) {
    ShadeProperties *shading = object->shading;
    ObjectTransform invTrans;
    MtxF invMtx;
    Vec3f direction;
    TriangleBatchInfo *batch;
    TriangleBatchInfo *endBatch;
    Vertex *vertices;
    Vec3s *normals;
    f32 shadeBase;
    s32 ambientFactor, diffuseFactor;
    s32 dirX, dirY, dirZ;

    if (shading == NULL) {
        return;
    }

    /* Start from the object's shadow direction (scaled up by 4). */
    direction.x = (f32) (shading->shadowDirX * 4);
    direction.y = (f32) (shading->shadowDirY * 4);
    direction.z = (f32) (shading->shadowDirZ * 4);

    if (arg2 != 0) {
        mtxf_transform_dir(get_projection_matrix_f32(), &direction, &direction);
    }

    /* Rotate the light direction into the object's local space by transforming
     * it through the inverse of the object's rotation (scale 1, no translation).
     * Vec3s.s[] order is {y_rotation, x_rotation, z_rotation}. */
    invTrans.rotation.s[0] = -object->trans.rotation.s[0];
    invTrans.rotation.s[1] = -object->trans.rotation.s[1];
    invTrans.rotation.s[2] = -object->trans.rotation.s[2];
    invTrans.scale = 1.0f;
    invTrans.position.x = 0.0f;
    invTrans.position.y = 0.0f;
    invTrans.position.z = 0.0f;
    mtxf_from_inverse_transform(&invMtx, &invTrans);
    mtxf_transform_dir(&invMtx, &direction, &direction);

    /* Ambient and diffuse light factors, truncated toward zero. */
    shadeBase = shading->unk0 * intensity * 255.0f;
    ambientFactor = (s32) (shading->ambient * shadeBase);
    diffuseFactor = (s32) (shading->diffuse * shadeBase);

    /* Direction components truncated toward zero to integers. */
    dirX = (s32) direction.x;
    dirY = (s32) direction.y;
    dirZ = (s32) direction.z;

    batch = DKR_PTR(TriangleBatchInfo, model->batches);
    endBatch = &DKR_PTR(TriangleBatchInfo, model->batches)[model->numberOfBatches];
    vertices = object->curVertData;
    normals = DKR_PTR(Vec3s, model->normals);

    for (; batch < endBatch; batch++) {
        s32 numVerts = batch[1].verticesOffset - batch->verticesOffset;

        if (batch->miscData == BATCH_VTX_COL) {
            vertices += numVerts;
            if (batch->flags & RENDER_ENVMAP) {
                normals += numVerts;
            }
        } else {
            s32 k;
            for (k = 0; k < numVerts; k++) {
                s32 raw = ((s32) normals->x * dirX + (s32) normals->y * dirY + (s32) normals->z * dirZ) >> 7;
                s32 val;

                normals++;
                if (raw > 0) {
                    val = (s32) (((u32) raw * (u32) diffuseFactor) >> 21) + ambientFactor;
                    if (val >= 0x100) {
                        val = 0xFF;
                    }
                } else {
                    val = ambientFactor;
                }

                vertices->r = val;
                vertices->g = val;
                vertices->b = val;
                vertices->a = 0xFF;
                vertices++;
            }
        }
    }
}
