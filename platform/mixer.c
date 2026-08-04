/**
 * mixer.c — RSP audio microcode emulator for the GoldenEye 007 PC port.
 *
 * Emulates the N64's RSP audio processing commands in software.
 * The game's libultra audio synthesis chain (synthesizer, sequence player,
 * ADPCM decoder, resampler, envelope mixer) generates these commands via
 * macros that now call directly into this file instead of building Acmd
 * packets for the RSP hardware.
 *
 * Adapted from fgsfdsfgs/perfect_dark port/src/mixer.c.
 * Adjusted for GoldenEye's old libaudio API which uses aSetBuffer to
 * configure DMEM addresses before each operation.
 *
 * Reference: N64 RSP audio microcode (aspMain)
 */

/* ultra64.h FIRST: on macOS <string.h>/<strings.h> turn bcopy/bcmp/bzero into
 * _FORTIFY_SOURCE builtin macros, which then break os_libc.h's plain function
 * declarations of the same names. Declaring them via os_libc.h before the
 * system headers apply their macros avoids the clash (matches stubs_dkr.c). */
#include <ultra64.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "endian_utils.h"
#include "fs_utf8.h"
#include "mixer.h"

/* ===== Constants ===== */

#define DMEM_SIZE       8192    /* Double N64 DMEM to prevent overflow on port */
#define NUM_SAMPLES     0xB8    /* 184 samples per sub-frame (AL_MAX_RSP_SAMPLES = 160, padded) */
#define NUM_BYTES       0x170   /* NUM_SAMPLES * 2 */

/* Standard DMEM offsets (from synthInternals.h) */
#define OFS_BASE        0x000
#define OFS_DECODER_OUT 0x140
#define OFS_MAIN_L      0x440   /* AL_MAIN_L_OUT */
#define OFS_MAIN_R      0x580   /* AL_MAIN_R_OUT */
#define OFS_AUX_L       0x6C0   /* AL_AUX_L_OUT  */
#define OFS_AUX_R       0x800   /* AL_AUX_R_OUT  */

#define MIXER_ENV_SAMPLE_XOR_DEFAULT 0
#define MIXER_POLE_SAMPLE_XOR_DEFAULT 0

/* ===== Helpers ===== */

#define ROUND_UP_64(v) (((v) + 63) & ~63)
#define ROUND_UP_32(v) (((v) + 31) & ~31)
#define ROUND_UP_16(v) (((v) + 15) & ~15)
#define ROUND_UP_8(v)  (((v) + 7)  & ~7)
#define ROUND_DOWN_16(v) ((v) & ~0xf)

#define BUF_U8(a)  (rspa.buf.as_u8  + (a))
#define BUF_S16(a) (rspa.buf.as_s16 + (a) / sizeof(int16_t))

static inline int16_t clamp16(int32_t v) {
    if (v < -0x8000) return -0x8000;
    if (v >  0x7fff) return  0x7fff;
    return (int16_t)v;
}

static inline int16_t clamp16Counted(int32_t v, uint32_t *hits) {
    if (v < -0x8000) {
        if (hits != NULL) (*hits)++;
        return -0x8000;
    }
    if (v > 0x7fff) {
        if (hits != NULL) (*hits)++;
        return 0x7fff;
    }
    return (int16_t)v;
}

static inline uint32_t abs16_peak(int16_t v) {
    return (v < 0) ? (uint32_t)(-(int32_t)v) : (uint32_t)v;
}

/* Sign-extend 4-bit ADPCM nibble to int (branchless, no UB) */
static inline int adpcm_sext4(int v) {
    return ((v & 0xf) ^ 8) - 8;
}

static uint8_t mixerEnvFlagBit(const char *name, uint8_t fallback) {
    const char *value = getenv(name);

    if (value == NULL || *value == '\0') {
        return fallback & 1;
    }

    return (uint8_t)((strtol(value, NULL, 0) != 0) ? 1 : 0);
}

static uint8_t mixerRaw16LegacyMode(void) {
    const char *value = getenv("MDKR_RAW16");

    if (value == NULL || *value == '\0' || strcmp(value, "fixed") == 0) {
        return 0;
    }
    if (strcmp(value, "legacy") == 0) {
        return 1;
    }
    fprintf(stderr,
            "[AUDIO] ignoring invalid MDKR_RAW16='%s' (expected fixed|legacy); "
            "using fixed\n",
            value);
    return 0;
}

static FILE *mixerPoleTraceFile(void) {
    static int initialized = 0;
    static FILE *fp = NULL;
    const char *path;

    if (initialized) {
        return fp;
    }

    initialized = 1;
    path = getenv("GE007_AUDIO_POLE_TRACE_JSONL");
    if (path != NULL && *path != '\0') {
        fp = mdkr_fopen_utf8(path, "w");
    }

    return fp;
}

