/*
 * Pulsing-light material uploads, by behaviour.
 *
 * Links the PRODUCTION game/src/textures_sprites.c (dead-stripped to the
 * material path) and decodes the display list material_set_blinking_lights()
 * actually emits into a small TMEM model: which source byte each tile samples
 * at every texel coordinate, and which bytes each LOADBLOCK reads.
 *
 * Retail always loads two 2048-byte images: a 64x16 or 32x32 tile at the
 * texel base and another at +2048 bytes. That is exactly the texture for the
 * two 4096-byte layouts (64x32 and 32x64), whose commands must stay
 * byte-identical. Smaller textures make the N64 fetch whatever follows the
 * allocation (Spaceport Alpha's 16x32 floor ring reads a Gfx list and the next
 * texture). The native contract: never read outside the texture, and sample
 * exactly the byte the N64 samples wherever the N64 sampled the texture
 * itself; an address past the texture wraps back into it.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "structs.h"
#include "f3ddkr.h"
#include "textures_sprites.h"

#define ARENA_TOKEN 0x10000000u
#define OTHER_TOKEN 0x70000000u

static unsigned char *sArena;
static size_t sArenaSize;
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

/* Link stubs for the symbols left on the material path after dead-stripping. */
u32 dkr_k0_to_physical(const void *x) {
    const unsigned char *p = x;
    if (sArena != NULL && p >= sArena && p <= sArena + sArenaSize) {
        return ARENA_TOKEN + (u32)(p - sArena);
    }
    return OTHER_TOKEN;
}
void dkr_dl_register_host_ptr(const void *x) { (void)x; }
int mdkr_trace_level(void) { return 0; }
void mdkr_trace(const char *fmt, ...) { (void)fmt; }

typedef struct TileState {
    unsigned tmem, line, cms, cmt, masks, maskt, siz, uls, ult, lrs, lrt;
} TileState;

typedef struct TmemModel {
    long long word_source[512]; /* arena byte offset of each loaded 64-bit word */
    TileState tile[8];
    unsigned timg;
    unsigned loads;
    long long load_begin[8], load_end[8];
} TmemModel;

static void model_run(TmemModel *m, const Gfx *begin, const Gfx *end) {
    memset(m, 0, sizeof(*m));
    for (int i = 0; i < 512; ++i) m->word_source[i] = -1;
    for (const Gfx *g = begin; g < end; ++g) {
        const unsigned w0 = (unsigned)g->words.w0, w1 = (unsigned)g->words.w1;
        const unsigned op = w0 >> 24;
        if (op == (unsigned)G_SETTIMG) {
            m->timg = w1;
        } else if (op == (unsigned)G_SETTILE) {
            TileState *t = &m->tile[(w1 >> 24) & 7];
            t->siz = (w0 >> 19) & 3;
            t->line = (w0 >> 9) & 0x1FF;
            t->tmem = w0 & 0x1FF;
            t->cmt = (w1 >> 18) & 3;
            t->maskt = (w1 >> 14) & 15;
            t->cms = (w1 >> 8) & 3;
            t->masks = (w1 >> 4) & 15;
        } else if (op == (unsigned)G_SETTILESIZE) {
            TileState *t = &m->tile[(w1 >> 24) & 7];
            t->uls = (w0 >> 12) & 0xFFF; t->ult = w0 & 0xFFF;
            t->lrs = (w1 >> 12) & 0xFFF; t->lrt = w1 & 0xFFF;
        } else if (op == (unsigned)G_LOADBLOCK) {
            const TileState *load = &m->tile[(w1 >> 24) & 7];
            const unsigned texels = ((w1 >> 12) & 0xFFF) + 1;
            const unsigned bytes = texels * 2; /* every load here is 16-bit */
            const long long source = (long long)m->timg - ARENA_TOKEN;
            EXPECT(m->timg >= ARENA_TOKEN && m->timg < OTHER_TOKEN, "load from a non-texture address");
            if (m->loads < 8) {
                m->load_begin[m->loads] = source;
                m->load_end[m->loads] = source + bytes;
            }
            ++m->loads;
            for (unsigned b = 0; b < bytes; b += 8) {
                const unsigned word = load->tmem + b / 8;
                if (word < 512) m->word_source[word] = source + b;
            }
        }
    }
}

