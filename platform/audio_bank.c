/*
 * Clean-room native audio engine: instrument-bank and sequence-file parsing.
 *
 * Explicit big-endian reads out of the raw ctl/seqfile images into
 * arena-resident host structs, rather than the stock overlay-and-patch on the
 * N64 image. Split out of audio_compat.c; see audio_compat_internal.h.
 */

#include "audio_compat_internal.h"

/*
 * Set by game/src/audio.c before each alBnkfNew/alSeqFileNew* call: the
 * (arena-backed) audio heap parsed banks are allocated from, and the raw ctl
 * image size used for bounds checks. Declared in <libaudio.h>.
 */
ALHeap *gAudioParseHeap = NULL;
u32     gAudioBankCtlSize = 0;

/* Parsed bank structs must be arena-resident — see alHeapDBAlloc's note. */
static void *bank_alloc(s32 size)
{
    return alHeapAlloc(gAudioParseHeap, 1, size);
}

static int bank_ctl_has(u32 size, u32 offset, u32 count)
{
    if (size == 0) {
        return 1; /* size unknown: trust the well-formed retail bank */
    }
    return offset <= size && count <= size - offset;
}

static u16 bank_ctl_u16(const u8 *data)
{
    return ((u16)data[0] << 8) | (u16)data[1];
}

static s16 bank_ctl_s16(const u8 *data)
{
    return (s16)bank_ctl_u16(data);
}

u32 bank_ctl_u32(const u8 *data)
{
    return ((u32)data[0] << 24) | ((u32)data[1] << 16) |
           ((u32)data[2] << 8) | (u32)data[3];
}

static s32 bank_ctl_s32(const u8 *data)
{
    return (s32)bank_ctl_u32(data);
}

f32 audio_exp2f(f32 exponent)
{
#ifdef TARGET_N64
    f32 scale = 1.0f;
    f32 y;
    f32 term;
    f32 sum;
    s32 i;

    while (exponent >= 1.0f) {
        scale *= 2.0f;
        exponent -= 1.0f;
    }

    while (exponent < 0.0f) {
        scale *= 0.5f;
        exponent += 1.0f;
    }

    y = exponent * 0.6931471805599453f;
    term = 1.0f;
    sum = 1.0f;
    for (i = 1; i <= 6; i++) {
        term *= y / (f32)i;
        sum += term;
    }

    return scale * sum;
#else
    return powf(2.0f, exponent);
#endif
}


static ALADPCMBook *bank_make_adpcm_book(const u8 *ctl, u32 ctl_size,
                                         u32 offset)
{
    s32 order;
    s32 predictor_count;
    s32 coefficient_count;
    ALADPCMBook *book;
    s32 i;

    if (!bank_ctl_has(ctl_size, offset, 8)) {
        return NULL;
    }

    order = bank_ctl_s32(ctl + offset);
    predictor_count = bank_ctl_s32(ctl + offset + 4);
    if (order <= 0 || order > 16 ||
        predictor_count <= 0 || predictor_count > 16) {
        return NULL;
    }

    coefficient_count = order * predictor_count * 8;
    if (!bank_ctl_has(ctl_size, offset + 8, (u32)coefficient_count * 2)) {
        return NULL;
    }

    book = bank_alloc((s32)(sizeof(*book) +
                         (u32)(coefficient_count - 1) * sizeof(book->book[0])));
    if (book == NULL) {
        return NULL;
    }

    book->order = order;
    book->npredictors = predictor_count;
    for (i = 0; i < coefficient_count; i++) {
        book->book[i] = bank_ctl_s16(ctl + offset + 8 + (u32)i * 2);
    }

    return book;
}

static ALADPCMloop *bank_make_adpcm_loop(const u8 *ctl, u32 ctl_size,
                                         u32 offset)
{
    ALADPCMloop *loop;
    s32 i;

    if (!bank_ctl_has(ctl_size, offset, 44)) {
        return NULL;
    }

    loop = bank_alloc((s32)sizeof(*loop));
    if (loop == NULL) {
        return NULL;
    }

    loop->start = bank_ctl_u32(ctl + offset);
    loop->end = bank_ctl_u32(ctl + offset + 4);
    loop->count = bank_ctl_u32(ctl + offset + 8);
    for (i = 0; i < 16; i++) {
        loop->state[i] = bank_ctl_s16(ctl + offset + 12 + (u32)i * 2);
    }

    return loop;
}

