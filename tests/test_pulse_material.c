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
 *
 * The one exception is that retail ring itself (3D texture 211) when its
 * console neighbour, texture 880, is loaded and both pass their shape and
 * content checks: the unchanged retail commands then run against an owned
 * 4096-byte copy laid out as console RDRAM holds it (ring texels, a 160-byte
 * zero fill, then texture 880's texels). The fixtures forge those CRCs over
 * synthetic texels; no ROM bytes are involved.
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
/* Host pointers that are not in the arena (the owned console image, the draw
 * tables) become OTHER_TOKEN + index so a load from them can still be read. */
static const unsigned char *sHostPointers[64];
static unsigned sHostPointerCount;

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
    for (unsigned i = 0; i < sHostPointerCount; ++i) {
        if (sHostPointers[i] == p) return OTHER_TOKEN + i;
    }
    if (sHostPointerCount < 64) sHostPointers[sHostPointerCount] = p;
    return OTHER_TOKEN + sHostPointerCount++;
}
/* Texture-cache tokens are the low 32 bits of arena pointers. */
void *dkr_lo32_to_ptr(u32 lo32) {
    return lo32 ? (void *)(((uintptr_t)sArena & ~(uintptr_t)0xFFFFFFFFu) | lo32) : NULL;
}
void dkr_dl_register_host_ptr(const void *x) { (void)x; }
/* tex_free asks whether asset refcounts are frozen for a rollback replay;
 * this unit is never replaying. */
bool mdkr_rollback_game_runtime_presentation_allowed(void) { return true; }
/* tex_free's pool release: the fixtures own their memory. */
static unsigned sPoolFrees;
void mempool_free(void *data) { (void)data; ++sPoolFrees; }
int mdkr_trace_level(void) { return 0; }
void mdkr_trace(const char *fmt, ...) { (void)fmt; }

typedef struct TileState {
    unsigned tmem, line, cms, cmt, masks, maskt, siz, uls, ult, lrs, lrt;
} TileState;

typedef struct TmemModel {
    long long word_source[512]; /* arena byte offset of each loaded 64-bit word */
    const unsigned char *word_host[512]; /* the same word as a host address */
    TileState tile[8];
    unsigned timg;
    unsigned loads;
    long long load_begin[8], load_end[8];
    bool load_in_arena[8];
    const unsigned char *load_host[8];
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
            const bool in_arena = m->timg >= ARENA_TOKEN && m->timg < OTHER_TOKEN;
            const long long source = in_arena ? (long long)m->timg - ARENA_TOKEN : -1000000;
            const unsigned char *host = in_arena ? sArena + source
                : (m->timg - OTHER_TOKEN < sHostPointerCount ? sHostPointers[m->timg - OTHER_TOKEN] : NULL);
            if (m->loads < 8) {
                m->load_begin[m->loads] = source;
                m->load_end[m->loads] = source + bytes;
                m->load_in_arena[m->loads] = in_arena;
                m->load_host[m->loads] = host;
            }
            ++m->loads;
            for (unsigned b = 0; b < bytes; b += 8) {
                const unsigned word = load->tmem + b / 8;
                if (word < 512) {
                    m->word_source[word] = source + b;
                    m->word_host[word] = host ? host + b : NULL;
                }
            }
        }
    }
}

/* TMEM byte address render tile `tile` samples at texel (s, t). */
static unsigned model_tmem_byte(const TmemModel *m, unsigned tile, unsigned s, unsigned t) {
    const TileState *ts = &m->tile[tile];
    const unsigned width = (ts->lrs >> 2) + 1, height = (ts->lrt >> 2) + 1;
    /* RDP rule: a mask wraps; with no mask only the clamp bit holds a
     * coordinate inside the tile, otherwise it walks on through TMEM. */
    if (ts->masks) s &= (1u << ts->masks) - 1;
    else if ((ts->cms & G_TX_CLAMP) && s >= width) s = width - 1;
    if (ts->maskt) t &= (1u << ts->maskt) - 1;
    else if ((ts->cmt & G_TX_CLAMP) && t >= height) t = height - 1;
    return ts->tmem * 8 + t * ts->line * 8 + s * 2;
}