static uint8_t mixerPoleSectionXor(int16_t fc, uint8_t fallback) {
    static int initialized = 0;
    static int has_mask = 0;
    static long mask = 0;
    int section = -1;

    if (!initialized) {
        const char *value = getenv("GE007_MIXER_POLE_FC_XOR_MASK");

        initialized = 1;
        if (value != NULL && *value != '\0') {
            mask = strtol(value, NULL, 0);
            has_mask = 1;
        }
    }

    if (!has_mask) {
        return fallback & 1;
    }

    switch (fc) {
    case 4736: section = 0; break;
    case 6144: section = 1; break;
    case 6784: section = 2; break;
    case 8192: section = 3; break;
    case 8832: section = 4; break;
    default: break;
    }

    if (section < 0) {
        return fallback & 1;
    }

    return (uint8_t)((mask >> section) & 1);
}

/* ===== RSP emulator state ===== */

static struct {
    /* Volume / envelope state (set by aSetVolume, used by aEnvMixer) */
    int16_t vol[2];
    int16_t target[2];
    int32_t rate[2];
    int16_t vol_dry;
    int16_t vol_wet;

    /* ADPCM loop state (set by aSetLoop) */
    ADPCM_STATE *adpcm_loop_state;

    /* ADPCM codebook (set by aLoadADPCM) */
    int16_t adpcm_table[8][2][8];

    /* aSetBuffer state — old libaudio sets these before each operation */
    uint16_t sb_dmemin;
    uint16_t sb_dmemout;
    uint16_t sb_count;

    /* A_AUX buffer state (for envmixer: main R, aux L, aux R) */
    uint16_t sb_aux_dmemin;   /* main R output */
    uint16_t sb_aux_dmemout;  /* aux L output  */
    uint16_t sb_aux_count;    /* aux R output  */

    /* Segment table (only segment 0 is used in practice) */
    uintptr_t segments[16];

    /* Runtime validation counters for custom FX DSP coverage. */
    PortMixerStats stats;
    uint8_t env_sample_xor;
    uint8_t pole_sample_xor;
    uint8_t disable_pole_filter;
    uint8_t raw16_legacy;

    /* Virtual DMEM */
    union {
        int16_t as_s16[DMEM_SIZE / sizeof(int16_t)];
        uint8_t as_u8[DMEM_SIZE];
    } buf;
} rspa;

/* ===== Polyphase FIR resample table (same as N64 RSP microcode) ===== */