static ALRawLoop *bank_make_raw_loop(const u8 *ctl, u32 ctl_size, u32 offset)
{
    ALRawLoop *loop;

    if (!bank_ctl_has(ctl_size, offset, 12)) {
        return NULL;
    }

    loop = bank_alloc((s32)sizeof(*loop));
    if (loop == NULL) {
        return NULL;
    }

    loop->start = bank_ctl_u32(ctl + offset);
    loop->end = bank_ctl_u32(ctl + offset + 4);
    loop->count = bank_ctl_u32(ctl + offset + 8);
    return loop;
}

static ALEnvelope *bank_make_envelope(const u8 *ctl, u32 ctl_size, u32 offset)
{
    ALEnvelope *envelope;

    if (!bank_ctl_has(ctl_size, offset, 14)) {
        return NULL;
    }

    envelope = bank_alloc((s32)sizeof(*envelope));
    if (envelope == NULL) {
        return NULL;
    }

    envelope->attackTime = bank_ctl_s32(ctl + offset);
    envelope->decayTime = bank_ctl_s32(ctl + offset + 4);
    envelope->releaseTime = bank_ctl_s32(ctl + offset + 8);
    envelope->attackVolume = ctl[offset + 12];
    envelope->decayVolume = ctl[offset + 13];
    return envelope;
}

static ALKeyMap *bank_make_keymap(const u8 *ctl, u32 ctl_size, u32 offset)
{
    ALKeyMap *keymap;

    if (!bank_ctl_has(ctl_size, offset, 6)) {
        return NULL;
    }

    keymap = bank_alloc((s32)sizeof(*keymap));
    if (keymap == NULL) {
        return NULL;
    }

    keymap->velocityMin = ctl[offset];
    keymap->velocityMax = ctl[offset + 1];
    keymap->keyMin = ctl[offset + 2];
    keymap->keyMax = ctl[offset + 3];
    keymap->keyBase = ctl[offset + 4];
    keymap->detune = (s8)ctl[offset + 5];
    return keymap;
}

static ALWaveTable *bank_make_wavetable(const u8 *ctl, u32 ctl_size,
                                        u32 offset, uintptr_t table_base)
{
    ALWaveTable *wave;
    u32 loop_offset;
    u32 book_offset;

    if (!bank_ctl_has(ctl_size, offset, 12)) {
        return NULL;
    }

    wave = bank_alloc((s32)sizeof(*wave));
    if (wave == NULL) {
        return NULL;
    }

    wave->base = (u8 *)(table_base + bank_ctl_u32(ctl + offset));
    wave->len = bank_ctl_s32(ctl + offset + 4);
    wave->type = ctl[offset + 8];
    wave->flags = 1;

    if (wave->type == AL_ADPCM_WAVE) {
        if (!bank_ctl_has(ctl_size, offset, 20)) {
            free(wave);
            return NULL;
        }
        loop_offset = bank_ctl_u32(ctl + offset + 12);
        book_offset = bank_ctl_u32(ctl + offset + 16);
        if (loop_offset != 0) {
            wave->waveInfo.adpcmWave.loop =
                bank_make_adpcm_loop(ctl, ctl_size, loop_offset);
        }
        if (book_offset != 0) {
            wave->waveInfo.adpcmWave.book =
                bank_make_adpcm_book(ctl, ctl_size, book_offset);
        }
    } else if (wave->type == AL_RAW16_WAVE) {
        if (!bank_ctl_has(ctl_size, offset, 16)) {
            free(wave);
            return NULL;
        }
        loop_offset = bank_ctl_u32(ctl + offset + 12);
        if (loop_offset != 0) {
            wave->waveInfo.rawWave.loop =
                bank_make_raw_loop(ctl, ctl_size, loop_offset);
        }
    }

    return wave;
}