/* Source arena offset sampled by render tile `tile` at texel (s, t). */
static long long model_sample(const TmemModel *m, unsigned tile, unsigned s, unsigned t) {
    const unsigned byte = model_tmem_byte(m, tile, s, t);
    if (byte / 8 >= 512 || m->word_source[byte / 8] < 0) return -1;
    return m->word_source[byte / 8] + byte % 8;
}

/* The host address of the texel render tile `tile` samples at (s, t). */
static const unsigned char *model_sample_host(const TmemModel *m, unsigned tile, unsigned s, unsigned t) {
    const unsigned byte = model_tmem_byte(m, tile, s, t);
    if (byte / 8 >= 512 || m->word_host[byte / 8] == NULL) return NULL;
    return m->word_host[byte / 8] + byte % 8;
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
        EXPECT(m.load_in_arena[i], "%ux%u: load %u is not from the texture", width, height, i);
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

/* ---- Spaceport Alpha's retail ring and its console neighbour ------------ */

extern s32 *gTextureCache;
extern s32 gNumberOfLoadedTextures;

#define RING_ID (0x8000 | 211)
#define NEIGHBOUR_ID (0x8000 | 880)
#define RING_CRC 0x52FCDA79u
#define NEIGHBOUR_CRC 0xF5DF41BFu
#define NEIGHBOUR_OFFSET 1184u /* console: ring texels + Gfx list + leftover + header */

static uint32_t crc32_bytes(const unsigned char *bytes, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* Choose the last four bytes so the CRC-32 of all `length` bytes is `target`.
 * Reflected CRC-32 over four bytes is an invertible map of (state ^ bytes). */
static void forge_crc(unsigned char *bytes, size_t length, uint32_t target) {
    uint32_t state = 0xFFFFFFFFu, want = target ^ 0xFFFFFFFFu;
    for (size_t i = 0; i + 4 < length; ++i) {
        state ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) state = (state >> 1) ^ (0xEDB88320u & (0u - (state & 1u)));
    }
    for (int round = 0; round < 32; ++round) {
        want = (want & 0x80000000u) ? ((want ^ 0xEDB88320u) << 1) | 1u : want << 1;
    }
    const uint32_t tail = want ^ state;
    for (int i = 0; i < 4; ++i) bytes[length - 4 + i] = (unsigned char)(tail >> (8 * i));
}

typedef struct RingFixture {
    unsigned char *ring, *neighbour;
    s32 cache[4];
} RingFixture;

/* Fixtures are never freed until the end, so no two share an address: the
 * production image is keyed by the two texture pointers it was built from.
 * Every arena a fixture allocated is freed by release_fixtures(). */
static unsigned char *sRetired[16];
static unsigned sRetiredCount;

static void release_fixtures(void) {
    for (unsigned i = 0; i < sRetiredCount; ++i) free(sRetired[i]);
    sRetiredCount = 0;
    free(sArena);
    sArena = NULL;
    sArenaSize = 0;
}

/* Both textures in one arena allocation, listed in the texture cache. */
static RingFixture make_ring_fixture(unsigned neighbourWidth) {
    RingFixture f;
    const size_t ringStride = 32 + 1024, neighbourStride = 32 + 4096;
    if (sArena != NULL) {
        if (sRetiredCount == 16) {
            fprintf(stderr, "FAIL: more than 16 ring fixtures\n");
            exit(2);
        }
        sRetired[sRetiredCount++] = sArena;
    }
    sArenaSize = ringStride + neighbourStride;
    sArena = calloc(1, sArenaSize);
    if (!sArena) { perror("calloc"); exit(2); }
    f.ring = sArena;
    f.neighbour = sArena + ringStride;
    TextureHeader *ring = (TextureHeader *)f.ring, *neighbour = (TextureHeader *)f.neighbour;
    ring->width = 16; ring->height = 32; ring->format = 0x11;
    ring->numOfTextures = 1 << 8; ring->textureSize = (s16)ringStride;
    neighbour->width = (u8)neighbourWidth; neighbour->height = (u8)(2048 / neighbourWidth); neighbour->format = 0x11;
    neighbour->numOfTextures = 1 << 8; neighbour->textureSize = (s16)neighbourStride;
    for (size_t i = 0; i < 1024; ++i) f.ring[32 + i] = (unsigned char)(i * 7 + 3);
    for (size_t i = 0; i < 4096; ++i) f.neighbour[32 + i] = (unsigned char)(i * 13 + 101);
    forge_crc(f.ring + 32, 1024, RING_CRC);
    forge_crc(f.neighbour + 32, 4096, NEIGHBOUR_CRC);
    f.cache[0] = RING_ID;
    f.cache[1] = (s32)(u32)(uintptr_t)f.ring;
    f.cache[2] = NEIGHBOUR_ID;
    f.cache[3] = (s32)(u32)(uintptr_t)f.neighbour;
    return f;
}

/* Which plan a fixture's draw took: 1 = the console image, 0 = the wrapped
 * plan inside the ring, -1 = anything else. */
static int ring_plan(RingFixture *f, Gfx *dl, size_t *count, TmemModel *m) {
    gTextureCache = f->cache;
    gNumberOfLoadedTextures = 2;
    sHostPointerCount = 0;
    *count = emit(f->ring, RENDER_Z_COMPARE | RENDER_FOG_ACTIVE | RENDER_PULSING_LIGHTS, 0, dl, 64);
    model_run(m, dl, dl + *count);
    gTextureCache = NULL;
    gNumberOfLoadedTextures = 0;
    if (m->loads != 2) return -1;
    if (m->load_in_arena[0] && m->load_in_arena[1] && m->load_begin[0] >= 32 &&
        m->load_end[0] <= 32 + 1024 && m->load_begin[1] >= 32 && m->load_end[1] <= 32 + 1024) return 0;
    if (!m->load_in_arena[0] && !m->load_in_arena[1] && m->load_host[0] && m->load_host[1] &&
        m->load_host[1] == m->load_host[0] + 2048 && m->load_end[0] - m->load_begin[0] == 2048 &&
        m->load_end[1] - m->load_begin[1] == 2048) return 1;
    return -1;
}

static void check_console_neighbour(void) {
    Gfx dl[64];
    TmemModel m;
    size_t count;
    RingFixture f = make_ring_fixture(32);
    EXPECT(crc32_bytes(f.ring + 32, 1024) == RING_CRC &&
           crc32_bytes(f.neighbour + 32, 4096) == NEIGHBOUR_CRC, "fixture CRCs forged");

    EXPECT(ring_plan(&f, dl, &count, &m) == 1,
           "the verified ring with its verified neighbour draws from one owned 4096-byte image");
    /* The console's view of that memory: ring texels, zero fill, neighbour. */
    unsigned char console[4096];
    memcpy(console, f.ring + 32, 1024);
    memset(console + 1024, 0, NEIGHBOUR_OFFSET - 1024);
    memcpy(console + NEIGHBOUR_OFFSET, f.neighbour + 32, 4096 - NEIGHBOUR_OFFSET);
    unsigned mismatches = 0, neighbourSamples = 0;
    for (unsigned tile = 0; tile < 2; ++tile) {
        for (unsigned t = 0; t < 64; ++t) {
            for (unsigned s = 0; s < 64; ++s) {
                const size_t retail = (tile == 1 ? 0u : 2048u) + ((t % 32) * 32 + (s % 32)) * 2u;
                const unsigned char *sampled = model_sample_host(&m, tile, s, t);
                if (retail >= NEIGHBOUR_OFFSET) ++neighbourSamples;
                if (sampled == NULL || memcmp(sampled, console + retail, 2) != 0) {
                    if (mismatches++ < 4) {
                        fprintf(stderr, "  console image tile %u (%u,%u): retail byte %zu not reproduced\n",
                                tile, s, t, retail);
                    }
                }
            }
        }
    }
    EXPECT(mismatches == 0, "%u texel samples differ from the console's memory layout", mismatches);
    EXPECT(neighbourSamples > 0, "the neighbour's texels are sampled");
    /* The commands are retail's, word for word apart from the two addresses. */
    {
        Gfx retail[32];
        Gfx *r = retail;
        memset(retail, 0, sizeof(retail));
        gDPLoadMultiBlock(r++, 0, 256, 1, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, 0, 0, 5, 5, 0, 0);
        gDPLoadTextureBlock(r++, 0, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, 0, 0, 5, 5, 0, 0);
        unsigned differing = 0;
        for (Gfx *g = retail, *e = dl; g < r; ++g, ++e) {
            if ((g->words.w0 >> 24) == (unsigned)G_SETTIMG) continue;
            differing += g->words.w0 != e->words.w0 || g->words.w1 != e->words.w1;
        }
        EXPECT(differing == 0, "%u retail texture commands differ for the ring", differing);
    }

    /* Controls: every broken identity falls back to the wrapped plan. */
    f = make_ring_fixture(32);
    f.cache[2] = -1;
    EXPECT(ring_plan(&f, dl, &count, &m) == 0, "no neighbour loaded: wrapped plan");
    f = make_ring_fixture(32);
    f.neighbour[32 + 2000] ^= 1;
    EXPECT(ring_plan(&f, dl, &count, &m) == 0, "neighbour content changed: wrapped plan");
    f = make_ring_fixture(32);
    f.ring[32 + 10] ^= 1;
    EXPECT(ring_plan(&f, dl, &count, &m) == 0, "ring content changed: wrapped plan");
    f = make_ring_fixture(32);
    f.cache[0] = 0x8000 | 212;
    EXPECT(ring_plan(&f, dl, &count, &m) == 0, "ring listed under another asset: wrapped plan");
    f = make_ring_fixture(64);
    EXPECT(ring_plan(&f, dl, &count, &m) == 0, "neighbour of another shape: wrapped plan");
    f = make_ring_fixture(32);
    f.cache[1] = (s32)(u32)(uintptr_t)f.neighbour;
    EXPECT(ring_plan(&f, dl, &count, &m) == 0, "another texture listed as the ring: wrapped plan");

    /* The image is keyed by the two texture pointers. Freeing either texture
     * must drop it, so whatever the pool puts at that address next is checked
     * again: here the "same" neighbour comes back with other contents. */
    f = make_ring_fixture(32);
    EXPECT(ring_plan(&f, dl, &count, &m) == 1, "a fresh verified pair builds the image");
    f.neighbour[32 + 3000] ^= 0x5A;
    EXPECT(ring_plan(&f, dl, &count, &m) == 1,
           "control: unchanged pointers reuse the image without re-reading the texels");
    gTextureCache = f.cache;
    gNumberOfLoadedTextures = 2;
    ((TextureHeader *)f.neighbour)->numberOfInstances = 1;
    sPoolFrees = 0;
    tex_free((TextureHeader *)f.neighbour);
    EXPECT(sPoolFrees == 1 && f.cache[2] == -1, "tex_free released the neighbour");
    f.cache[2] = NEIGHBOUR_ID; /* reloaded at the same address */
    f.cache[3] = (s32)(u32)(uintptr_t)f.neighbour;
    EXPECT(ring_plan(&f, dl, &count, &m) == 0,
           "after the neighbour is freed its address is checked again (the CRC now fails)");
    /* A refusal is cached the same way, until the next free and reload. */
    f.neighbour[32 + 3000] ^= 0x5A;
    gTextureCache = f.cache;
    gNumberOfLoadedTextures = 2;
    ((TextureHeader *)f.neighbour)->numberOfInstances = 1;
    tex_free((TextureHeader *)f.neighbour);
    f.cache[2] = NEIGHBOUR_ID;
    f.cache[3] = (s32)(u32)(uintptr_t)f.neighbour;
    EXPECT(ring_plan(&f, dl, &count, &m) == 1, "a reloaded genuine neighbour verifies again");
    material_pulse_ring_forget();
}

int main(void) {
    check_model_control();
    check_console_neighbour();
    release_fixtures();
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
         "the texture; the verified Spaceport ring samples the console's neighbour image");
    return 0;
}