static int16_t resample_table[64][4] = {
    {0x0c39, 0x66ad, 0x0d46, 0xffdf}, {0x0b39, 0x6696, 0x0e5f, 0xffd8},
    {0x0a44, 0x6669, 0x0f83, 0xffd0}, {0x095a, 0x6626, 0x10b4, 0xffc8},
    {0x087d, 0x65cd, 0x11f0, 0xffbf}, {0x07ab, 0x655e, 0x1338, 0xffb6},
    {0x06e4, 0x64d9, 0x148c, 0xffac}, {0x0628, 0x643f, 0x15eb, 0xffa1},
    {0x0577, 0x638f, 0x1756, 0xff96}, {0x04d1, 0x62cb, 0x18cb, 0xff8a},
    {0x0435, 0x61f3, 0x1a4c, 0xff7e}, {0x03a4, 0x6106, 0x1bd7, 0xff71},
    {0x031c, 0x6007, 0x1d6c, 0xff64}, {0x029f, 0x5ef5, 0x1f0b, 0xff56},
    {0x022a, 0x5dd0, 0x20b3, 0xff48}, {0x01be, 0x5c9a, 0x2264, 0xff3a},
    {0x015b, 0x5b53, 0x241e, 0xff2c}, {0x0101, 0x59fc, 0x25e0, 0xff1e},
    {0x00ae, 0x5896, 0x27a9, 0xff10}, {0x0063, 0x5720, 0x297a, 0xff02},
    {0x001f, 0x559d, 0x2b50, 0xfef4}, {0xffe2, 0x540d, 0x2d2c, 0xfee8},
    {0xffac, 0x5270, 0x2f0d, 0xfedb}, {0xff7c, 0x50c7, 0x30f3, 0xfed0},
    {0xff53, 0x4f14, 0x32dc, 0xfec6}, {0xff2e, 0x4d57, 0x34c8, 0xfebd},
    {0xff0f, 0x4b91, 0x36b6, 0xfeb6}, {0xfef5, 0x49c2, 0x38a5, 0xfeb0},
    {0xfedf, 0x47ed, 0x3a95, 0xfeac}, {0xfece, 0x4611, 0x3c85, 0xfeab},
    {0xfec0, 0x4430, 0x3e74, 0xfeac}, {0xfeb6, 0x424a, 0x4060, 0xfeaf},
    {0xfeaf, 0x4060, 0x424a, 0xfeb6}, {0xfeac, 0x3e74, 0x4430, 0xfec0},
    {0xfeab, 0x3c85, 0x4611, 0xfece}, {0xfeac, 0x3a95, 0x47ed, 0xfedf},
    {0xfeb0, 0x38a5, 0x49c2, 0xfef5}, {0xfeb6, 0x36b6, 0x4b91, 0xff0f},
    {0xfebd, 0x34c8, 0x4d57, 0xff2e}, {0xfec6, 0x32dc, 0x4f14, 0xff53},
    {0xfed0, 0x30f3, 0x50c7, 0xff7c}, {0xfedb, 0x2f0d, 0x5270, 0xffac},
    {0xfee8, 0x2d2c, 0x540d, 0xffe2}, {0xfef4, 0x2b50, 0x559d, 0x001f},
    {0xff02, 0x297a, 0x5720, 0x0063}, {0xff10, 0x27a9, 0x5896, 0x00ae},
    {0xff1e, 0x25e0, 0x59fc, 0x0101}, {0xff2c, 0x241e, 0x5b53, 0x015b},
    {0xff3a, 0x2264, 0x5c9a, 0x01be}, {0xff48, 0x20b3, 0x5dd0, 0x022a},
    {0xff56, 0x1f0b, 0x5ef5, 0x029f}, {0xff64, 0x1d6c, 0x6007, 0x031c},
    {0xff71, 0x1bd7, 0x6106, 0x03a4}, {0xff7e, 0x1a4c, 0x61f3, 0x0435},
    {0xff8a, 0x18cb, 0x62cb, 0x04d1}, {0xff96, 0x1756, 0x638f, 0x0577},
    {0xffa1, 0x15eb, 0x643f, 0x0628}, {0xffac, 0x148c, 0x64d9, 0x06e4},
    {0xffb6, 0x1338, 0x655e, 0x07ab}, {0xffbf, 0x11f0, 0x65cd, 0x087d},
    {0xffc8, 0x10b4, 0x6626, 0x095a}, {0xffd0, 0x0f83, 0x6669, 0x0a44},
    {0xffd8, 0x0e5f, 0x6696, 0x0b39}, {0xffdf, 0x0d46, 0x66ad, 0x0c39}
};

/* ===== Initialization ===== */

void mixerInit(void) {
    memset(&rspa, 0, sizeof(rspa));
    rspa.env_sample_xor = mixerEnvFlagBit("GE007_MIXER_ENV_SAMPLE_XOR",
                                          MIXER_ENV_SAMPLE_XOR_DEFAULT);
    rspa.pole_sample_xor = mixerEnvFlagBit("GE007_MIXER_POLE_SAMPLE_XOR",
                                           MIXER_POLE_SAMPLE_XOR_DEFAULT);
    rspa.disable_pole_filter =
        mixerEnvFlagBit("GE007_DISABLE_NATIVE_POLE_FILTER", 0);
    /* Same-binary positive control for the pre-fix host-endian interpretation.
     * The production default is the faithful big-endian PCM conversion. */
    rspa.raw16_legacy = mixerRaw16LegacyMode();
    rspa.stats.envSampleXor = rspa.env_sample_xor;
    rspa.stats.poleSampleXor = rspa.pole_sample_xor;
}

/* ===== Segment addressing ===== */

void mixerSegment(unsigned int seg, unsigned int base) {
    if (seg < 16)
        rspa.segments[seg] = (uintptr_t)base;
}

/* ===== Buffer setup (old libaudio two-step pattern) ===== */

void mixerSetBuffer(unsigned int flags, unsigned int dmemin,
                    unsigned int dmemout, unsigned int count) {
    if (flags & A_AUX) {
        /* A_AUX: dmemin=main_R, dmemout=aux_L, count=aux_R */
        rspa.sb_aux_dmemin  = (uint16_t)dmemin;
        rspa.sb_aux_dmemout = (uint16_t)dmemout;
        rspa.sb_aux_count   = (uint16_t)count;
    } else {
        /* A_MAIN: standard buffer setup */
        rspa.sb_dmemin  = (uint16_t)dmemin;
        rspa.sb_dmemout = (uint16_t)dmemout;
        rspa.sb_count   = (uint16_t)count;
    }
}

/* ===== Clear buffer ===== */

void mixerClearBuffer(unsigned int dmem, unsigned int count) {
    count = ROUND_UP_16(count);
    if (dmem + count <= DMEM_SIZE)
        memset(BUF_U8(dmem), 0, count);
}