/* Source arena offset sampled by render tile `tile` at texel (s, t). */
static long long model_sample(const TmemModel *m, unsigned tile, unsigned s, unsigned t) {
    const TileState *ts = &m->tile[tile];
    const unsigned width = (ts->lrs >> 2) + 1, height = (ts->lrt >> 2) + 1;
    /* RDP rule: a mask wraps; with no mask only the clamp bit holds a
     * coordinate inside the tile, otherwise it walks on through TMEM. */
    if (ts->masks) s &= (1u << ts->masks) - 1;
    else if ((ts->cms & G_TX_CLAMP) && s >= width) s = width - 1;
    if (ts->maskt) t &= (1u << ts->maskt) - 1;
    else if ((ts->cmt & G_TX_CLAMP) && t >= height) t = height - 1;
    const unsigned byte = ts->tmem * 8 + t * ts->line * 8 + s * 2;
    if (byte / 8 >= 512 || m->word_source[byte / 8] < 0) return -1;
    return m->word_source[byte / 8] + byte % 8;
}

static unsigned char *make_texture(unsigned width, unsigned height, unsigned frames, size_t *texels) {
    const size_t bytes = (size_t)width * height * 2;
    const size_t stride = 32 + bytes;
    *texels = bytes;
    free(sArena);
    sArenaSize = stride * frames;
    sArena = calloc(1, sArenaSize);
    if (!sArena) { perror("calloc"); exit(2); }
    for (unsigned f = 0; f < frames; ++f) {
        TextureHeader *h = (TextureHeader *)(sArena + f * stride);
        h->width = (u8)width;
        h->height = (u8)height;
        h->format = 0x11; /* RGBA16 */
        h->numOfTextures = (u16)(frames << 8);
        h->textureSize = (s16)stride;
    }
    return sArena;
}

static size_t emit(unsigned char *texture, u32 flags, s32 index, Gfx *out, size_t capacity) {
    Gfx *cursor = out;
    memset(out, 0, capacity * sizeof(*out));
    material_set_blinking_lights(&cursor, (TextureHeader *)texture, flags, index);
    return (size_t)(cursor - out);
}

static void check_policy(unsigned width, unsigned height, unsigned frame) {
    Gfx dl[64];
    TmemModel m;
    size_t texels;
    unsigned char *texture = make_texture(width, height, frame + 1, &texels);
    const size_t base = (size_t)frame * (32 + texels) + 32;
    const size_t count = emit(texture, RENDER_Z_COMPARE | RENDER_FOG_ACTIVE | RENDER_PULSING_LIGHTS,
                              (s32)(frame << 16), dl, 64);
    EXPECT(count > 0 && count < 64, "%ux%u: emitted %zu commands", width, height, count);
    model_run(&m, dl, dl + count);
    EXPECT(m.loads == 2, "%ux%u: %u texture loads, expected two images", width, height, m.loads);
    for (unsigned i = 0; i < m.loads && i < 8; ++i) {
        EXPECT(m.load_begin[i] >= (long long)base && m.load_end[i] <= (long long)(base + texels),
               "%ux%u frame %u: load %u reads [%lld,%lld) outside texels [%zu,%zu)", width, height, frame,
               i, m.load_begin[i], m.load_end[i], base, base + texels);
    }
    /* Retail: tile 1 (TEXEL1) at +0, tile 0 (TEXEL0) at +2048, 64x16 for
     * 64-wide textures and 32x32 for every other width. */
    const unsigned tw = width == 64 ? 64 : 32, th = width == 64 ? 16 : 32;
    unsigned mismatches = 0;
    for (unsigned tile = 0; tile < 2; ++tile) {
        for (unsigned t = 0; t < 2 * th; ++t) {
            for (unsigned s = 0; s < 2 * tw; ++s) {
                const size_t retail = (tile == 1 ? 0u : 2048u) + ((t % th) * tw + (s % tw)) * 2u;
                const long long expected = (long long)(base + retail % texels);
                const long long sampled = model_sample(&m, tile, s, t);
                if (sampled != expected && mismatches++ < 4) {
                    fprintf(stderr, "  %ux%u tile %u (%u,%u): samples %lld, N64 byte %zu -> want %lld\n",
                            width, height, tile, s, t, sampled - (long long)base, retail,
                            expected - (long long)base);
                }
            }
        }
    }
    EXPECT(mismatches == 0, "%ux%u frame %u: %u texel samples differ from the retail address (wrapped into "
           "the texture)", width, height, frame, mismatches);
}