static ALSound *bank_make_sound(const u8 *ctl, u32 ctl_size, u32 offset,
                                uintptr_t table_base)
{
    ALSound *sound;
    u32 envelope_offset;
    u32 keymap_offset;
    u32 wavetable_offset;

    if (!bank_ctl_has(ctl_size, offset, 15)) {
        return NULL;
    }

    sound = bank_alloc((s32)sizeof(*sound));
    if (sound == NULL) {
        return NULL;
    }

    envelope_offset = bank_ctl_u32(ctl + offset);
    keymap_offset = bank_ctl_u32(ctl + offset + 4);
    wavetable_offset = bank_ctl_u32(ctl + offset + 8);
    if (envelope_offset != 0) {
        sound->envelope = bank_make_envelope(ctl, ctl_size, envelope_offset);
    }
    if (keymap_offset != 0) {
        sound->keyMap = bank_make_keymap(ctl, ctl_size, keymap_offset);
    }
    if (wavetable_offset != 0) {
        sound->wavetable =
            bank_make_wavetable(ctl, ctl_size, wavetable_offset, table_base);
    }
    sound->samplePan = ctl[offset + 12];
    sound->sampleVolume = ctl[offset + 13];
    sound->flags = 1;
    return sound;
}

static ALInstrument *bank_make_instrument(const u8 *ctl, u32 ctl_size,
                                          u32 offset, uintptr_t table_base)
{
    ALInstrument *instrument;
    s16 sound_count;
    u32 extra_sounds;
    s32 i;

    if (!bank_ctl_has(ctl_size, offset, 16)) {
        return NULL;
    }

    sound_count = bank_ctl_s16(ctl + offset + 14);
    if (sound_count < 0 ||
        !bank_ctl_has(ctl_size, offset + 16, (u32)sound_count * 4)) {
        return NULL;
    }

    extra_sounds = sound_count > 1 ? (u32)(sound_count - 1) : 0;
    instrument = bank_alloc((s32)(sizeof(*instrument) +
                               extra_sounds *
                                   sizeof(instrument->soundArray[0])));
    if (instrument == NULL) {
        return NULL;
    }

    instrument->volume = ctl[offset];
    instrument->pan = ctl[offset + 1];
    instrument->priority = ctl[offset + 2];
    instrument->flags = 1;
    instrument->tremType = ctl[offset + 4];
    instrument->tremRate = ctl[offset + 5];
    instrument->tremDepth = ctl[offset + 6];
    instrument->tremDelay = ctl[offset + 7];
    instrument->vibType = ctl[offset + 8];
    instrument->vibRate = ctl[offset + 9];
    instrument->vibDepth = ctl[offset + 10];
    instrument->vibDelay = ctl[offset + 11];
    instrument->bendRange = bank_ctl_s16(ctl + offset + 12);
    instrument->soundCount = sound_count;

    for (i = 0; i < sound_count; i++) {
        u32 sound_offset = bank_ctl_u32(ctl + offset + 16 + (u32)i * 4);
        if (sound_offset != 0) {
            instrument->soundArray[i] =
                bank_make_sound(ctl, ctl_size, sound_offset, table_base);
        }
    }

    return instrument;
}

static ALBank *bank_make_bank(const u8 *ctl, u32 ctl_size, u32 offset,
                              uintptr_t table_base)
{
    ALBank *bank;
    s16 instrument_count;
    u32 percussion_offset;
    u32 extra_instruments;
    s32 i;

    if (!bank_ctl_has(ctl_size, offset, 12)) {
        return NULL;
    }

    instrument_count = bank_ctl_s16(ctl + offset);
    if (instrument_count < 0 ||
        !bank_ctl_has(ctl_size, offset + 12, (u32)instrument_count * 4)) {
        return NULL;
    }

    extra_instruments =
        instrument_count > 1 ? (u32)(instrument_count - 1) : 0;
    bank = bank_alloc((s32)(sizeof(*bank) +
                         extra_instruments * sizeof(bank->instArray[0])));
    if (bank == NULL) {
        return NULL;
    }

    bank->instCount = instrument_count;
    bank->flags = 1;
    bank->sampleRate = bank_ctl_s32(ctl + offset + 4);
    percussion_offset = bank_ctl_u32(ctl + offset + 8);
    if (percussion_offset != 0) {
        bank->percussion =
            bank_make_instrument(ctl, ctl_size, percussion_offset, table_base);
    }

    for (i = 0; i < instrument_count; i++) {
        u32 instrument_offset = bank_ctl_u32(ctl + offset + 12 + (u32)i * 4);
        if (instrument_offset != 0) {
            bank->instArray[i] =
                bank_make_instrument(ctl, ctl_size, instrument_offset,
                                     table_base);
        }
    }

    return bank;
}