/* ===== Load buffer (DRAM → DMEM) ===== */

void mixerLoadBuffer(const void *addr) {
    uint16_t nbytes = ROUND_UP_8(rspa.sb_count);
    if (addr && nbytes > 0 && rspa.sb_dmemin + nbytes <= DMEM_SIZE)
        memcpy(BUF_U8(rspa.sb_dmemin), addr, nbytes);
}

/* Like mixerLoadBuffer, but converts each big-endian 16-bit word to host order.
 * Used only for RAW16 (uncompressed PCM) sample data, which is stored
 * big-endian in the N64 ROM and read back from DMEM as host int16 by the
 * resampler. ADPCM loads must NOT use this (their data is a byte stream). */
void mixerLoadBufferSwap16(const void *addr) {
    uint16_t nbytes = ROUND_UP_8(rspa.sb_count);
    if (!addr || nbytes == 0 || rspa.sb_dmemin + nbytes > DMEM_SIZE)
        return;
    if (rspa.raw16_legacy) {
        memcpy(BUF_U8(rspa.sb_dmemin), addr, nbytes);
    } else {
        if (!mdkr_copy_be16_to_host(BUF_U8(rspa.sb_dmemin),
                                    DMEM_SIZE - rspa.sb_dmemin,
                                    addr, nbytes)) {
            return;
        }
    }
    rspa.stats.raw16LoadCalls++;
    rspa.stats.raw16LoadBytes += nbytes;
}

/* ===== Save buffer (DMEM → DRAM) ===== */

void mixerSaveBuffer(void *addr) {
    uint16_t nbytes = ROUND_UP_8(rspa.sb_count);
    if (!addr || nbytes == 0) return;
    if (rspa.sb_dmemout + nbytes <= DMEM_SIZE) {
        memcpy(addr, BUF_U8(rspa.sb_dmemout), nbytes);
        rspa.stats.saveBufferCalls++;
        rspa.stats.saveBufferBytes += nbytes;
        if (rspa.sb_dmemout != 0) {
            rspa.stats.saveBufferDmemoutCalls++;
        }
    }
}

/* ===== Load ADPCM codebook ===== */

void mixerLoadADPCM(unsigned int count, const void *addr) {
    if (!addr || count == 0 || count > sizeof(rspa.adpcm_table)) return;
    /* Use byte-by-byte copy to avoid SIGBUS on misaligned source */
    {
        u8 *dst = (u8 *)rspa.adpcm_table;
        const u8 *src = (const u8 *)addr;
        unsigned int i;
        for (i = 0; i < count; i++) dst[i] = src[i];
    }
}

/* ===== Set loop state ===== */

void mixerSetLoop(void *state) {
    rspa.adpcm_loop_state = (ADPCM_STATE *)state;
}

/* ===== DMEM move ===== */

void mixerDMEMMove(unsigned int dmemin, unsigned int dmemout,
                   unsigned int count) {
    count = ROUND_UP_16(count);
    if (dmemin + count <= DMEM_SIZE && dmemout + count <= DMEM_SIZE)
        memmove(BUF_U8(dmemout), BUF_U8(dmemin), count);
}

/* ===== Interleave L/R channels ===== */

void mixerInterleave(unsigned int left, unsigned int right) {
    int16_t *l = BUF_S16(left);
    int16_t *r = BUF_S16(right);
    int count = ROUND_UP_16(rspa.sb_count / 2) / 8;
    int16_t *d = BUF_S16(rspa.sb_dmemin);  /* output goes to sb_dmemin (set as 0 by save.c) */

    while (count > 0) {
        int16_t l0 = *l++, l1 = *l++, l2 = *l++, l3 = *l++;
        int16_t l4 = *l++, l5 = *l++, l6 = *l++, l7 = *l++;
        int16_t r0 = *r++, r1 = *r++, r2 = *r++, r3 = *r++;
        int16_t r4 = *r++, r5 = *r++, r6 = *r++, r7 = *r++;
        *d++ = l0; *d++ = r0; *d++ = l1; *d++ = r1;
        *d++ = l2; *d++ = r2; *d++ = l3; *d++ = r3;
        *d++ = l4; *d++ = r4; *d++ = l5; *d++ = r5;
        *d++ = l6; *d++ = r6; *d++ = l7; *d++ = r7;
        --count;
    }
}

/* ===== ADPCM Decoder =====
 *
 * Decodes VADPCM compressed audio.  Uses the codebook loaded by
 * aLoadADPCM and the state buffer (16 × s16) for prediction history.
 * Supports A_INIT (zero history), A_LOOP (restore loop state), and
 * A_CONTINUE (use state from previous call).
 *
 * Uses sb_dmemin/sb_dmemout/sb_count from the preceding aSetBuffer.
 */