static void check_legacy_bytes(unsigned width, unsigned height) {
    Gfx dl[64], retail[64];
    size_t texels;
    unsigned char *texture = make_texture(width, height, 1, &texels);
    const size_t count = emit(texture, RENDER_Z_COMPARE | RENDER_PULSING_LIGHTS, 0, dl, 64);
    Gfx *r = retail;
    const u32 mblock = ARENA_TOKEN + 32, tblock = mblock + 2048;
    memset(retail, 0, sizeof(retail));
    if (width == 64) {
        gDPLoadMultiBlock(r++, mblock, 256, 1, G_IM_FMT_RGBA, G_IM_SIZ_16b, 64, 16, 0, 0, 0, 6, 4, 0, 0);
        gDPLoadTextureBlock(r++, tblock, G_IM_FMT_RGBA, G_IM_SIZ_16b, 64, 16, 0, 0, 0, 6, 4, 0, 0);
    } else {
        gDPLoadMultiBlock(r++, mblock, 256, 1, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, 0, 0, 5, 5, 0, 0);
        gDPLoadTextureBlock(r++, tblock, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, 0, 0, 5, 5, 0, 0);
    }
    const size_t retail_count = (size_t)(r - retail);
    EXPECT(count >= retail_count && !memcmp(dl, retail, retail_count * sizeof(Gfx)),
           "%ux%u: texture commands differ from retail", width, height);
}

/* Positive control: the model must see retail's 16x32 loads leave the texture
 * and its 32-wide samples, or the checks above could not fail. */
static void check_model_control(void) {
    Gfx retail[32];
    TmemModel m;
    size_t texels;
    Gfx *r = retail;
    make_texture(16, 32, 1, &texels);
    const u32 mblock = ARENA_TOKEN + 32;
    gDPLoadMultiBlock(r++, mblock, 256, 1, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, 0, 0, 5, 5, 0, 0);
    gDPLoadTextureBlock(r++, mblock + 2048, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, 0, 0, 5, 5, 0, 0);
    model_run(&m, retail, r);
    EXPECT(m.loads == 2 && m.load_end[0] == 32 + 2048 && m.load_begin[1] == 32 + 2048,
           "control: retail 16x32 loads must read 2048 bytes from the base and from +2048");
    EXPECT(m.load_end[0] > (long long)(32 + texels), "control: retail TEXEL1 load must overrun the 1024 texels");
    EXPECT(model_sample(&m, 1, 17, 3) == 32 + (3 * 32 + 17) * 2, "control: retail tile is 32 texels wide");
    EXPECT(model_sample(&m, 0, 0, 0) == 32 + 2048, "control: retail TEXEL0 starts past the texture");
}

int main(void) {
    check_model_control();
    /* Both retail 4096-byte layouts, byte-for-byte and by behaviour. */
    check_legacy_bytes(64, 32);
    check_legacy_bytes(32, 64);
    /* Spaceport Alpha's 16x32 floor ring (model 18, texture 211), and every
     * other admitted RGBA16 pulse shape, including a second animation frame. */
    for (unsigned width = 4; width <= 64; width *= 2) {
        for (unsigned height = 2; height <= 64; height *= 2) {
            if (width * height > 2048) continue;
            check_policy(width, height, 0);
        }
    }
    check_policy(16, 32, 1);
    check_policy(64, 32, 1);
    free(sArena);
    if (failures) {
        fprintf(stderr, "pulse_material: %d failure(s)\n", failures);
        return 1;
    }
    puts("pulse_material: legacy layouts byte-identical; every shape samples the N64 byte, wrapped into "
         "the texture");
    return 0;
}