/*
 * Build a host-native ALSeqFile from a big-endian raw seqfile image. `dst` is a
 * freshly host-laid-out ALSeqFile (sized for seqCount host entries by the
 * caller); `rawBE` is the raw image; `base` is the ROM offset added to each
 * sequence offset, so seqArray[i].offset ends up an absolute ROM offset which
 * music_sequence_init turns back into an ASSET_AUDIO-local offset.
 *
 * This exists because the on-disk record is 8 bytes (4-byte offset + 4-byte
 * len) while the host ALSeqFile entry carries an 8-byte pointer — an in-place
 * patch would read its own overwritten output on LP64.
 */
void alSeqFileNewFrom(ALSeqFile *dst, const u8 *rawBE, u8 *base)
{
    uintptr_t base_addr = (uintptr_t)base;
    s16 seq_count;
    s32 i;

    if (dst == NULL || rawBE == NULL) {
        return;
    }

    seq_count = bank_ctl_s16(rawBE + 2);
    dst->revision = bank_ctl_s16(rawBE);
    dst->seqCount = seq_count;
    for (i = 0; i < seq_count; i++) {
        u32 offset = bank_ctl_u32(rawBE + 4 + (u32)i * 8);
        s32 len = bank_ctl_s32(rawBE + 4 + (u32)i * 8 + 4);

        dst->seqArray[i].offset = (u8 *)(base_addr + offset);
        dst->seqArray[i].len = len;
    }
}

/* Sequence count from a raw big-endian seqfile header, so the caller can size
 * the host ALSeqFile before parsing it. */
s16 alSeqFileCount(const u8 *rawBE)
{
    return bank_ctl_s16(rawBE + 2);
}

/* Superseded by alSeqFileNewFrom(); the stock in-place patcher is unusable on
 * LP64 for the reason above. Kept because the libaudio ABI declares it. */
void alSeqFileNew(ALSeqFile *file, u8 *base)
{
    (void)file;
    (void)base;
}

void alBnkfNew(ALBankFile *file, u8 *table)
{
    u8 *ctl = (u8 *)file;
    uintptr_t table_base = (uintptr_t)table;
    u32 ctl_size = gAudioBankCtlSize;
    s16 bank_count;
    u32 *bank_offsets;
    s32 i;

    if (file == NULL || !bank_ctl_has(ctl_size, 0, 4)) {
        return;
    }

    bank_count = bank_ctl_s16(ctl + 2);
    if (bank_count <= 0 ||
        !bank_ctl_has(ctl_size, 4, (u32)bank_count * 4)) {
        file->revision = bank_ctl_s16(ctl);
        file->bankCount = 0;
        return;
    }

    bank_offsets = calloc((u32)bank_count, sizeof(*bank_offsets));
    if (bank_offsets == NULL) {
        return;
    }

    for (i = 0; i < bank_count; i++) {
        bank_offsets[i] = bank_ctl_u32(ctl + 4 + (u32)i * 4);
    }

    file->revision = bank_ctl_s16(ctl);
    file->bankCount = bank_count;
    for (i = 0; i < bank_count; i++) {
        u32 bank_offset = bank_offsets[i];
        file->bankArray[i] = bank_offset != 0
            ? bank_make_bank(ctl, ctl_size, bank_offset, table_base)
            : NULL;
    }
    free(bank_offsets);
}