void mixerADPCMdec(unsigned int flags, void *state) {
    uint8_t *in  = BUF_U8(rspa.sb_dmemin);
    int16_t *out = BUF_S16(rspa.sb_dmemout);
    int nbytes   = ROUND_UP_32(rspa.sb_count);

    /* Safety: ensure we don't read/write outside DMEM */
    if (rspa.sb_dmemin >= DMEM_SIZE || rspa.sb_dmemout >= DMEM_SIZE) return;
    if (nbytes <= 0 || nbytes > 2048) return;
    rspa.stats.adpcmDecCalls++;

    if (flags & A_INIT) {
        memset(out, 0, 16 * sizeof(int16_t));
    } else if (flags & A_LOOP) {
        memcpy(out, rspa.adpcm_loop_state, 16 * sizeof(int16_t));
    } else {
        memcpy(out, state, 16 * sizeof(int16_t));
    }

    out += 16;

    while (nbytes > 0) {
        int shift = *in >> 4;
        int table_index = *in++ & 0xf;
        int16_t (*tbl)[8] = rspa.adpcm_table[table_index & 7];
        int i, j, k;

        for (i = 0; i < 2; i++) {
            int16_t ins[8];
            int16_t prev1 = out[-1];
            int16_t prev2 = out[-2];

            for (j = 0; j < 4; j++) {
                ins[j * 2]     = (int16_t)((uint32_t)adpcm_sext4(*in >> 4) << shift);
                ins[j * 2 + 1] = (int16_t)((uint32_t)adpcm_sext4(*in++) << shift);
            }

            for (j = 0; j < 8; j++) {
                int32_t acc = tbl[0][j] * prev2 + tbl[1][j] * prev1
                            + (int32_t)((uint32_t)ins[j] << 11);
                for (k = 0; k < j; k++) {
                    acc += tbl[1][((j - k) - 1)] * ins[k];
                }
                acc >>= 11;
                *out++ = clamp16Counted(acc, &rspa.stats.adpcmClampHits);
            }
        }

        nbytes -= 16 * sizeof(int16_t);
    }

    memcpy(state, out - 16, 16 * sizeof(int16_t));
}

/* ===== Resampler =====
 *
 * Polyphase FIR resampler using 64 phases × 4 taps.
 * Pitch is 16.16 fixed-point (UNITY_PITCH = 0x8000).
 * Uses sb_dmemin (input) and sb_dmemout (output) from aSetBuffer.
 */

void mixerResample(unsigned int flags, unsigned int pitch, void *state) {
    int16_t tmp[16];
    if (rspa.sb_dmemin >= DMEM_SIZE || rspa.sb_dmemout >= DMEM_SIZE) return;
    if (pitch == 0) return;
    int16_t *in_initial = BUF_S16(rspa.sb_dmemin);
    int16_t *in = in_initial;
    int16_t *out = BUF_S16(rspa.sb_dmemout);
    int nbytes = ROUND_UP_16(rspa.sb_count);
    uint32_t pitch_accumulator;
    int i;
    int16_t *tbl;
    int32_t sample;

    rspa.stats.resampleCalls++;

    if (flags & A_INIT) {
        memset(tmp, 0, 5 * sizeof(int16_t));
    } else {
        memcpy(tmp, state, 16 * sizeof(int16_t));
    }

    if (flags & 2) {
        memcpy(in - 8, tmp + 8, 8 * sizeof(int16_t));
        in -= tmp[5] / sizeof(int16_t);
    }
    in -= 4;
    pitch_accumulator = (uint16_t)tmp[4];
    memcpy(in, tmp, 4 * sizeof(int16_t));

    do {
        for (i = 0; i < 8; i++) {
            tbl = resample_table[pitch_accumulator * 64 >> 16];
            sample = ((in[0] * tbl[0] + 0x4000) >> 15) +
                     ((in[1] * tbl[1] + 0x4000) >> 15) +
                     ((in[2] * tbl[2] + 0x4000) >> 15) +
                     ((in[3] * tbl[3] + 0x4000) >> 15);
            *out++ = clamp16Counted(sample, &rspa.stats.resampleClampHits);

            pitch_accumulator += (pitch << 1);
            in += pitch_accumulator >> 16;
            pitch_accumulator %= 0x10000;
        }
        nbytes -= 8 * sizeof(int16_t);
    } while (nbytes > 0);

    ((int16_t *)state)[4] = (int16_t)pitch_accumulator;
    memcpy(state, in, 4 * sizeof(int16_t));
    i = (in - in_initial + 4) & 7;
    in -= i;
    if (i != 0)
        i = -8 - i;
    ((int16_t *)state)[5] = i;
    memcpy((int16_t *)state + 8, in, 8 * sizeof(int16_t));
}

