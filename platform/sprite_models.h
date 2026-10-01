#ifndef MDKR_SPRITE_MODELS_H
#define MDKR_SPRITE_MODELS_H
#include "sprite_model_mesh.h"
#include "mod_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Decodes nothing unless gfx_sprite_models_supported(): the launcher has no
 * game renderer and the OpenGL backend has no model path. */
void mdkr_sprite_models_init(const MdkrModRegistry *registry);
/* Frees all model storage. While any is loaded it first retires every
 * renderer reference to it (retained display lists, then the GPU mesh
 * cache), so no caller can free storage the renderer still names. */
/* Sprites a registry supplies a model for (by its near mesh), decoding none. */
unsigned mdkr_sprite_models_present(const MdkrModRegistry *registry);
void mdkr_sprite_models_shutdown(void);
void mdkr_sprite_models_toggle(void);
int mdkr_sprite_models_enabled(void);
int mdkr_sprite_models_count(void);
int mdkr_sprite_models_ready(unsigned sprite);
/* The command's identity is a bounded sprite ID, never an asset pointer.
 * LOD does not participate in the object's presentation identity. */
struct GfxModernMesh *mdkr_sprite_models_mesh(unsigned sprite, int far);
enum {
    MDKR_SPRITE_MODEL_DRAW_FADING = 1u,      /* prim alpha below one */
    MDKR_SPRITE_MODEL_DRAW_DEPTH_WRITE = 2u, /* the draw wrote depth */
    MDKR_SPRITE_MODEL_DRAW_ENV_TINT = 4u     /* lerped toward the level light */
};
void mdkr_sprite_models_note_draw(unsigned sprite, int far, unsigned flags);
/* .dkrmesh files parsed since the last init; shared meshes parse once. */
unsigned mdkr_sprite_models_geometry_parses(void);
/* Census of model poses: one per authored draw and one per presentation
 * replay, with the trunk attachment and the phase the pose used. Reported at
 * shutdown beside the draw counts. */
void mdkr_sprite_models_note_pose(unsigned sprite, float phase, int attached, int replayed);
int gfx_sprite_models_supported(void);
/* Retire backend references before replacing CPU model storage. Safe before
 * renderer initialization and after renderer shutdown. */
void gfx_clear_modern_mesh_cache(void);
/* Retire retained display lists (gfx_pc_dkr.c); they address models by
 * sprite ID and must never resolve against another pack generation. */
void gfx_dkr_replay_invalidate(void);
#ifdef __cplusplus
}
#endif
#endif
