/* Placement/format ported from the maintainer's DKR-R palm_model.cpp.
 * Source assets are supplied locally, never compiled into this project. */
#include "sprite_model_mesh.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t read32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int mdkr_sprite_model_supported(unsigned id, int type, int hb, int behavior) {
    if (type != 1 || hb != behavior) return 0;
    if (id >= 147 && id <= 151) return hb == 17;
    if (id == 154 || id == 155) return hb == 77 || (id == 154 && hb == 50);
    if (id == 156) return hb == 32;
    return (id == 107 || id == 108 || (id >= 113 && id <= 116)) && hb == 2;
}

void mdkr_sprite_model_mesh_free(struct GfxModernMesh *mesh) {
    if (!mesh) return;
    free((void *)mesh->vtx);
    free((void *)mesh->idx);
    memset(mesh, 0, sizeof(*mesh));
}

int mdkr_sprite_model_parse(const void *data, size_t size, struct GfxModernMesh *out) {
    const unsigned char *bytes = data;
    struct GfxModernMesh parsed = {0};
    float *vertices;
    uint32_t *indices, nv, ni, i, j;
    if (!out || out->vtx || out->idx || out->tex_rgba || out->backend_handle ||
        !bytes || size < 16 || memcmp(bytes, "DKRPM001", 8)) return 0;
    nv = read32(bytes + 8); ni = read32(bytes + 12);
    if (!nv || nv > 60000 || !ni || ni > 120000 || ni % 3 ||
        size != 16u + (size_t)nv * 32u + (size_t)ni * 4u) return 0;
    vertices = calloc(nv, 36); indices = malloc((size_t)ni * sizeof(*indices));
    parsed.vtx = vertices; parsed.idx = indices;
    if (!vertices || !indices) goto fail;
    for (i = 0; i < nv; ++i) {
        for (j = 0; j < 8; ++j) {
            uint32_t word = read32(bytes + 16 + i * 32 + j * 4);
            float value;
            memcpy(&value, &word, sizeof value);
            if (!isfinite(value) || fabsf(value) > (j < 3 ? 2.0f : j < 6 ? 1.01f : 16.0f)) goto fail;
            vertices[i * 9 + j] = value;
        }
        /* The inherited decor shader reserves vertex alpha for snow cover. */
        ((unsigned char *)&vertices[i * 9 + 8])[0] = 255;
        ((unsigned char *)&vertices[i * 9 + 8])[1] = 255;
        ((unsigned char *)&vertices[i * 9 + 8])[2] = 255;
    }
    for (i = 0; i < ni; ++i) {
        indices[i] = read32(bytes + 16 + nv * 32 + i * 4);
        if (indices[i] >= nv) goto fail;
    }
    parsed.vtx_count = nv; parsed.idx_count = ni; parsed.cutout = 1;
    *out = parsed;
    return 1;
fail:
    mdkr_sprite_model_mesh_free(&parsed);
    return 0;
}

static void position(const MdkrSpriteModelPose *p, const float v[3], float out[3]) {
    const unsigned id = p->sprite;
    float width = 66, height = 66, bottom = 2;
    float x, y, z, t, cy, sy, cp, sp, cr, sr;
    const float rad = 6.283185307179586f / 65536.0f;
    const int attached = p->attached && (id == 114 || id == 116);
    if ((id >= 147 && id <= 151) || id == 154 || id == 155) {
        width = 43; height = id == 154 ? 48 : id == 155 ? 50 : 66;
        bottom = id == 154 ? 22 : id == 155 ? 21 : id == 147 ? 4 : 6;
    } else if (id == 156) { width = height = 50; bottom = 9;
    } else if (id == 107) { width = height = 122; bottom = -6;
    } else if (id == 108) { width = height = 113; bottom = 0;
    } else if (id == 113) { width = height = 115; bottom = 0;
    } else if (id == 114) { width = height = 139; bottom = -69.5f;
    } else if (id == 116) { width = 78; height = 60; bottom = -30; }
    x = v[0] * width; y = v[1] * height + bottom; z = v[2] * width;
    if (attached) { x += .010f * width; z -= .0025f * width; }
    cy = cosf((p->rotation[0] + (id == 156 ? p->phase * 256 : 0)) * rad);
    sy = sinf((p->rotation[0] + (id == 156 ? p->phase * 256 : 0)) * rad);
    cp = cosf(p->rotation[1] * rad); sp = sinf(p->rotation[1] * rad);
    cr = cosf(p->rotation[2] * rad); sr = sinf(p->rotation[2] * rad);
    t = y * cp - z * sp; z = y * sp + z * cp; y = t;
    t = x * cr - y * sr; y = x * sr + y * cr; x = t;
    out[0] = (attached ? p->attachment[0] : p->position[0]) + p->scale * (x * cy + z * sy);
    out[1] = (attached ? p->attachment[1] - 3 : p->position[1]) + p->scale * y;
    out[2] = (attached ? p->attachment[2] : p->position[2]) + p->scale * (-x * sy + z * cy);
}