/* ===== Set Volume =====
 *
 * Old libaudio calling pattern (separate from naudio):
 *   A_LEFT | A_VOL:   set left volume, dry amount, wet amount
 *   A_RIGHT | A_VOL:  set right volume
 *   A_LEFT | A_RATE:  set left target and rate
 *   A_RIGHT | A_RATE: set right target and rate
 *   A_AUX:            set dry and wet amounts
 */

void mixerSetVolume(unsigned int flags, int16_t vol,
                    int16_t voltgt, int16_t volrate) {
    if (flags & A_AUX) {
        rspa.vol_dry = vol;
        rspa.vol_wet = volrate;
    } else if (flags & A_VOL) {
        if (flags & A_LEFT) {
            rspa.vol[0]  = vol;
            rspa.vol_dry = voltgt;
            rspa.vol_wet = volrate;
        } else {
            rspa.vol[1] = vol;
        }
    } else /* A_RATE */ {
        if (flags & A_LEFT) {
            rspa.target[0] = vol;
            rspa.rate[0] = (int32_t)(((uint32_t)(uint16_t)voltgt << 16) | (uint16_t)volrate);
        } else {
            rspa.target[1] = vol;
            rspa.rate[1] = (int32_t)(((uint32_t)(uint16_t)voltgt << 16) | (uint16_t)volrate);
        }
    }
}

/* ===== Envelope Mixer =====
 *
 * Mixes mono input → stereo output with per-sample volume envelopes.
 * Splits output into dry (main L/R) and wet (aux L/R) buses.
 *
 * Old libaudio: buffer addresses come from aSetBuffer(A_MAIN) and
 * aSetBuffer(A_AUX).
 */

void mixerEnvMixer(unsigned int flags, void *state) {
    int16_t *in = BUF_S16(rspa.sb_dmemin);
    int16_t *dry[2] = {
        BUF_S16(rspa.sb_dmemout),       /* main L (from A_MAIN setbuf) */
        BUF_S16(rspa.sb_aux_dmemin)     /* main R (from A_AUX setbuf)  */
    };
    int16_t *wet[2] = {
        BUF_S16(rspa.sb_aux_dmemout),   /* aux L (from A_AUX setbuf) */
        BUF_S16(rspa.sb_aux_count)      /* aux R (from A_AUX setbuf) */
    };
    int nsamples = rspa.sb_count / 2;   /* byte count → sample count */

    struct {
        int32_t t[2];
        int32_t rate[2];
        int16_t tgt[2];
        int16_t voldry;
        int16_t volwet;
    } *savedstate = (void *)state;

    int32_t t[2], tgt[2], rate[2];
    int16_t voldry, volwet;

    rspa.stats.envMixerCalls++;
    rspa.stats.envMixerSampleFrames += (uint32_t)nsamples;

    if (flags & A_INIT) {
        int i;
        for (i = 0; i < 2; ++i) {
            t[i] = (int32_t)((uint32_t)rspa.vol[i] << 16);
            rate[i] = rspa.rate[i] >> 3;
            tgt[i] = (int32_t)((uint32_t)rspa.target[i] << 16);
        }
        voldry = rspa.vol_dry;
        volwet = rspa.vol_wet;
    } else {
        int i;
        for (i = 0; i < 2; ++i) {
            t[i] = savedstate->t[i];
            rate[i] = savedstate->rate[i];
            tgt[i] = (int32_t)((uint32_t)savedstate->tgt[i] << 16);
        }
        voldry = savedstate->voldry;
        volwet = savedstate->volwet;
    }

    {
        int i;
        for (i = 0; i < nsamples; ++i) {
            int sample_idx = i ^ rspa.env_sample_xor;
            int16_t gain[4];
            int16_t vol[2];
            int j;

            for (j = 0; j < 2; ++j) {
                t[j] = (int32_t)((uint32_t)t[j] + (uint32_t)rate[j]);
                if ((rate[j] <= 0 && t[j] <= tgt[j]) ||
                    (rate[j] > 0  && t[j] >= tgt[j])) {
                    t[j] = tgt[j];
                    rate[j] = 0;
                }
                vol[j] = t[j] >> 16;
            }

            gain[0] = clamp16((vol[0] * voldry + 0x4000) >> 15);
            gain[1] = clamp16((vol[1] * voldry + 0x4000) >> 15);
            gain[2] = clamp16((vol[0] * volwet + 0x4000) >> 15);
            gain[3] = clamp16((vol[1] * volwet + 0x4000) >> 15);

            {
                const int16_t insamp = in[sample_idx];
                dry[0][sample_idx] = clamp16Counted(
                    dry[0][sample_idx] + ((insamp * gain[0]) >> 15),
                    &rspa.stats.envMixerClampHits);
                dry[1][sample_idx] = clamp16Counted(
                    dry[1][sample_idx] + ((insamp * gain[1]) >> 15),
                    &rspa.stats.envMixerClampHits);
                wet[0][sample_idx] = clamp16Counted(
                    wet[0][sample_idx] + ((insamp * gain[2]) >> 15),
                    &rspa.stats.envMixerClampHits);
                wet[1][sample_idx] = clamp16Counted(
                    wet[1][sample_idx] + ((insamp * gain[3]) >> 15),
                    &rspa.stats.envMixerClampHits);
            }
        }
    }

    {
        int i;
        for (i = 0; i < 2; ++i) {
            savedstate->t[i] = t[i];
            savedstate->rate[i] = rate[i];
            savedstate->tgt[i] = tgt[i] >> 16;
        }
        savedstate->voldry = voldry;
        savedstate->volwet = volwet;
    }
}

