#ifndef MDKR_SPRITE_MODELS_H
#define MDKR_SPRITE_MODELS_H
#include "sprite_model_mesh.h"
#include "mod_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

void mdkr_sprite_models_init(const MdkrModRegistry *registry);
void mdkr_sprite_models_shutdown(void);
void mdkr_sprite_models_toggle(void);
int mdkr_sprite_models_enabled(void);
int mdkr_sprite_models_count(void);
int mdkr_sprite_models_ready(unsigned sprite);
/* The command's identity is a bounded sprite ID, never an asset pointer.
 * LOD does not participate in the object's presentation identity. */
struct GfxModernMesh *mdkr_sprite_models_mesh(unsigned sprite, int far);
void mdkr_sprite_models_note_draw(unsigned sprite, int far);
int gfx_sprite_models_supported(void);
/* Retire backend references before replacing CPU model storage. Safe before
 * renderer initialization and after renderer shutdown. */
void gfx_clear_modern_mesh_cache(void);
#ifdef __cplusplus
}
#endif
#endif