int mdkr_sprite_model_matrix(const MdkrSpriteModelPose *p, float out[4][4]) {
    float origin[3], unit[3] = {0}, endpoint[3];
    unsigned i, j;
    if (!p || !out || !isfinite(p->scale) || p->scale <= 0 || p->scale > 32 ||
        !isfinite(p->phase) || fabsf(p->phase) > 512) return 0;
    for (i = 0; i < 3; ++i)
        if (!isfinite(p->position[i]) || fabsf(p->position[i]) >= 30000 ||
            (p->attached && (!isfinite(p->attachment[i]) || fabsf(p->attachment[i]) >= 30000))) return 0;
    position(p, unit, origin);
    memset(out, 0, 16 * sizeof(float));
    /* Evaluate the linear basis around zero to avoid cancellation at distant
     * world positions. This matches the source's static-local-mesh contract. */
    {
        MdkrSpriteModelPose local = *p;
        float local_origin[3];
        memset(local.position, 0, sizeof(local.position));
        memset(local.attachment, 0, sizeof(local.attachment));
        position(&local, unit, local_origin);
        for (i = 0; i < 3; ++i) {
            unit[i] = 1; position(&local, unit, endpoint); unit[i] = 0;
            for (j = 0; j < 3; ++j) out[i][j] = endpoint[j] - local_origin[j];
        }
    }
    memcpy(out[3], origin, sizeof origin); out[3][3] = 1;
    return 1;
}

void mdkr_sprite_model_trunk_candidates_init(MdkrSpriteModelTrunkCandidates *candidates) {
    if (candidates == NULL) return;
    memset(candidates, 0, sizeof(*candidates));
    candidates->distances[0] = candidates->distances[1] = candidates->distances[2] = 10001.0f;
}

void mdkr_sprite_model_trunk_candidates_add(MdkrSpriteModelTrunkCandidates *candidates,
                                            const float p[3], const float anchor[3]) {
    float d = 0;
    unsigned i, j, a;
    if (candidates == NULL || p == NULL || anchor == NULL || candidates->found > 3) return;
    for (a = 0; a < 3; ++a) {
        const float delta = p[a] - anchor[a];
        if (!isfinite(p[a]) || !isfinite(anchor[a])) return;
        d += delta * delta;
    }
    if (!isfinite(d) || d > 10000) return;
    for (j = 0; j < candidates->found; ++j)
        /* Coordinates, not representations: +0 and -0 are the same point.
         * Nonfinite inputs have already been rejected above. */
        if (p[0] == candidates->nearest[j][0] &&
            p[1] == candidates->nearest[j][1] &&
            p[2] == candidates->nearest[j][2]) return;
    for (i = 0; i < 3; ++i) if (d < candidates->distances[i]) {
        for (j = 2; j > i; --j) {
            candidates->distances[j] = candidates->distances[j - 1];
            memcpy(candidates->nearest[j], candidates->nearest[j - 1], sizeof candidates->nearest[j]);
        }
        candidates->distances[i] = d;
        memcpy(candidates->nearest[i], p, sizeof candidates->nearest[i]);
        if (candidates->found < 3) ++candidates->found;
        break;
    }
}

int mdkr_sprite_model_trunk_candidates_finish(
    const MdkrSpriteModelTrunkCandidates *candidates, float tip[3]) {
    unsigned i, j, a;
    if (candidates == NULL || tip == NULL || candidates->found != 3) return 0;
    for (i = 0; i < 3; ++i) for (j = i + 1; j < 3; ++j) {
        float d = 0;
        for (a = 0; a < 3; ++a) {
            const float delta = candidates->nearest[i][a] - candidates->nearest[j][a];
            d += delta * delta;
        }
        if (d > 1600) return 0;
    }
    for (a = 0; a < 3; ++a)
        tip[a] = (candidates->nearest[0][a] + candidates->nearest[1][a] + candidates->nearest[2][a]) / 3;
    return 1;
}

int mdkr_sprite_model_trunk_tip(const float *v, size_t count, const float anchor[3], float tip[3]) {
    MdkrSpriteModelTrunkCandidates candidates;
    size_t k;
    mdkr_sprite_model_trunk_candidates_init(&candidates);
    if (v == NULL || anchor == NULL || tip == NULL) return 0;
    for (k = 0; k < count; ++k) mdkr_sprite_model_trunk_candidates_add(&candidates, v + k * 3, anchor);
    return mdkr_sprite_model_trunk_candidates_finish(&candidates, tip);
}