/* ===== Mix =====
 *
 * out[i] += (in[i] * gain) >> 15
 * Uses sb_count for byte count.
 */

void mixerMix(unsigned int flags, int16_t gain,
              unsigned int dmemi, unsigned int dmemo) {
    int nbytes = ROUND_UP_16(rspa.sb_count);
    int16_t *in  = BUF_S16(dmemi);
    int16_t *out = BUF_S16(dmemo);
    /* Master bus? Only saturation HERE is audible clipping of the final mix
     * (everything upstream is a per-voice/aux stage). Tracked for headroom
     * analysis — see PortMixerStats.mainMixOverPeak. */
    const int isMain = (dmemo == OFS_MAIN_L || dmemo == OFS_MAIN_R);
    int i;
    int32_t sample;

    (void)flags;

    rspa.stats.mixCalls++;
    if (isMain) {
        rspa.stats.mainMixSamples += (uint32_t)(nbytes / (int)sizeof(int16_t));
    }

    if (gain == (int16_t)-0x8000) {
        while (nbytes > 0) {
            for (i = 0; i < 8; i++) {
                sample = *out - *in++;
                if (isMain && (sample > 0x7fff || sample < -0x8000)) {
                    uint32_t mag = (sample < 0) ? (uint32_t)(-sample) : (uint32_t)sample;
                    rspa.stats.mainMixClampHits++;
                    if (mag > rspa.stats.mainMixOverPeak) rspa.stats.mainMixOverPeak = mag;
                }
                *out++ = clamp16Counted(sample, &rspa.stats.mixClampHits);
            }
            nbytes -= 8 * sizeof(int16_t);
        }
        return;
    }

    while (nbytes > 0) {
        for (i = 0; i < 8; i++) {
            sample = ((*out * 0x7fff + *in++ * gain) + 0x4000) >> 15;
            if (isMain && (sample > 0x7fff || sample < -0x8000)) {
                uint32_t mag = (sample < 0) ? (uint32_t)(-sample) : (uint32_t)sample;
                rspa.stats.mainMixClampHits++;
                if (mag > rspa.stats.mainMixOverPeak) rspa.stats.mainMixOverPeak = mag;
            }
            *out++ = clamp16Counted(sample, &rspa.stats.mixClampHits);
        }
        nbytes -= 8 * sizeof(int16_t);
    }
}

/* ===== Pole Filter =====
 *
 * Used by libaudio's custom FX low-pass stages.  aLoadADPCM loads the 16
 * filter coefficients immediately before aPoleFilter; the command reuses the
 * ADPCM table DMEM area for those coefficients on hardware.
 */

