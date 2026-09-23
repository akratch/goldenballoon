#ifndef MDKR_SPRITE_MODEL_MESH_H
#define MDKR_SPRITE_MODEL_MESH_H
#include <stddef.h>
#include <stdint.h>
#include "fast3d/gfx_rendering_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* DKR-R DK RPM001 assets, in normalized local coordinates. All transforms
 * remain presentation data; no replacement enters the game's collision world. */
typedef struct MdkrSpriteModelPose {
    float position[3], scale;
    int16_t rotation[3]; /* yaw, pitch, roll */
    uint16_t sprite;
    float phase; /* banana's full 0..256 authored phase, including replay */
    int attached;
    float attachment[3];
} MdkrSpriteModelPose;

int mdkr_sprite_model_supported(unsigned sprite, int model_type,
                                int header_behavior, int behavior);
/* out must be zero-initialized or previously freed. Failure leaves it
 * unchanged; success transfers vertex/index ownership to out. Texture and GPU
 * resources are not owned by this parser or released by mesh_free. */
int mdkr_sprite_model_parse(const void *bytes, size_t size,
                            struct GfxModernMesh *out);
void mdkr_sprite_model_mesh_free(struct GfxModernMesh *mesh);
int mdkr_sprite_model_matrix(const MdkrSpriteModelPose *pose, float out[4][4]);
typedef struct MdkrSpriteModelTrunkCandidates {
    float nearest[3][3];
    float distances[3];
    unsigned found;
} MdkrSpriteModelTrunkCandidates;
void mdkr_sprite_model_trunk_candidates_init(MdkrSpriteModelTrunkCandidates *candidates);
void mdkr_sprite_model_trunk_candidates_add(MdkrSpriteModelTrunkCandidates *candidates,
                                            const float point[3],
                                            const float anchor[3]);
int mdkr_sprite_model_trunk_candidates_finish(
    const MdkrSpriteModelTrunkCandidates *candidates, float tip[3]);
int mdkr_sprite_model_trunk_tip(const float *vertices, size_t count,
                                const float anchor[3], float tip[3]);
#ifdef __cplusplus
}
#endif
#endif
