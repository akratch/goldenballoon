/*
 * A replacement sprite model inherits the material of the billboard it
 * replaces, by behaviour.
 *
 * Links the PRODUCTION game/src/textures_sprites.c (dead-stripped to the
 * material path): the flags come from material_sprite_billboard_flags, the
 * one helper both the billboard and the model path load, and the material from
 * material_load_simple. The test follows the display list it emits into the
 * draw table entry it selects and decodes what the RDP would run: whether the
 * render mode writes depth, the cycle type and the second-cycle combiner.
 * That decoded state then goes through gfx_combine_cycle2_rgb, the decode the
 * DKR renderer runs on every combine, and gfx_modern_material_resolve, the
 * resolver it hands the mesh backend.
 *
 * Three claims (audit A1 #2): a fading billboard does not write depth, so its
 * model must not either; a fogged billboard lerps toward the level light
 * colour (ENV by ENV_ALPHA) in its second cycle, so its model must receive
 * that colour; and a material without that cycle receives none.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "types.h"
#include "structs.h"
#include "f3ddkr.h"
#include "textures_sprites.h"
#include "fast3d/gfx_modern_material.h"

static int failures;

#define EXPECT(cond, ...)                                                  \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                                  \
            fputc('\n', stderr);                                           \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

/* Link stubs. Pointers handed to the display list become small tokens so the
 * test can follow a G_DMADL back to the table entry it names. */
static const void *sTokens[64];
static unsigned sTokenCount;
u32 dkr_k0_to_physical(const void *x) {
    for (unsigned i = 0; i < sTokenCount; ++i) {
        if (sTokens[i] == x) return 0x40000000u + i;
    }
    if (sTokenCount < 64) sTokens[sTokenCount] = x;
    return 0x40000000u + sTokenCount++;
}
void dkr_dl_register_host_ptr(const void *x) { (void)x; }
int mdkr_trace_level(void) { return 0; }
void mdkr_trace(const char *fmt, ...) { (void)fmt; }

typedef struct DecodedMaterial {
    bool found;
    uint32_t combine_w0, combine_w1, othermode_h, othermode_l;
} DecodedMaterial;

/* What material_load_simple emits for these final material flags. */
static DecodedMaterial load_material(s32 materialFlags) {
    Gfx dl[32];
    Gfx *cursor = dl;
    DecodedMaterial out = {0};
    memset(dl, 0, sizeof dl);
    sprite_opaque(TRUE); /* the game's default; also forces the reload */
    material_load_simple(&cursor, materialFlags);
    for (const Gfx *g = dl; g < cursor; ++g) {
        if ((g->words.w0 >> 24) != (unsigned)G_DMADL) continue;
        const unsigned token = (unsigned)g->words.w1 - 0x40000000u;
        if (token >= sTokenCount) continue;
        const Gfx *entry = (const Gfx *)sTokens[token];
        for (unsigned i = 0; i < ((g->words.w0 >> 16) & 0xFFu); ++i) {
            const unsigned op = (unsigned)(entry[i].words.w0 >> 24);
            if (op == (unsigned)G_SETCOMBINE) {
                out.combine_w0 = (uint32_t)entry[i].words.w0;
                out.combine_w1 = (uint32_t)entry[i].words.w1;
            } else if (op == (unsigned)G_RDPSETOTHERMODE) {
                out.othermode_h = (uint32_t)entry[i].words.w0 & 0xFFFFFFu;
                out.othermode_l = (uint32_t)entry[i].words.w1;
                out.found = true;
            }
        }
    }
    return out;
}

/* The billboard's material: the flags both the billboard and the model load. */
static DecodedMaterial load(const Sprite *sprite, s32 objectFlags) {
    return load_material(material_sprite_billboard_flags(sprite, objectFlags));
}

/* The same decode the DKR renderer runs on every G_SETCOMBINE
 * (gfx_combine_cycle2_rgb in dkr_dp_set_combine), then its resolver. */
static void resolve(const DecodedMaterial *m, const uint8_t prim[4], const uint8_t env[4],
                    struct GfxModernMeshMaterial *out) {
    gfx_modern_material_resolve(gfx_combine_cycle2_rgb(m->combine_w0, m->combine_w1),
                                (m->othermode_h & (3u << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE,
                                prim, env, out);
}

static bool writes_depth(const DecodedMaterial *m) {
    return (m->othermode_l & Z_CMP) != 0 && (m->othermode_l & Z_UPD) == Z_UPD;
}

int main(void) {
    /* render_3d_billboard's flags: the object's own, plus Z update and fog;
     * RENDER_SEMI_TRANSPARENT when it fades. A banana or balloon does not
     * carry the vehicle-part bit, which shares the anti-aliasing bit. */
    Sprite sprite;
    memset(&sprite, 0, sizeof sprite);
    sprite.drawFlags = RENDER_CUTOUT;
    const s32 object = RENDER_Z_COMPARE | RENDER_Z_UPDATE | RENDER_FOG_ACTIVE;
    const uint8_t prim[4] = {255, 255, 255, 128}, env[4] = {200, 100, 50, 96};
    struct GfxModernMeshMaterial material;

    DecodedMaterial opaque = load(&sprite, object);
    DecodedMaterial fading = load(&sprite, object | RENDER_SEMI_TRANSPARENT);
    EXPECT(opaque.found && fading.found, "material_load_simple selected a draw table entry");
    EXPECT(writes_depth(&opaque), "an opaque cutout billboard writes depth");
    EXPECT(!writes_depth(&fading), "a fading billboard (and so its model) does not write depth");

    /* Control: the flags the model path used to load, the object's flags
     * masked without the billboard's adjustment, select the interpenetrating
     * entry that DOES write depth, so the check above can fail. */
    {
        DecodedMaterial raw = load_material(sprite.drawFlags |
            ((object | RENDER_SEMI_TRANSPARENT) &
             (RENDER_FOG_ACTIVE | RENDER_SEMI_TRANSPARENT | RENDER_Z_COMPARE | RENDER_ANTI_ALIASING)));
        EXPECT(raw.found && writes_depth(&raw), "control: unadjusted flags select a depth-writing entry");
    }

    resolve(&opaque, prim, env, &material);
    EXPECT(material.env[0] == 200 / 255.0f && material.env[1] == 100 / 255.0f &&
           material.env[2] == 50 / 255.0f && material.env[3] == 96 / 255.0f,
           "a fogged billboard's second cycle hands its model the level light colour");
    EXPECT(material.tint[3] == 128 / 255.0f && material.tint[0] == 1.0f,
           "the model is modulated and faded by the billboard's prim colour");
    resolve(&fading, prim, env, &material);
    EXPECT(material.env[3] == 96 / 255.0f, "a fading billboard keeps the light tint");

    DecodedMaterial unfogged = load(&sprite, RENDER_Z_COMPARE | RENDER_Z_UPDATE);
    resolve(&unfogged, prim, env, &material);
    EXPECT(unfogged.found && material.env[3] == 0.0f && material.env[0] == 0.0f,
           "a material without the ENV cycle gives its model no light tint");

    if (failures) {
        fprintf(stderr, "sprite_model_material: %d failure(s)\n", failures);
        return 1;
    }
    puts("sprite_model_material: fading models write no depth; the light tint follows the billboard's combiner");
    return 0;
}