void mixerPoleFilter(unsigned int flags, int16_t gain, void *state) {
    int nbytes = ROUND_UP_16(rspa.sb_count);
    int nsamples = nbytes / (int)sizeof(int16_t);
    int16_t *in;
    int16_t *out;
    const int16_t *table;
    const int16_t *h1;
    const int16_t *h2_src;
    int16_t h2_original[8];
    int16_t h2_scaled[8];
    int16_t l1;
    int16_t l2;
    int16_t saved_l1;
    int16_t saved_l2;
    uint16_t ugain = (uint16_t)gain;
    uint8_t sample_xor;
    uint32_t input_peak = 0;
    uint32_t peak = 0;
    int i;

    if (state == NULL || nbytes <= 0) return;
    if (rspa.sb_dmemin + nbytes > DMEM_SIZE || rspa.sb_dmemout + nbytes > DMEM_SIZE) return;
    if (rspa.disable_pole_filter) {
        rspa.stats.poleFilterCalls++;
        rspa.stats.poleFilterSampleFrames += (uint32_t)nsamples;
        return;
    }

    in = BUF_S16(rspa.sb_dmemin);
    out = BUF_S16(rspa.sb_dmemout);
    table = &rspa.adpcm_table[0][0][0];
    h1 = table;
    h2_src = table + 8;

    if (flags & A_INIT) {
        l1 = 0;
        l2 = 0;
    } else {
        int16_t *saved = (int16_t *)state;
        l1 = saved[2];
        l2 = saved[3];
    }
    saved_l1 = l1;
    saved_l2 = l2;

    for (i = 0; i < 8; i++) {
        h2_original[i] = h2_src[i];
        h2_scaled[i] = (int16_t)(((int32_t)h2_src[i] * ugain) >> 14);
    }
    sample_xor = mixerPoleSectionXor(h2_original[0], rspa.pole_sample_xor);

    while (nbytes > 0) {
        int16_t frame[8];
        int16_t filtered[8];
        int j;

        for (i = 0; i < 8; i++) {
            frame[i] = in[i ^ sample_xor];
            {
                uint32_t sample_peak = abs16_peak(frame[i]);
                if (sample_peak > input_peak) {
                    input_peak = sample_peak;
                }
            }
        }

        for (i = 0; i < 8; i++) {
            int32_t acc = (int32_t)frame[i] * ugain;
            acc += (int32_t)h1[i] * l1;
            acc += (int32_t)h2_original[i] * l2;
            for (j = 0; j < i; j++) {
                acc += (int32_t)h2_scaled[j] * frame[i - 1 - j];
            }
            filtered[i] = clamp16Counted(acc >> 14, &rspa.stats.poleFilterClampHits);
            out[i ^ sample_xor] = filtered[i];
            {
                uint32_t sample_peak = abs16_peak(filtered[i]);
                if (sample_peak > peak) {
                    peak = sample_peak;
                }
            }
        }

        l1 = filtered[6];
        l2 = filtered[7];
        in += 8;
        out += 8;
        nbytes -= 8 * (int)sizeof(int16_t);
    }

    {
        int16_t *saved = (int16_t *)state;
        saved[2] = l1;
        saved[3] = l2;
    }
    rspa.stats.poleFilterCalls++;
    rspa.stats.poleFilterSampleFrames += (uint32_t)nsamples;
    if (peak > rspa.stats.poleFilterPeak) {
        rspa.stats.poleFilterPeak = peak;
    }
    {
        FILE *trace = mixerPoleTraceFile();
        if (trace != NULL) {
            fprintf(trace,
                    "{\"event\":\"pole_filter\",\"call\":%u,"
                    "\"flags\":%u,\"gain\":%d,\"ugain\":%u,"
                    "\"dmemin\":%u,\"dmemout\":%u,\"count_bytes\":%u,"
                    "\"sample_frames\":%d,\"sample_xor\":%u,"
                    "\"section_sample_xor\":%u,"
                    "\"fc\":%d,\"h2_1\":%d,\"h2_7\":%d,"
                    "\"state_l1_in\":%d,\"state_l2_in\":%d,"
                    "\"state_l1_out\":%d,\"state_l2_out\":%d,"
                    "\"input_peak\":%u,\"output_peak\":%u}\n",
                    rspa.stats.poleFilterCalls,
                    flags,
                    gain,
                    ugain,
                    rspa.sb_dmemin,
                    rspa.sb_dmemout,
                    rspa.sb_count,
                    nsamples,
                    rspa.pole_sample_xor,
                    sample_xor,
                    h2_original[0],
                    h2_original[1],
                    h2_original[7],
                    saved_l1,
                    saved_l2,
                    l1,
                    l2,
                    input_peak,
                    peak);
        }
    }
}

void mixerGetStats(PortMixerStats *out) {
    if (out != NULL) {
        *out = rspa.stats;
    }
}

void mixerGetMainBusClip(uint32_t *hits, uint32_t *overPeak, uint32_t *samples) {
    if (hits != NULL)     *hits     = rspa.stats.mainMixClampHits;
    if (overPeak != NULL) *overPeak = rspa.stats.mainMixOverPeak;
    if (samples != NULL)  *samples  = rspa.stats.mainMixSamples;
}

void mixerGetRaw16Stats(uint32_t *calls, uint64_t *bytes, int *legacy) {
    if (calls != NULL)  *calls = rspa.stats.raw16LoadCalls;
    if (bytes != NULL)  *bytes = rspa.stats.raw16LoadBytes;
    if (legacy != NULL) *legacy = rspa.raw16_legacy != 0;
}
