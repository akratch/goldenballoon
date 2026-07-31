/*
 * Clean-room native audio engine: bank/sequence parsing, the filter chain, the
 * synthesis driver, and the compressed-sequence (CSP) player.
 *
 * This replaces the decompiled SGI libultra synthesiser that mdkr64 used to
 * compile out of game/libultra/src/audio/. It implements the same libaudio ABI
 * the game code calls, but with real host pointers and explicit big-endian
 * parsing rather than overlay-and-patch on the raw N64 image.
 *
 * Provenance: ported from the author's GoldenEye port mgb64
 * (src/platform/audio_compat.c) — shared first-party work. The DKR-specific
 * extensions live in the clearly marked block at the end of this file, and the
 * bank/seqfile layer is mdkr64's own arena-resident parser.
 */

#include <libaudio.h>
#include "synthInternals.h"
#include "seqp.h"
#include "audio_event_queue.h"
#include "audio_fx_transfer.h"

#include <limits.h>
#include <math.h>

#ifdef TARGET_N64
#ifndef SIZE_MAX
#define SIZE_MAX ((size_t)-1)
#endif

extern void *calloc(size_t count, size_t size);
extern void free(void *ptr);
extern void *memcpy(void *dest, const void *src, size_t count);
extern void *memset(void *dest, int value, size_t count);
#else
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#endif

ALGlobals *alGlobals = NULL;

/* ===================================================================
 * DKR extensions (early definitions)
 *
 * These three live here rather than in the DKR block at the end of the
 * file because the engine above uses them: alFxPull() reads alFXEnabled
 * and the two pan entry points call modify_panning().
 * =================================================================== */

/* Reverb master switch. game/src/audio.c sets this from MDKR_AUDIO_REVERB
 * via alFxReverbSet(); alFxPull() gates the wet stage on it. */
u8 alFXEnabled = TRUE;

/* "Audio Options" stereo mode. Global to the synth, not per voice.
 * Mirrors StereoPanMode in libultra/src/audio/synstartvoiceparam.h. */
static s16 s_stereoPanMode = 0; /* STEREO */

void set_stereo_pan_mode(s32 panMode)
{
    s_stereoPanMode = (s16)panMode;
}

s32 get_stereo_pan_mode(void)
{
    return s_stereoPanMode;
}

/*
 * Fold a 0..127 pan (64 = centre) into the selected stereo mode. Every pan
 * value reaching the mixer passes through here — both the note-on pan and
 * later CC10 updates — so the raw pan is never used directly.
 *   STEREO     full width, unchanged
 *   HEADPHONES half width: deviation from centre is halved (~32..95)
 *   MONO       collapsed to centre
 */
s32 modify_panning(s32 pan)
{
    switch (s_stereoPanMode) {
        case 0: /* STEREO */
            return pan;
        case 2: /* HEADPHONES */
            return ((pan - AL_PAN_CENTER) >> 1) + AL_PAN_CENTER;
        default: /* MONO */
            return AL_PAN_CENTER;
    }
}

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

static u32 bank_ctl_u32(const u8 *data)
{
    return ((u32)data[0] << 24) | ((u32)data[1] << 16) |
           ((u32)data[2] << 8) | (u32)data[3];
}

static s32 bank_ctl_s32(const u8 *data)
{
    return (s32)bank_ctl_u32(data);
}

static f32 audio_exp2f(f32 exponent)
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

/*
 * RAW16 samples are serialized big-endian signed PCM. The N64 RSP consumes
 * those bytes natively; the port's software mixer consumes host-native s16 and
 * therefore needs the dedicated conversion load. Keep this local alias so the
 * three RAW16 sites stay mechanically distinguishable from _decodeChunk's
 * ADPCM byte-stream load, which must remain unswapped.
 * (tests/check_raw16_audio.py asserts exactly that split.)
 */
#ifdef NATIVE_PORT
#define aLoadRaw16Buffer(pkt, source) aLoadBufferSwap16((pkt), (source))
#else
#define aLoadRaw16Buffer(pkt, source) aLoadBuffer((pkt), (source))
#endif

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

static u8 cseq_track_byte(ALCSeq *seq, u32 track)
{
    u8 value;

    if (seq->curBULen[track] != 0) {
        value = *seq->curBUPtr[track]++;
        seq->curBULen[track]--;
        return value;
    }

    value = *seq->curLoc[track]++;
    if (value == AL_CMIDI_BLOCK_CODE) {
        u8 next = *seq->curLoc[track]++;

        if (next != AL_CMIDI_BLOCK_CODE) {
            u8 backup_lo = *seq->curLoc[track]++;
            u8 length = *seq->curLoc[track]++;
            u32 backup = ((u32)next << 8) | backup_lo;

            seq->curBUPtr[track] = seq->curLoc[track] - (backup + 4);
            seq->curBULen[track] = length;
            value = *seq->curBUPtr[track]++;
            seq->curBULen[track]--;
        }
    }

    return value;
}

static u32 cseq_read_var_len(ALCSeq *seq, u32 track)
{
    u32 value = cseq_track_byte(seq, track);

    if (value & 0x80) {
        u32 next;

        value &= 0x7f;
        do {
            next = cseq_track_byte(seq, track);
            value = (value << 7) + (next & 0x7f);
        } while (next & 0x80);
    }

    return value;
}

static void cseq_track_event(ALCSeq *seq, u32 track, ALEvent *event)
{
    u8 status = cseq_track_byte(seq, track);

    if (status == AL_MIDI_Meta) {
        u8 type = cseq_track_byte(seq, track);

        if (type == AL_MIDI_META_TEMPO) {
            event->type = AL_TEMPO_EVT;
            event->msg.tempo.status = status;
            event->msg.tempo.type = type;
            event->msg.tempo.byte1 = cseq_track_byte(seq, track);
            event->msg.tempo.byte2 = cseq_track_byte(seq, track);
            event->msg.tempo.byte3 = cseq_track_byte(seq, track);
            seq->lastStatus[track] = 0;
        } else if (type == AL_MIDI_META_EOT) {
            u32 mask = 1u << track;

            seq->validTracks ^= mask;
            event->type =
                seq->validTracks != 0 ? AL_TRACK_END : AL_SEQ_END_EVT;
        } else if (type == AL_CMIDI_LOOPSTART_CODE) {
            (void)cseq_track_byte(seq, track);
            (void)cseq_track_byte(seq, track);
            seq->lastStatus[track] = 0;
            event->type = AL_CSP_LOOPSTART;
        } else if (type == AL_CMIDI_LOOPEND_CODE) {
            u8 *cursor = seq->curLoc[track];
            u8 loop_count = *cursor++;
            u8 current_count = *cursor;

            if (current_count == 0) {
                *cursor = loop_count;
                seq->curLoc[track] = cursor + 5;
            } else {
                u32 offset;

                if (current_count != 0xff) {
                    *cursor = current_count - 1;
                }
                cursor++;
                offset = ((u32)cursor[0] << 24) | ((u32)cursor[1] << 16) |
                         ((u32)cursor[2] << 8) | (u32)cursor[3];
                cursor += 4;
                seq->curLoc[track] = cursor - offset;
            }
            seq->lastStatus[track] = 0;
            event->type = AL_CSP_LOOPEND;
        } else {
            event->type = AL_TRACK_END;
        }
    } else {
        event->type = AL_SEQ_MIDI_EVT;
        if (status & 0x80) {
            event->msg.midi.status = status;
            event->msg.midi.byte1 = cseq_track_byte(seq, track);
            seq->lastStatus[track] = status;
        } else {
            event->msg.midi.status = seq->lastStatus[track];
            event->msg.midi.byte1 = status;
        }

        if ((event->msg.midi.status & 0xf0) != AL_MIDI_ProgramChange &&
            (event->msg.midi.status & 0xf0) != AL_MIDI_ChannelPressure) {
            event->msg.midi.byte2 = cseq_track_byte(seq, track);
            if ((event->msg.midi.status & 0xf0) == AL_MIDI_NoteOn) {
                event->msg.midi.duration = cseq_read_var_len(seq, track);
            }
        } else {
            event->msg.midi.byte2 = 0;
        }
    }

}

static int cseq_header_is_native(ALCMidiHdr *header)
{
    return header->division > 0 && header->division < 0x10000;
}

static void cseq_copy_to_marker(ALCSeqMarker *marker, const ALCSeq *seq)
{
    s32 i;

    marker->validTracks = seq->validTracks;
    marker->lastTicks = seq->lastTicks;
    marker->lastDeltaTicks = seq->lastDeltaTicks;

    for (i = 0; i < 16; i++) {
        marker->curLoc[i] = seq->curLoc[i];
        marker->curBUPtr[i] = seq->curBUPtr[i];
        marker->curBULen[i] = seq->curBULen[i];
        marker->lastStatus[i] = seq->lastStatus[i];
        marker->evtDeltaTicks[i] = seq->evtDeltaTicks[i];
    }
}

void alCSeqNew(ALCSeq *seq, u8 *ptr)
{
    ALCMidiHdr *header = (ALCMidiHdr *)ptr;
    int native_header;
    u32 division;
    s32 i;

    if (seq == NULL || ptr == NULL) {
        return;
    }

    native_header = cseq_header_is_native(header);
    seq->base = header;
    seq->validTracks = 0;
    seq->lastDeltaTicks = 0;
    seq->lastTicks = 0;
    seq->deltaFlag = 1;

    division = native_header ? header->division : bank_ctl_u32(ptr + 64);
    header->division = division;

    for (i = 0; i < 16; i++) {
        u32 offset = native_header ? header->trackOffset[i]
                                   : bank_ctl_u32(ptr + (u32)i * 4);

        header->trackOffset[i] = offset;
        seq->lastStatus[i] = 0;
        seq->curBUPtr[i] = NULL;
        seq->curBULen[i] = 0;
        if (offset != 0) {
            seq->validTracks |= 1u << i;
            seq->curLoc[i] = ptr + offset;
            seq->evtDeltaTicks[i] = cseq_read_var_len(seq, (u32)i);
        } else {
            seq->curLoc[i] = NULL;
            seq->evtDeltaTicks[i] = 0;
        }
    }

    seq->qnpt = division != 0 ? 1.0f / (f32)division : 0.0f;
}

void alCSeqNextEvent(ALCSeq *seq, ALEvent *event)
{
    u32 first_time = 0xffffffffu;
    u32 first_track = 0;
    u32 last_ticks;
    s32 i;

    if (seq == NULL || event == NULL) {
        return;
    }

    if (seq->validTracks == 0) {
        event->type = AL_SEQ_END_EVT;
        event->msg.end.ticks = 0;
        return;
    }

    last_ticks = seq->lastDeltaTicks;
    for (i = 0; i < 16; i++) {
        if ((seq->validTracks >> i) & 1u) {
            if (seq->deltaFlag) {
                seq->evtDeltaTicks[i] -= last_ticks;
            }
            if (seq->evtDeltaTicks[i] < first_time) {
                first_time = seq->evtDeltaTicks[i];
                first_track = (u32)i;
            }
        }
    }

    cseq_track_event(seq, first_track, event);

    event->msg.midi.ticks = first_time;
    seq->lastTicks += first_time;
    seq->lastDeltaTicks = first_time;
    if (event->type != AL_TRACK_END) {
        seq->evtDeltaTicks[first_track] +=
            cseq_read_var_len(seq, first_track);
    }
    seq->deltaFlag = 1;
}

char __alCSeqNextDelta(ALCSeq *seq, s32 *delta_ticks)
{
    u32 first_time = 0xffffffffu;
    u32 last_ticks;
    s32 i;

    if (seq == NULL || delta_ticks == NULL || seq->validTracks == 0) {
        return FALSE;
    }

    last_ticks = seq->lastDeltaTicks;
    for (i = 0; i < 16; i++) {
        if ((seq->validTracks >> i) & 1u) {
            if (seq->deltaFlag) {
                seq->evtDeltaTicks[i] -= last_ticks;
            }
            if (seq->evtDeltaTicks[i] < first_time) {
                first_time = seq->evtDeltaTicks[i];
            }
        }
    }

    seq->deltaFlag = 0;
    *delta_ticks = (s32)first_time;
    return TRUE;
}

f32 alCSeqTicksToSec(ALCSeq *seq, s32 ticks, u32 tempo)
{
    if (seq == NULL || seq->base == NULL || seq->base->division == 0) {
        return 0.0f;
    }

    return ((f32)ticks * (f32)tempo) /
           ((f32)seq->base->division * 1000000.0f);
}

u32 alCSeqSecToTicks(ALCSeq *seq, f32 sec, u32 tempo)
{
    if (seq == NULL || seq->base == NULL || tempo == 0) {
        return 0;
    }

    return (u32)(((sec * 1000000.0f) * (f32)seq->base->division) /
                 (f32)tempo);
}

s32 alCSeqGetTicks(ALCSeq *seq)
{
    return seq != NULL ? (s32)seq->lastTicks : 0;
}

void alCSeqNewMarker(ALCSeq *seq, ALCSeqMarker *marker, u32 ticks)
{
    ALCSeq temp_seq;
    ALEvent event;

    if (seq == NULL || marker == NULL || seq->base == NULL) {
        return;
    }

    alCSeqNew(&temp_seq, (u8 *)seq->base);
    do {
        cseq_copy_to_marker(marker, &temp_seq);
        alCSeqNextEvent(&temp_seq, &event);
        if (event.type == AL_SEQ_END_EVT) {
            break;
        }
    } while (temp_seq.lastTicks < ticks);
}

void alCSeqSetLoc(ALCSeq *seq, ALCSeqMarker *marker)
{
    s32 i;

    if (seq == NULL || marker == NULL) {
        return;
    }

    seq->validTracks = marker->validTracks;
    seq->lastTicks = marker->lastTicks;
    seq->lastDeltaTicks = marker->lastDeltaTicks;

    for (i = 0; i < 16; i++) {
        seq->curLoc[i] = marker->curLoc[i];
        seq->curBUPtr[i] = marker->curBUPtr[i];
        seq->curBULen[i] = marker->curBULen[i];
        seq->lastStatus[i] = marker->lastStatus[i];
        seq->evtDeltaTicks[i] = marker->evtDeltaTicks[i];
    }
}

void alCSeqGetLoc(ALCSeq *seq, ALCSeqMarker *marker)
{
    if (seq == NULL || marker == NULL) {
        return;
    }

    cseq_copy_to_marker(marker, seq);
}

#define NATIVE_MIDI_HEADER_MAGIC 0x4d546864u
#define NATIVE_MIDI_TRACK_MAGIC 0x4d54726bu

static u8 seq_read_u8(ALSeq *seq)
{
    return *seq->curPtr++;
}

static u16 seq_read_u16(ALSeq *seq)
{
    u16 value = (u16)seq_read_u8(seq) << 8;

    value |= seq_read_u8(seq);
    return value;
}

static u32 seq_read_u32(ALSeq *seq)
{
    u32 value = (u32)seq_read_u8(seq) << 24;

    value |= (u32)seq_read_u8(seq) << 16;
    value |= (u32)seq_read_u8(seq) << 8;
    value |= seq_read_u8(seq);
    return value;
}

static s32 seq_read_var_len(ALSeq *seq)
{
    s32 value = seq_read_u8(seq);

    if (value & 0x80) {
        s32 next;

        value &= 0x7f;
        do {
            next = seq_read_u8(seq);
            value = (value << 7) + (next & 0x7f);
        } while (next & 0x80);
    }

    return value;
}

static void seq_skip_bytes(ALSeq *seq, s32 byte_count)
{
    if (byte_count <= 0) {
        return;
    }

    seq->curPtr += byte_count;
}

void alSeqNew(ALSeq *seq, u8 *ptr, s32 len)
{
    u16 division;

    if (seq == NULL || ptr == NULL) {
        return;
    }

    seq->base = ptr;
    seq->len = len;
    seq->lastStatus = 0;
    seq->lastTicks = 0;
    seq->curPtr = ptr;
    seq->trackStart = ptr;
    seq->division = 0;
    seq->qnpt = 0.0f;

    if (len < 22 || seq_read_u32(seq) != NATIVE_MIDI_HEADER_MAGIC) {
        return;
    }

    (void)seq_read_u32(seq);
    if (seq_read_u16(seq) != 0 || seq_read_u16(seq) != 1) {
        return;
    }

    division = seq_read_u16(seq);
    if (division & 0x8000) {
        return;
    }

    seq->division = (s16)division;
    seq->qnpt = division != 0 ? 1.0f / (f32)division : 0.0f;

    if (seq_read_u32(seq) != NATIVE_MIDI_TRACK_MAGIC) {
        return;
    }

    (void)seq_read_u32(seq);
    seq->trackStart = seq->curPtr;
}

void alSeqNextEvent(ALSeq *seq, ALEvent *event)
{
    u8 status;
    s32 delta_ticks;

    if (seq == NULL || event == NULL) {
        return;
    }

    if (seq->curPtr >= seq->base + seq->len) {
        event->type = AL_SEQ_END_EVT;
        event->msg.end.ticks = 0;
        return;
    }

    delta_ticks = seq_read_var_len(seq);
    seq->lastTicks += delta_ticks;
    status = seq_read_u8(seq);

    if (status == 0xf0 || status == 0xf7) {
        s32 payload_len = seq_read_var_len(seq);

        seq_skip_bytes(seq, payload_len);
        alSeqNextEvent(seq, event);
        return;
    }

    if (status == AL_MIDI_Meta) {
        u8 type = seq_read_u8(seq);

        if (type == AL_MIDI_META_TEMPO) {
            event->type = AL_TEMPO_EVT;
            event->msg.tempo.ticks = delta_ticks;
            event->msg.tempo.status = status;
            event->msg.tempo.type = type;
            event->msg.tempo.len = seq_read_u8(seq);
            event->msg.tempo.byte1 = seq_read_u8(seq);
            event->msg.tempo.byte2 = seq_read_u8(seq);
            event->msg.tempo.byte3 = seq_read_u8(seq);
        } else if (type == AL_MIDI_META_EOT) {
            event->type = AL_SEQ_END_EVT;
            event->msg.end.ticks = delta_ticks;
            event->msg.end.status = status;
            event->msg.end.type = type;
            event->msg.end.len = seq_read_u8(seq);
        } else {
            s32 payload_len = seq_read_var_len(seq);

            seq_skip_bytes(seq, payload_len);
            alSeqNextEvent(seq, event);
            return;
        }

        seq->lastStatus = 0;
        return;
    }

    event->type = AL_SEQ_MIDI_EVT;
    event->msg.midi.ticks = delta_ticks;
    if (status & 0x80) {
        event->msg.midi.status = status;
        event->msg.midi.byte1 = seq_read_u8(seq);
        seq->lastStatus = status;
    } else {
        event->msg.midi.status = seq->lastStatus;
        event->msg.midi.byte1 = status;
    }

    if ((event->msg.midi.status & 0xf0) != AL_MIDI_ProgramChange &&
        (event->msg.midi.status & 0xf0) != AL_MIDI_ChannelPressure) {
        event->msg.midi.byte2 = seq_read_u8(seq);
    } else {
        event->msg.midi.byte2 = 0;
    }
}

char __alSeqNextDelta(ALSeq *seq, s32 *delta_ticks)
{
    u8 *saved;

    if (seq == NULL || delta_ticks == NULL || seq->curPtr == NULL ||
        seq->base == NULL || seq->curPtr >= seq->base + seq->len) {
        return FALSE;
    }

    saved = seq->curPtr;
    *delta_ticks = seq_read_var_len(seq);
    seq->curPtr = saved;
    return TRUE;
}

f32 alSeqTicksToSec(ALSeq *seq, s32 ticks, u32 tempo)
{
    if (seq == NULL || seq->division == 0) {
        return 0.0f;
    }

    return ((f32)ticks * (f32)tempo) /
           ((f32)seq->division * 1000000.0f);
}

u32 alSeqSecToTicks(ALSeq *seq, f32 sec, u32 tempo)
{
    if (seq == NULL || tempo == 0) {
        return 0;
    }

    return (u32)(((sec * 1000000.0f) * (f32)seq->division) /
                 (f32)tempo);
}

void alSeqNewMarker(ALSeq *seq, ALSeqMarker *marker, u32 ticks)
{
    ALEvent event;
    u8 *saved_ptr;
    u8 *last_ptr;
    s32 saved_ticks;
    s32 last_ticks;
    s16 saved_status;
    s16 last_status;

    if (seq == NULL || marker == NULL) {
        return;
    }

    if (ticks == 0) {
        marker->curPtr = seq->trackStart;
        marker->lastStatus = 0;
        marker->lastTicks = 0;
        marker->curTicks = 0;
        return;
    }

    saved_ptr = seq->curPtr;
    saved_status = seq->lastStatus;
    saved_ticks = seq->lastTicks;

    seq->curPtr = seq->trackStart;
    seq->lastStatus = 0;
    seq->lastTicks = 0;

    do {
        last_ptr = seq->curPtr;
        last_status = seq->lastStatus;
        last_ticks = seq->lastTicks;

        alSeqNextEvent(seq, &event);
        if (event.type == AL_SEQ_END_EVT) {
            last_ptr = seq->curPtr;
            last_status = seq->lastStatus;
            last_ticks = seq->lastTicks;
            break;
        }
    } while ((u32)seq->lastTicks < ticks);

    marker->curPtr = last_ptr;
    marker->lastStatus = last_status;
    marker->lastTicks = last_ticks;
    marker->curTicks = seq->lastTicks;

    seq->curPtr = saved_ptr;
    seq->lastStatus = saved_status;
    seq->lastTicks = saved_ticks;
}

s32 alSeqGetTicks(ALSeq *seq)
{
    return seq != NULL ? seq->lastTicks : 0;
}

void alSeqSetLoc(ALSeq *seq, ALSeqMarker *marker)
{
    if (seq == NULL || marker == NULL) {
        return;
    }

    seq->curPtr = marker->curPtr;
    seq->lastStatus = marker->lastStatus;
    seq->lastTicks = marker->lastTicks;
}

void alSeqGetLoc(ALSeq *seq, ALSeqMarker *marker)
{
    if (seq == NULL || marker == NULL) {
        return;
    }

    marker->curPtr = seq->curPtr;
    marker->lastStatus = seq->lastStatus;
    marker->lastTicks = seq->lastTicks;
}

static void enqueue_voice_update(ALVoice *voice, ALParam *update)
{
    ALFilter *filter;

    if (voice == NULL || voice->pvoice == NULL || update == NULL) {
        if (update != NULL) {
            __freeParam(update);
        }
        return;
    }

    filter = voice->pvoice->channelKnob;
    if (filter == NULL || filter->setParam == NULL) {
        __freeParam(update);
        return;
    }

    filter->setParam(filter, AL_FILTER_ADD_UPDATE, update);
}

static void enqueue_filter_update(ALFilter *filter, ALParam *update)
{
    if (filter == NULL || filter->setParam == NULL || update == NULL) {
        if (update != NULL) {
            __freeParam(update);
        }
        return;
    }

    filter->setParam(filter, AL_FILTER_ADD_UPDATE, update);
}

static ALParam *alloc_voice_update(ALSynth *synth, ALVoice *voice, s16 type)
{
    ALParam *update;

    if (synth == NULL || voice == NULL || voice->pvoice == NULL) {
        return NULL;
    }

    update = __allocParam();
    if (update == NULL) {
        return NULL;
    }

    update->next = NULL;
    update->delta = synth->paramSamples + voice->pvoice->offset;
    update->type = type;
    return update;
}

static PVoice *take_first_voice(ALLink *from, ALLink *to)
{
    ALLink *node;

    if (from == NULL || to == NULL || from->next == NULL) {
        return NULL;
    }

    node = from->next;
    alUnlink(node);
    alLink(node, to);
    return (PVoice *)node;
}

static PVoice *find_stealable_voice(ALSynth *synth, s16 priority)
{
    ALLink *node;
    PVoice *candidate = NULL;

    if (synth == NULL) {
        return NULL;
    }

    for (node = synth->pAllocList.next; node != NULL; node = node->next) {
        PVoice *physical = (PVoice *)node;

        if (physical->vvoice != NULL &&
            physical->vvoice->priority <= priority &&
            physical->offset == 0) {
            candidate = physical;
            priority = physical->vvoice->priority;
        }
    }

    return candidate;
}

static s32 native_time_to_samples_no_round(ALSynth *synth, s32 micros)
{
    f32 samples;

    if (synth == NULL || synth->outputRate <= 0) {
        return 0;
    }

    samples = ((f32)micros * (f32)synth->outputRate) / 1000000.0f + 0.5f;
    return (s32)samples;
}

static s32 native_next_sample_time(ALSynth *synth, ALPlayer **client)
{
    ALPlayer *current;
    ALMicroTime best_delta = 0x7fffffff;

    if (client != NULL) {
        *client = NULL;
    }

    if (synth == NULL || synth->head == NULL || client == NULL) {
        return 0x7fffffff;
    }

    for (current = synth->head; current != NULL; current = current->next) {
        ALMicroTime delta = current->samplesLeft - synth->curSamples;

        if (delta < best_delta) {
            best_delta = delta;
            *client = current;
        }
    }

    return *client != NULL ? (*client)->samplesLeft : 0x7fffffff;
}

#define NATIVE_FX_FILTER_SCALE 16384
#define NATIVE_FX_RATE_RANGE 2.0f
#define NATIVE_FX_CENTS_DENOM 173123.404906676f
#define NATIVE_EQPOWER_LENGTH 128
#define NATIVE_HALF_PI 1.57079632679489661923

static s32 s_native_fx_bypass_params[2] = {
    0,
    AL_FX_BUFFER_SIZE,
};
static s16 s_native_eqpower[NATIVE_EQPOWER_LENGTH];
static int s_native_eqpower_ready = 0;

static s32 *native_fx_params_for_config(ALSynConfig *config)
{
    /* DKR's ALSynConfig carries one ALFxId PER AUX BUS (fxType[2]), where
     * mgb64's engine had a single scalar. Reading it as a scalar silently
     * compares an array address against the enum, always misses AL_FX_CUSTOM,
     * and falls through to the bypass params — which allocates the reverb but
     * gives it no delay line, so the wet path is inaudible and reverb-on and
     * reverb-off captures come out identical. Index bus 0 explicitly; that is
     * the bus every CSP voice is parented to. */
    if (config != NULL && config->fxType[0] == AL_FX_CUSTOM &&
        config->params != NULL) {
        return config->params;
    }

    return s_native_fx_bypass_params;
}

void init_lpfilter(ALLowPass *low_pass)
{
    s32 i;
    s16 fc;
    double fcoef;
    double ffc;

    if (low_pass == NULL) {
        return;
    }

    fc = (s16)(((s32)low_pass->fc * NATIVE_FX_FILTER_SCALE) >> 15);
    low_pass->fgain = (s16)(NATIVE_FX_FILTER_SCALE - fc);
    low_pass->first = 1;

    for (i = 0; i < 16; i++) {
        low_pass->fcvec.fccoef[i] = 0;
    }

    low_pass->fcvec.fccoef[8] = fc;
    ffc = (double)fc / (double)NATIVE_FX_FILTER_SCALE;
    fcoef = ffc;
    for (i = 9; i < 16; i++) {
        fcoef *= ffc;
        low_pass->fcvec.fccoef[i] =
            (s16)(fcoef * (double)NATIVE_FX_FILTER_SCALE);
    }
}

static void native_eqpower_init(void)
{
    s32 i;

    if (s_native_eqpower_ready) {
        return;
    }

    for (i = 0; i < NATIVE_EQPOWER_LENGTH; i++) {
        double t = (NATIVE_HALF_PI * (double)i) /
                   (double)(NATIVE_EQPOWER_LENGTH - 1);
        double scaled = cos(t) * 32767.0;
        long value = (long)(scaled + 0.5);

        if (value < 0) {
            value = 0;
        } else if (value > 32767) {
            value = 32767;
        }
        s_native_eqpower[i] = (s16)value;
    }

    s_native_eqpower_ready = 1;
}

static s16 native_eqpower_at(s32 idx)
{
    native_eqpower_init();

    if (idx < 0) {
        idx = 0;
    } else if (idx >= NATIVE_EQPOWER_LENGTH) {
        idx = NATIVE_EQPOWER_LENGTH - 1;
    }

    return s_native_eqpower[idx];
}

static s16 native_env_rate(f32 volume, f32 target, s32 count, u16 *low)
{
    f32 step;
    s16 high;

    if (low == NULL) {
        return 0;
    }

    if (count == 0) {
        if (target >= volume) {
            *low = 0xffff;
            return 0x7fff;
        }

        *low = 0;
        return (s16)-0x8000;
    }

    step = ((target - volume) / (f32)count) * 8.0f;
    if (step < 0.0f) {
        step -= 1.0f;
    }

    high = (s16)(s32)step;
    *low = (u16)(s32)(65535.0f * (step - (f32)high));
    return (s16)(s32)step;
}

static s16 native_env_target_left(ALEnvMixer *envmixer)
{
    return (s16)(((s32)envmixer->volume *
                  native_eqpower_at(envmixer->pan)) >>
                 15);
}

static s16 native_env_target_right(ALEnvMixer *envmixer)
{
    return (s16)(((s32)envmixer->volume *
                  native_eqpower_at((NATIVE_EQPOWER_LENGTH - 1) -
                                    envmixer->pan)) >>
                 15);
}

static s16 native_env_advance_volume(s16 current, s16 rate_high,
                                     u16 rate_low, s32 delta)
{
    f32 rate = (((f32)((s32)rate_high * 65536) + (f32)rate_low) /
                65536.0f);
    return (s16)((f32)current + (rate * (f32)delta * 0.125f));
}

static void native_env_update_current_targets(ALEnvMixer *envmixer)
{
    envmixer->ltgt = native_env_target_left(envmixer);
    envmixer->rtgt = native_env_target_right(envmixer);
    envmixer->delta = envmixer->segEnd;
    envmixer->cvolL = envmixer->ltgt;
    envmixer->cvolR = envmixer->rtgt;
}

static Acmd *native_envmixer_pull_subframe(ALEnvMixer *envmixer, s16 *input,
                                           s16 *output, s32 out_count,
                                           s32 sample_offset, Acmd *cmd)
{
    ALFilter *source;
    Acmd *ptr = cmd;

    if (envmixer == NULL || input == NULL || output == NULL ||
        out_count <= 0) {
        return ptr;
    }

    source = envmixer->filter.source;
    if (source == NULL || source->handler == NULL) {
        return ptr;
    }

    ptr = source->handler(source, input, out_count, sample_offset, cmd);

    aSetBuffer(ptr++, A_MAIN, *input, AL_MAIN_L_OUT + *output,
               out_count << 1);
    aSetBuffer(ptr++, A_AUX, AL_MAIN_R_OUT + *output,
               AL_AUX_L_OUT + *output, AL_AUX_R_OUT + *output);

    if (envmixer->first) {
        envmixer->first = 0;
        envmixer->ltgt = native_env_target_left(envmixer);
        envmixer->lratm = native_env_rate(envmixer->cvolL, envmixer->ltgt,
                                          envmixer->segEnd,
                                          &envmixer->lratl);
        envmixer->rtgt = native_env_target_right(envmixer);
        envmixer->rratm = native_env_rate(envmixer->cvolR, envmixer->rtgt,
                                          envmixer->segEnd,
                                          &envmixer->rratl);

        aSetVolume(ptr++, A_LEFT | A_VOL, envmixer->cvolL, 0, 0);
        aSetVolume(ptr++, A_RIGHT | A_VOL, envmixer->cvolR, 0, 0);
        aSetVolume(ptr++, A_LEFT | A_RATE, envmixer->ltgt,
                   envmixer->lratm, envmixer->lratl);
        aSetVolume(ptr++, A_RIGHT | A_RATE, envmixer->rtgt,
                   envmixer->rratm, envmixer->rratl);
        aSetVolume(ptr++, A_AUX, envmixer->dryamt, 0, envmixer->wetamt);
        aEnvMixer(ptr++, A_INIT | A_AUX, osVirtualToPhysical(envmixer->state));
    } else {
        aEnvMixer(ptr++, A_CONTINUE | A_AUX,
                  osVirtualToPhysical(envmixer->state));
    }

    *input += (s16)(out_count << 1);
    return ptr;
}

Acmd *alEnvmixerPull(void *filter, s16 *outp, s32 out_count,
                     s32 sample_offset, Acmd *cmd)
{
    ALEnvMixer *envmixer = (ALEnvMixer *)filter;
    Acmd *ptr = cmd;
    s16 input = AL_RESAMPLER_OUT;
    s16 local_out = 0;
    s32 current_offset = sample_offset;

    if (envmixer == NULL) {
        return ptr;
    }

    while (envmixer->ctrlList != NULL) {
        ALParam *update;
        s32 last_offset = current_offset;
        s32 samples;

        current_offset = envmixer->ctrlList->delta;
        samples = current_offset - last_offset;
        if (samples > out_count) {
            break;
        }
        if (samples < 0) {
            samples = 0;
        }
        if (samples > AL_MAX_RSP_SAMPLES) {
            samples = AL_MAX_RSP_SAMPLES;
        }

        switch (envmixer->ctrlList->type) {
        case AL_FILTER_START_VOICE_ALT:
        {
            ALStartParamAlt *param = (ALStartParamAlt *)envmixer->ctrlList;
            s32 volume;

            if (param->unity) {
                envmixer->filter.setParam(&envmixer->filter,
                                           AL_FILTER_SET_UNITY_PITCH, 0);
            }
            envmixer->filter.setParam(&envmixer->filter,
                                       AL_FILTER_SET_WAVETABLE, param->wave);
            envmixer->filter.setParam(&envmixer->filter, AL_FILTER_START, 0);

            envmixer->first = 1;
            envmixer->delta = 0;
            envmixer->segEnd = param->samples;
            /*
             * Volume is applied LINEARLY. mgb64 squares it here
             * ((v*v)>>15, a perceptual-loudness curve); DKR's synthesiser
             * does not, and squaring costs ~4 dB overall and attenuates
             * quiet voices quadratically, which collapses the RAW16 bass
             * against the rest of the mix. Behaviour cross-checked against
             * the deleted game/libultra/src/audio/env.c, whose "map volume
             * non-linearly" step is the vestigial no-op (fVol+fVol)/2.
             */
            volume = (s32)param->volume;
            envmixer->volume = (s16)volume;
            envmixer->pan = param->pan;
            envmixer->dryamt = native_eqpower_at(param->fxMix);
            envmixer->wetamt =
                native_eqpower_at((NATIVE_EQPOWER_LENGTH - 1) - param->fxMix);

            if (param->samples != 0) {
                envmixer->cvolL = 1;
                envmixer->cvolR = 1;
            } else {
                envmixer->cvolL = native_env_target_left(envmixer);
                envmixer->cvolR = native_env_target_right(envmixer);
            }

            if (envmixer->filter.source != NULL &&
                envmixer->filter.source->setParam != NULL) {
                envmixer->filter.source->setParam(
                    envmixer->filter.source, AL_FILTER_SET_PITCH,
                    alParamFromF32Bits(param->pitch));
            }
            break;
        }

        case AL_FILTER_SET_FXAMT:
        case AL_FILTER_SET_PAN:
        case AL_FILTER_SET_VOLUME:
            ptr = native_envmixer_pull_subframe(envmixer, &input, &local_out,
                                                samples, sample_offset, ptr);
            envmixer->delta += samples;
            if (envmixer->delta >= envmixer->segEnd) {
                native_env_update_current_targets(envmixer);
            } else {
                envmixer->cvolL = native_env_advance_volume(
                    envmixer->cvolL, envmixer->lratm, envmixer->lratl,
                    envmixer->delta);
                envmixer->cvolR = native_env_advance_volume(
                    envmixer->cvolR, envmixer->rratm, envmixer->rratl,
                    envmixer->delta);
            }

            if (envmixer->cvolL == 0) {
                envmixer->cvolL = 1;
            }
            if (envmixer->cvolR == 0) {
                envmixer->cvolR = 1;
            }

            if (envmixer->ctrlList->type == AL_FILTER_SET_PAN) {
                envmixer->pan = (s16)envmixer->ctrlList->data.i;
            } else if (envmixer->ctrlList->type == AL_FILTER_SET_VOLUME) {
                s32 volume = envmixer->ctrlList->data.i;
                envmixer->delta = 0;
                /* Linear, as above (env.c cross-check). */
                envmixer->volume = (s16)volume;
                envmixer->segEnd = envmixer->ctrlList->moredata.i;
            } else {
                s32 fx_mix = envmixer->ctrlList->data.i;
                envmixer->dryamt = native_eqpower_at(fx_mix);
                envmixer->wetamt =
                    native_eqpower_at((NATIVE_EQPOWER_LENGTH - 1) - fx_mix);
            }
            envmixer->first = 1;
            break;

        case AL_FILTER_START_VOICE:
        {
            ALStartParam *param = (ALStartParam *)envmixer->ctrlList;

            if (param->unity) {
                envmixer->filter.setParam(&envmixer->filter,
                                           AL_FILTER_SET_UNITY_PITCH, 0);
            }
            envmixer->filter.setParam(&envmixer->filter,
                                       AL_FILTER_SET_WAVETABLE, param->wave);
            envmixer->filter.setParam(&envmixer->filter, AL_FILTER_START, 0);
            break;
        }

        case AL_FILTER_STOP_VOICE:
            ptr = native_envmixer_pull_subframe(envmixer, &input, &local_out,
                                                samples, sample_offset, ptr);
            if (envmixer->filter.setParam != NULL) {
                envmixer->filter.setParam(&envmixer->filter, AL_FILTER_RESET,
                                           0);
            }
            break;

        case AL_FILTER_FREE_VOICE:
        {
            ALFreeParam *param = (ALFreeParam *)envmixer->ctrlList;
            if (param->pvoice != NULL) {
                param->pvoice->offset = 0;
                if (alGlobals != NULL) {
                    _freePVoice(&alGlobals->drvr, (PVoice *)param->pvoice);
                }
            }
            break;
        }

        default:
            ptr = native_envmixer_pull_subframe(envmixer, &input, &local_out,
                                                samples, sample_offset, ptr);
            envmixer->delta += samples;
            if (envmixer->filter.source != NULL &&
                envmixer->filter.source->setParam != NULL) {
                envmixer->filter.source->setParam(
                    envmixer->filter.source, envmixer->ctrlList->type,
                    alParamFromS32(envmixer->ctrlList->data.i));
            }
            break;
        }

        local_out += (s16)(samples << 1);
        out_count -= samples;

        update = envmixer->ctrlList;
        envmixer->ctrlList = envmixer->ctrlList->next;
        if (envmixer->ctrlList == NULL) {
            envmixer->ctrlTail = NULL;
        }
        __freeParam(update);
    }

    if (envmixer->motion == AL_PLAYING) {
        ptr = native_envmixer_pull_subframe(envmixer, &input, &local_out,
                                            out_count, sample_offset, ptr);
        envmixer->delta += out_count;
    }

    if (envmixer->delta > envmixer->segEnd) {
        envmixer->delta = envmixer->segEnd;
    }

    (void)outp;
    return ptr;
}

s32 alEnvmixerParam(void *filter, s32 param_id, void *param)
{
    ALEnvMixer *envmixer = (ALEnvMixer *)filter;
    ALFilter *base = (ALFilter *)filter;

    if (envmixer == NULL || base == NULL) {
        return 0;
    }

    switch (param_id) {
    case AL_FILTER_ADD_UPDATE:
    {
        ALParam *update = (ALParam *)param;
        if (update == NULL) {
            break;
        }
        update->next = NULL;
        if (envmixer->ctrlTail != NULL) {
            envmixer->ctrlTail->next = update;
        } else {
            envmixer->ctrlList = update;
        }
        envmixer->ctrlTail = update;
        break;
    }

    case AL_FILTER_RESET:
        envmixer->first = 1;
        envmixer->motion = AL_STOPPED;
        envmixer->volume = 1;
        if (base->source != NULL && base->source->setParam != NULL) {
            base->source->setParam(base->source, AL_FILTER_RESET, param);
        }
        break;

    case AL_FILTER_START:
        envmixer->motion = AL_PLAYING;
        if (base->source != NULL && base->source->setParam != NULL) {
            base->source->setParam(base->source, AL_FILTER_START, param);
        }
        break;

    case AL_FILTER_SET_SOURCE:
        base->source = (ALFilter *)param;
        break;

    default:
        if (base->source != NULL && base->source->setParam != NULL) {
            base->source->setParam(base->source, param_id, param);
        }
        break;
    }

    return 0;
}

static void native_fx_note_guard(
    const char *operation,
    const ALFx *fx,
    const s16 *current,
    s32 count)
{
#ifdef NATIVE_PORT
    static s32 warned;
#endif

    portAudioFxRecordGuardTrip();
#ifdef NATIVE_PORT
    if (!warned) {
        warned = 1;
        fprintf(stderr,
                "[AUDIO] ALFx delay-line transfer rejected/clamped: "
                "op=%s base=%p current=%p length=%u count=%d\n",
                operation, fx != NULL ? (void *)fx->base : NULL,
                (const void *)current,
                fx != NULL ? (unsigned int)fx->length : 0,
                (int)count);
        fflush(stderr);
    }
#else
    (void)operation;
    (void)fx;
    (void)current;
    (void)count;
#endif
}

static s32 native_fx_pointer_offset(
    const ALFx *fx,
    const s16 *current,
    u32 *offset_out)
{
    uintptr_t base;
    uintptr_t address;
    u64 logical_bytes;
    u64 byte_offset;

    if (fx == NULL || fx->base == NULL || fx->length == 0 ||
        current == NULL || offset_out == NULL) {
        return FALSE;
    }

    base = (uintptr_t)fx->base;
    address = (uintptr_t)current;
    logical_bytes = (u64)fx->length * sizeof(*fx->base);

    if (address >= base) {
        byte_offset = (u64)(address - base);
    } else {
        u64 bytes_before = (u64)(base - address);

        if (bytes_before > logical_bytes) {
            return FALSE;
        }
        byte_offset = logical_bytes - bytes_before;
    }

    if (byte_offset > logical_bytes ||
        (byte_offset % sizeof(*fx->base)) != 0) {
        return FALSE;
    }

    *offset_out = (u32)(byte_offset / sizeof(*fx->base));
    return TRUE;
}

static s16 *native_fx_ring_relative(ALFx *fx, s64 relative_samples)
{
    u32 input_offset;
    s64 target;
    s64 length;

    if (!native_fx_pointer_offset(fx, fx != NULL ? fx->input : NULL,
                                  &input_offset)) {
        return NULL;
    }

    length = (s64)fx->length;
    relative_samples %= length;
    target = (s64)input_offset + relative_samples;
    target %= length;
    if (target < 0) {
        target += length;
    }

    return &fx->base[target];
}

static s32 native_fx_plan_transfer(
    ALFx *fx,
    s16 *current,
    s32 count,
    const char *operation,
    struct PortAudioFxTransferPlan *plan)
{
    u32 current_offset;
    s32 ok;

    if (fx == NULL || fx->base == NULL || fx->length == 0 || count <= 0 ||
        !native_fx_pointer_offset(fx, current, &current_offset)) {
        if (count > 0) {
            native_fx_note_guard(operation, fx, current, count);
        }
        return FALSE;
    }

    ok = portAudioFxPlanTransfer(
        fx->length, fx->length + PORT_AUDIO_FX_TAIL_SLACK_SAMPLES,
        current_offset, (u32)count, plan);
    if (!ok || plan->clamped || plan->planned_samples != (u32)count) {
        native_fx_note_guard(operation, fx, current, count);
        return FALSE;
    }
    return TRUE;
}

static Acmd *native_fx_load_buffer(ALFx *fx, s16 *current, s32 buffer,
                                   s32 count, Acmd *cmd)
{
    Acmd *ptr = cmd;
    struct PortAudioFxTransferPlan plan;

    if (!native_fx_plan_transfer(fx, current, count, "load", &plan)) {
        return ptr;
    }

    if (plan.first_samples != 0) {
        aSetBuffer(ptr++, 0, buffer, 0, plan.first_samples << 1);
        aLoadBuffer(
            ptr++, osVirtualToPhysical(&fx->base[plan.first_offset]));
    }
    if (plan.second_samples != 0) {
        aSetBuffer(ptr++, 0, buffer + (plan.first_samples << 1), 0,
                   plan.second_samples << 1);
        aLoadBuffer(
            ptr++, osVirtualToPhysical(&fx->base[plan.second_offset]));
    }

    aSetBuffer(ptr++, 0, 0, 0, plan.planned_samples << 1);
    return ptr;
}

static Acmd *native_fx_save_buffer(ALFx *fx, s16 *current, s32 buffer,
                                   s32 count, Acmd *cmd)
{
    Acmd *ptr = cmd;
    struct PortAudioFxTransferPlan plan;

    if (!native_fx_plan_transfer(fx, current, count, "save", &plan)) {
        return ptr;
    }

    if (plan.first_samples != 0) {
        aSetBuffer(ptr++, 0, 0, buffer, plan.first_samples << 1);
        aSaveBuffer(
            ptr++, osVirtualToPhysical(&fx->base[plan.first_offset]));
    }
    if (plan.second_samples != 0) {
        aSetBuffer(ptr++, 0, 0, buffer + (plan.first_samples << 1),
                   plan.second_samples << 1);
        aSaveBuffer(
            ptr++, osVirtualToPhysical(&fx->base[plan.second_offset]));
        aSetBuffer(ptr++, 0, 0, 0, plan.planned_samples << 1);
    }

    return ptr;
}

static f32 native_fx_modulate(ALDelay *delay, s32 count)
{
    f32 value;

    delay->rsval += delay->rsinc * (f32)count;
    if (delay->rsval > NATIVE_FX_RATE_RANGE) {
        delay->rsval -= NATIVE_FX_RATE_RANGE * 2.0f;
    }

    value = delay->rsval;
    if (value < 0.0f) {
        value = -value;
    }
    value -= NATIVE_FX_RATE_RANGE * 0.5f;
    return delay->rsgain * value;
}

static Acmd *native_fx_load_output_buffer(ALFx *fx, ALDelay *delay,
                                          s32 buffer, s32 in_count,
                                          Acmd *cmd)
{
    Acmd *ptr = cmd;
    s16 *out_ptr;

    if (delay == NULL || in_count <= 0) {
        return ptr;
    }

    if (delay->rs != NULL) {
        s32 length = (s32)delay->output - (s32)delay->input;
        f32 delta = native_fx_modulate(delay, in_count);
        f32 fratio;
        f32 needed;
        s32 count;
        s32 ratio;
        s32 ram_align;
        s32 resample_buffer = AL_TEMP_2;

        if (length != 0) {
            delta /= (f32)length;
        } else {
            delta = 0.0f;
        }
        delta = (f32)(s32)(delta * (f32)UNITY_PITCH);
        delta /= (f32)UNITY_PITCH;
        fratio = 1.0f - delta;

        needed = delay->rs->delta + (fratio * (f32)in_count);
        count = (s32)needed;
        delay->rs->delta = needed - (f32)count;

        out_ptr = native_fx_ring_relative(
            fx, -(s64)((s32)delay->output - delay->rsdelta));
        if (out_ptr == NULL) {
            native_fx_note_guard("chorus-tap", fx, out_ptr, count);
            return ptr;
        }
        ram_align = (s32)(((uintptr_t)out_ptr & 0x7) >> 1);
        out_ptr = native_fx_ring_relative(
            fx,
            -(s64)((s32)delay->output - delay->rsdelta) -
                (s64)ram_align);
        ptr = native_fx_load_buffer(
            fx, out_ptr, resample_buffer, count + ram_align, ptr);

        ratio = (s32)(fratio * (f32)UNITY_PITCH);
        aSetBuffer(ptr++, 0, resample_buffer + (ram_align << 1), buffer,
                   in_count << 1);
        aResample(ptr++, delay->rs->first, ratio,
                  osVirtualToPhysical(delay->rs->state));
        delay->rs->first = 0;
        delay->rsdelta += count - in_count;
    } else {
        out_ptr = native_fx_ring_relative(
            fx, -(s64)(s32)delay->output);
        ptr = native_fx_load_buffer(fx, out_ptr, buffer, in_count, ptr);
    }

    return ptr;
}

static Acmd *native_fx_filter_buffer(ALLowPass *low_pass, s32 buffer,
                                     s32 count, Acmd *cmd)
{
    Acmd *ptr = cmd;

    if (low_pass == NULL || count <= 0) {
        return ptr;
    }

    aSetBuffer(ptr++, 0, buffer, buffer, count << 1);
    aLoadADPCM(ptr++, 32, osVirtualToPhysical(low_pass->fcvec.fccoef));
    aPoleFilter(ptr++, low_pass->first, low_pass->fgain,
                osVirtualToPhysical(low_pass->fstate));
    low_pass->first = 0;
    return ptr;
}

Acmd *alFxPull(void *filter, s16 *outp, s32 out_count, s32 sample_offset,
               Acmd *cmd)
{
    ALFx *fx = (ALFx *)filter;
    ALFilter *source;
    Acmd *ptr = cmd;
    s16 input = AL_AUX_L_OUT;
    s16 output = AL_AUX_R_OUT;
    s16 buffer_a = AL_TEMP_0;
    s16 buffer_b = AL_TEMP_1;
    uintptr_t previous_output = 0;
    s32 previous_output_valid = FALSE;
    s32 i;

    if (fx == NULL || out_count <= 0) {
        return ptr;
    }

    source = fx->filter.source;
    if (source == NULL || source->handler == NULL) {
        return ptr;
    }

    ptr = source->handler(source, outp, out_count, sample_offset, cmd);

    /*
     * Reverb master switch (alFxReverbSet, driven by MDKR_AUDIO_REVERB in
     * game/src/audio.c). The upstream source is pulled FIRST and
     * unconditionally, so the dry signal always reaches the bus; only the
     * delay-line/wet stage below is skipped. That is what makes the
     * reverb-on/reverb-off capture pair a true A/B of the wet path alone.
     */
    if (alFXEnabled == FALSE) {
        return ptr;
    }

    if (fx->base == NULL || fx->length == 0) {
        return ptr;
    }

    aSetBuffer(ptr++, 0, 0, 0, out_count << 1);
    aMix(ptr++, 0, 0xda83, AL_AUX_L_OUT, input);
    aMix(ptr++, 0, 0x5a82, AL_AUX_R_OUT, input);
    ptr = native_fx_save_buffer(fx, fx->input, input, out_count, ptr);
    aClearBuffer(ptr++, output, out_count << 1);

    for (i = 0; i < fx->section_count; i++) {
        ALDelay *delay = &fx->delay[i];
        s16 *input_ptr = native_fx_ring_relative(
            fx, -(s64)(s32)delay->input);
        s16 *output_ptr = native_fx_ring_relative(
            fx, -(s64)(s32)delay->output);

        if (input_ptr == NULL || output_ptr == NULL) {
            native_fx_note_guard("section-tap", fx, input_ptr, out_count);
            continue;
        }

        if (previous_output_valid &&
            (uintptr_t)input_ptr == previous_output) {
            s16 tmp = buffer_b;
            buffer_b = buffer_a;
            buffer_a = tmp;
        } else {
            ptr = native_fx_load_buffer(fx, input_ptr, buffer_a, out_count,
                                        ptr);
        }

        ptr = native_fx_load_output_buffer(fx, delay, buffer_b, out_count,
                                           ptr);

        if (delay->ffcoef != 0) {
            aMix(ptr++, 0, (u16)delay->ffcoef, buffer_a, buffer_b);
            if (delay->rs == NULL && delay->lp == NULL) {
                ptr = native_fx_save_buffer(fx, output_ptr, buffer_b,
                                            out_count, ptr);
            }
        }

        if (delay->fbcoef != 0) {
            aMix(ptr++, 0, (u16)delay->fbcoef, buffer_b, buffer_a);
            ptr = native_fx_save_buffer(fx, input_ptr, buffer_a, out_count,
                                        ptr);
        }

        if (delay->lp != NULL) {
            ptr = native_fx_filter_buffer(delay->lp, buffer_b, out_count, ptr);
        }

        if (delay->rs == NULL) {
            ptr = native_fx_save_buffer(fx, output_ptr, buffer_b, out_count,
                                        ptr);
        }

        if (delay->gain != 0) {
            aMix(ptr++, 0, (u16)delay->gain, buffer_b, output);
        }

        /* This is intentionally the original POSITIVE output token used only
         * for equality on the next section. Keep it as integer arithmetic so
         * forming a far-outside C pointer cannot itself invoke UB on LP64. */
        previous_output =
            (uintptr_t)fx->input +
            (uintptr_t)delay->output * sizeof(*fx->input);
        previous_output_valid = TRUE;
    }

    {
        u32 input_offset;
        u64 advanced;

        if (!native_fx_pointer_offset(fx, fx->input, &input_offset)) {
            native_fx_note_guard("advance", fx, fx->input, out_count);
            fx->input = fx->base;
        } else {
            advanced = (u64)input_offset + (u32)out_count;
            if (advanced > fx->length) {
                if (advanced > (u64)fx->length * 2u) {
                    native_fx_note_guard("advance", fx, fx->input, out_count);
                    advanced %= fx->length;
                } else {
                    advanced -= fx->length;
                }
            }
            fx->input = &fx->base[(u32)advanced];
        }
    }

    aDMEMMove(ptr++, output, AL_AUX_L_OUT, out_count << 1);
    return ptr;
}

s32 alFxParam(void *filter, s32 param_id, void *param)
{
    if (filter != NULL && param_id == AL_FILTER_SET_SOURCE) {
        ((ALFilter *)filter)->source = (ALFilter *)param;
    }

    return 0;
}

s32 alFxParamHdl(void *filter, s32 param_id, void *param)
{
    ALFx *fx = (ALFx *)filter;
    s32 slot;
    s32 field;
    s32 value;

    if (fx == NULL || fx->delay == NULL || param == NULL) {
        return 0;
    }

    field = (param_id - 2) % 8;
    slot = (param_id - 2) / 8;
    if (slot < 0 || slot >= fx->section_count) {
        return 0;
    }

    value = *(s32 *)param;
    switch (field) {
    case 0:
        fx->delay[slot].input = (u32)value;
        break;
    case 1:
        fx->delay[slot].output = (u32)value;
        break;
    case 2:
        fx->delay[slot].fbcoef = (s16)value;
        break;
    case 3:
        fx->delay[slot].ffcoef = (s16)value;
        break;
    case 4:
        fx->delay[slot].gain = (s16)value;
        break;
    case 5:
        fx->delay[slot].rsinc = (f32)value / 16777215.0f;
        break;
    case 6:
        fx->delay[slot].rsgain =
            ((f32)value / NATIVE_FX_CENTS_DENOM) *
            (f32)((s32)fx->delay[slot].output -
                  (s32)fx->delay[slot].input);
        break;
    case 7:
        if (fx->delay[slot].lp != NULL) {
            fx->delay[slot].lp->fc = (s16)value;
        }
        break;
    default:
        break;
    }

    return 0;
}

void alFxNew(ALFx *fx, ALSynConfig *config, ALHeap *heap)
{
    ALFilter *filter;
    s32 *params;
    s32 section_count;
    s32 length;
    s32 idx;
    s32 i;

    if (fx == NULL || heap == NULL) {
        return;
    }

    memset(fx, 0, sizeof(*fx));
    filter = (ALFilter *)fx;
    alFilterNew(filter, 0, alFxParam, AL_FX);
    filter->handler = alFxPull;
    fx->paramHdl = (ALSetFXParam)alFxParamHdl;

    params = native_fx_params_for_config(config);
    section_count = params[0];
    length = params[1];
    if (section_count < 0 || section_count > 64 || length <= 0 ||
        length > INT_MAX / (s32)sizeof(*fx->base) -
                     (s32)PORT_AUDIO_FX_TAIL_SLACK_SAMPLES) {
        params = s_native_fx_bypass_params;
        section_count = params[0];
        length = params[1];
    }

    fx->section_count = (u8)section_count;
    fx->length = (u32)length;
    fx->delay = alHeapAlloc(heap, section_count, sizeof(*fx->delay));
    fx->base = alHeapAlloc(
        heap, length + (s32)PORT_AUDIO_FX_TAIL_SLACK_SAMPLES,
        sizeof(*fx->base));
    fx->input = fx->base;
    if (fx->base == NULL || fx->delay == NULL) {
        fx->section_count = 0;
        fx->length = 0;
        fx->input = NULL;
        return;
    }

    idx = 2;
    for (i = 0; i < section_count; i++) {
        ALDelay *delay = &fx->delay[i];
        s32 chorus_rate;
        s32 chorus_depth;
        s32 low_pass_fc;

        delay->input = (u32)params[idx++];
        delay->output = (u32)params[idx++];
        delay->fbcoef = (s16)params[idx++];
        delay->ffcoef = (s16)params[idx++];
        delay->gain = (s16)params[idx++];
        chorus_rate = params[idx++];
        chorus_depth = params[idx++];
        low_pass_fc = params[idx++];

        if (chorus_rate != 0 && config != NULL && config->outputRate > 0) {
            delay->rsinc = (((f32)chorus_rate / 1000.0f) *
                            NATIVE_FX_RATE_RANGE) /
                           (f32)config->outputRate;
            delay->rsgain =
                ((f32)chorus_depth / NATIVE_FX_CENTS_DENOM) *
                (f32)((s32)delay->output - (s32)delay->input);
            delay->rsval = 1.0f;
            delay->rsdelta = 0;
            delay->rs = alHeapAlloc(heap, 1, sizeof(*delay->rs));
            if (delay->rs != NULL) {
                delay->rs->state = alHeapAlloc(heap, 1, sizeof(RESAMPLE_STATE));
                delay->rs->delta = 0.0f;
                delay->rs->first = 1;
            }
        }

        if (low_pass_fc != 0) {
            delay->lp = alHeapAlloc(heap, 1, sizeof(*delay->lp));
            if (delay->lp != NULL) {
                delay->lp->fstate = alHeapAlloc(heap, 1, sizeof(POLEF_STATE));
                delay->lp->fc = (s16)low_pass_fc;
                init_lpfilter(delay->lp);
            }
        }
    }
}

void alEnvmixerNew(ALEnvMixer *envmixer, ALHeap *heap)
{
    if (envmixer == NULL || heap == NULL) {
        return;
    }

    memset(envmixer, 0, sizeof(*envmixer));
    alFilterNew((ALFilter *)envmixer, alEnvmixerPull, alEnvmixerParam,
                AL_ENVMIX);
    envmixer->state = alHeapAlloc(heap, 1, sizeof(ENVMIX_STATE));
    envmixer->first = 1;
    envmixer->motion = AL_STOPPED;
    envmixer->volume = 1;
    envmixer->ltgt = 1;
    envmixer->rtgt = 1;
    envmixer->cvolL = 1;
    envmixer->cvolR = 1;
    envmixer->lratm = 1;
    envmixer->rratm = 1;
}

void alLoadNew(ALLoadFilter *load, ALDMANew dma_new, ALHeap *heap)
{
    if (load == NULL || heap == NULL) {
        return;
    }

    memset(load, 0, sizeof(*load));
    alFilterNew((ALFilter *)load, alAdpcmPull, alLoadParam, AL_ADPCM);
    load->state = alHeapAlloc(heap, 1, sizeof(ADPCM_STATE));
    load->lstate = alHeapAlloc(heap, 1, sizeof(ADPCM_STATE));
    if (dma_new != NULL) {
        load->dma = dma_new(&load->dmaState);
    }
    load->first = 1;
}

void alResampleNew(ALResampler *resampler, ALHeap *heap)
{
    if (resampler == NULL || heap == NULL) {
        return;
    }

    memset(resampler, 0, sizeof(*resampler));
    alFilterNew((ALFilter *)resampler, alResamplePull, alResampleParam,
                AL_RESAMPLE);
    resampler->state = alHeapAlloc(heap, 1, sizeof(RESAMPLE_STATE));
    resampler->first = 1;
    resampler->motion = AL_STOPPED;
    resampler->ratio = 1.0f;
}

void alAuxBusNew(ALAuxBus *bus, void *sources, s32 max_sources)
{
    if (bus == NULL) {
        return;
    }

    memset(bus, 0, sizeof(*bus));
    alFilterNew((ALFilter *)bus, alAuxBusPull, alAuxBusParam, AL_AUXBUS);
    bus->sourceCount = 0;
    bus->maxSources = max_sources;
    bus->sources = (ALFilter **)sources;
}

void alMainBusNew(ALMainBus *bus, void *sources, s32 max_sources)
{
    if (bus == NULL) {
        return;
    }

    memset(bus, 0, sizeof(*bus));
    alFilterNew((ALFilter *)bus, alMainBusPull, alMainBusParam, AL_MAINBUS);
    bus->sourceCount = 0;
    bus->maxSources = max_sources;
    bus->sources = (ALFilter **)sources;
}

void alSaveNew(ALSave *save)
{
    if (save == NULL) {
        return;
    }

    memset(save, 0, sizeof(*save));
    alFilterNew((ALFilter *)save, alSavePull, alSaveParam, AL_SAVE);
    save->first = 1;
}

void alSynNew(ALSynth *synth, ALSynConfig *config)
{
    ALHeap *heap;
    ALSave *save;
    PVoice *voices;
    ALFilter **sources;
    ALParam *params;
    s32 i;

    if (synth == NULL || config == NULL || config->heap == NULL) {
        return;
    }

    memset(synth, 0, sizeof(*synth));
    heap = config->heap;

    if (config->maxPVoices <= 0 || config->maxUpdates <= 0 ||
        config->outputRate <= 0) {
        return;
    }

    synth->numPVoices = config->maxPVoices;
    synth->outputRate = config->outputRate;
    synth->maxOutSamples = AL_MAX_RSP_SAMPLES;
    synth->dma = (ALDMANew)config->dmaproc;
    synth->heap = heap;

    save = alHeapAlloc(heap, 1, (s32)sizeof(*save));
    if (save == NULL) {
        return;
    }

    alSaveNew(save);
    synth->outputFilter = (ALFilter *)save;

    synth->auxBus = alHeapAlloc(heap, 1, sizeof(*synth->auxBus));
    synth->mainBus = alHeapAlloc(heap, 1, sizeof(*synth->mainBus));
    if (synth->auxBus == NULL || synth->mainBus == NULL) {
        return;
    }

    synth->maxAuxBusses = 1;
    sources = alHeapAlloc(heap, config->maxPVoices, sizeof(*sources));
    if (sources == NULL) {
        return;
    }
    alAuxBusNew(synth->auxBus, sources, config->maxPVoices);

    sources = alHeapAlloc(heap, config->maxPVoices, sizeof(*sources));
    if (sources == NULL) {
        return;
    }
    alMainBusNew(synth->mainBus, sources, config->maxPVoices);

    if (config->fxType[0] != AL_FX_NONE) {
        alSynAllocFX(synth, 0, config, heap);
    } else {
        alMainBusParam(synth->mainBus, AL_FILTER_ADD_SOURCE, &synth->auxBus[0]);
    }

    voices = alHeapAlloc(heap, config->maxPVoices, sizeof(*voices));
    if (voices == NULL) {
        return;
    }
    for (i = 0; i < config->maxPVoices; i++) {
        PVoice *physical = &voices[i];

        alLink((ALLink *)physical, &synth->pFreeList);
        physical->vvoice = NULL;
        physical->offset = 0;

        alLoadNew(&physical->decoder, synth->dma, heap);
        alLoadParam(&physical->decoder, AL_FILTER_SET_SOURCE, NULL);

        alResampleNew(&physical->resampler, heap);
        alResampleParam(&physical->resampler, AL_FILTER_SET_SOURCE,
                        &physical->decoder);

        alEnvmixerNew(&physical->envmixer, heap);
        alEnvmixerParam(&physical->envmixer, AL_FILTER_SET_SOURCE,
                        &physical->resampler);

        alAuxBusParam(synth->auxBus, AL_FILTER_ADD_SOURCE,
                      &physical->envmixer);
        physical->channelKnob = (ALFilter *)&physical->envmixer;
    }

    alSaveParam(save, AL_FILTER_SET_SOURCE, synth->mainBus);

    params = alHeapAlloc(heap, config->maxUpdates, sizeof(ALStartParamAlt));
    if (params == NULL) {
        return;
    }
    synth->paramList = NULL;
    for (i = 0; i < config->maxUpdates; i++) {
        ALParam *param =
            (ALParam *)((u8 *)params + (size_t)i * sizeof(ALStartParamAlt));

        param->next = synth->paramList;
        synth->paramList = param;
    }
}

Acmd *alAudioFrame(Acmd *cmd_list, s32 *cmd_len, s16 *out_buf, s32 out_len)
{
    ALSynth *synth;
    Acmd *cmd_end = cmd_list;
    s16 tmp = 0;

    if (cmd_len == NULL) {
        return cmd_list;
    }

    *cmd_len = 0;
    if (alGlobals == NULL || cmd_list == NULL || out_buf == NULL ||
        out_len <= 0) {
        return cmd_list;
    }

    synth = &alGlobals->drvr;
    if (synth->head == NULL || synth->outputFilter == NULL ||
        synth->outputRate <= 0) {
        return cmd_list;
    }

    for (;;) {
        ALPlayer *client = NULL;

        synth->paramSamples = native_next_sample_time(synth, &client);
        if (client == NULL || client->handler == NULL ||
            synth->paramSamples - synth->curSamples >= out_len) {
            break;
        }

        synth->paramSamples &= ~0xf;
        client->samplesLeft += native_time_to_samples_no_round(
            synth, client->handler(client));
    }

    synth->paramSamples &= ~0xf;

    while (out_len > 0) {
        ALFilter *output = synth->outputFilter;
        Acmd *cmd_ptr = cmd_end;
        s32 chunk_samples = out_len;

        if (chunk_samples > synth->maxOutSamples) {
            chunk_samples = synth->maxOutSamples;
        }

        aSegment(cmd_ptr++, 0, 0);
        if (output->setParam == NULL || output->handler == NULL) {
            break;
        }
        output->setParam(output, AL_FILTER_SET_DRAM, out_buf);
        cmd_end = output->handler(output, &tmp, chunk_samples,
                                  synth->curSamples, cmd_ptr);

        out_len -= chunk_samples;
        out_buf += chunk_samples << 1;
        synth->curSamples += chunk_samples;
    }

    *cmd_len = (s32)(cmd_end - cmd_list);
    _collectPVoices(synth);
    return cmd_end;
}

ALParam *__allocParam(void)
{
    ALSynth *synth;
    ALParam *param;

    if (alGlobals == NULL) {
        return NULL;
    }

    synth = &alGlobals->drvr;
    param = synth->paramList;
    if (param != NULL) {
        synth->paramList = param->next;
        param->next = NULL;
    }

    return param;
}

void __freeParam(ALParam *param)
{
    ALSynth *synth;

    if (alGlobals == NULL || param == NULL) {
        return;
    }

    synth = &alGlobals->drvr;
    param->next = synth->paramList;
    synth->paramList = param;
}

void _collectPVoices(ALSynth *synth)
{
    ALLink *node;

    if (synth == NULL) {
        return;
    }

    while ((node = synth->pLameList.next) != NULL) {
        alUnlink(node);
        alLink(node, &synth->pFreeList);
    }
}

void _freePVoice(ALSynth *synth, PVoice *pvoice)
{
    if (synth == NULL || pvoice == NULL) {
        return;
    }

    alUnlink((ALLink *)pvoice);
    alLink((ALLink *)pvoice, &synth->pLameList);
}

s32 _timeToSamples(ALSynth *synth, s32 micros)
{
    return native_time_to_samples_no_round(synth, micros) & ~0xf;
}

void alInit(ALGlobals *globals, ALSynConfig *config)
{
    if (alGlobals != NULL || globals == NULL || config == NULL) {
        return;
    }

    alGlobals = globals;
    alSynNew(&alGlobals->drvr, config);
}

void alClose(ALGlobals *globals)
{
    if (alGlobals == NULL) {
        return;
    }

    if (globals != NULL) {
        alSynDelete(&globals->drvr);
    } else {
        alSynDelete(&alGlobals->drvr);
    }

    alGlobals = NULL;
}

void alCopy(void *src, void *dest, s32 len)
{
    if (src == NULL || dest == NULL || len <= 0) {
        return;
    }

    memcpy(dest, src, (size_t)len);
}

f32 alCents2Ratio(s32 cents)
{
    return audio_exp2f((f32)cents / 1200.0f);
}

void alHeapInit(ALHeap *hp, u8 *base, s32 len)
{
    if (hp == NULL) {
        return;
    }

    hp->base = base;
    hp->cur = base;
    hp->len = len;
    hp->count = 0;
}

/*
 * ARENA-RESIDENT BUMP ALLOCATOR — do not replace with calloc().
 *
 * mgb64 can malloc audio memory because its mixer receives real pointers. DKR
 * cannot: platform/mixer.h's MIXER_RESOLVE() rebuilds every address the mixer
 * touches from its low 32 bits via dkr_lo32_to_ptr() (= g_dkrArenaHi | lo32),
 * which is only correct for pointers inside the size-aligned ARENA. A malloc'd
 * buffer has different high bits, so the reconstruction would hand the mixer a
 * wild pointer. game/src/audio.c backs ALHeap with arena memory, so bump-
 * allocating within it keeps every buffer arena-resident and the round-trip
 * exact. Same invariant the deleted heapalloc.c maintained.
 */
void *alHeapDBAlloc(u8 *file, s32 line, ALHeap *hp, s32 num, s32 size)
{
    s32 bytes;
    u8 *ptr;

    (void)file;
    (void)line;

    if (hp == NULL || num <= 0 || size <= 0) {
        return NULL;
    }

    /* Overflow-safe: reject before the multiply can wrap. */
    if (num > (s32)(0x7FFFFFFF / size)) {
        return NULL;
    }

    bytes = (num * size + AL_CACHE_ALIGN) & ~AL_CACHE_ALIGN;
    if (bytes < 0 || hp->cur + bytes > hp->base + hp->len) {
        return NULL;
    }

    ptr = hp->cur;
    hp->cur += bytes;
    hp->count++;
    /* The stock allocator hands back zeroed memory and callers rely on it
     * (e.g. PVoice::unkDC's implicit aux-bus-0 parentage). */
    memset(ptr, 0, (size_t)bytes);
    return ptr;
}

void alSynDelete(ALSynth *s)
{
    if (s == NULL) {
        return;
    }

    s->head = NULL;
}

void alSynSetPriority(ALSynth *s, ALVoice *voice, s16 priority)
{
    (void)s;
    if (voice == NULL) {
        return;
    }

    voice->priority = priority;
}

/*
 * DKR channel state.
 *
 * DKR's ALCSPlayer_Custom is layout-identical to the stock ALCSPlayer except
 * that chanState points at ALChanState_Custom, which extends ALChanState with
 * the per-channel `fade` (CC8) and `unk11` (raw CC7) bytes the DKR sequences
 * drive. The extra fields sit AFTER every stock field, so the common members
 * keep their offsets and only the array stride grows — which is why every
 * access goes through this cast and why alCSPNew sizes the array with
 * sizeof(ALChanState_Custom).
 */
#define CSP_CHAN(p) (((ALCSPlayer_Custom *)(p))->chanState)

static void native_seqp_remove_event_for_voice(ALSeqPlayer *seqp, ALVoice *voice,
                                               s16 event_type)
{
    ALLink *node;

    if (seqp == NULL || voice == NULL) {
        return;
    }

    node = seqp->evtq.allocList.next;
    while (node != NULL) {
        ALLink *next = node->next;
        ALEventListItem *item = (ALEventListItem *)node;
        ALEventListItem *next_item = (ALEventListItem *)next;
        ALVoice *event_voice = NULL;

        if (item->evt.type == AL_SEQP_ENV_EVT) {
            event_voice = item->evt.msg.vol.voice;
        } else if (item->evt.type == AL_NOTE_END_EVT) {
            event_voice = item->evt.msg.note.voice;
        }

        if (item->evt.type == event_type && event_voice == voice) {
            if (next_item != NULL) {
                next_item->delta += item->delta;
            }
            alUnlink(node);
            alLink(node, &seqp->evtq.freeList);
            portAudioEventQueueNoteRemoved(&seqp->evtq);
        }

        node = next;
    }
}

static void native_seqp_repost_event_item(ALEventQueue *evtq,
                                          ALEventListItem *item)
{
    ALLink *node;
    OSIntMask mask;

    if (evtq == NULL || item == NULL) {
        return;
    }

    item->node.next = NULL;
    item->node.prev = NULL;
    mask = osSetIntMask(OS_IM_NONE);

    for (node = &evtq->allocList; node != NULL; node = node->next) {
        ALEventListItem *next_item;

        if (node->next == NULL) {
            alLink((ALLink *)item, node);
            break;
        }

        next_item = (ALEventListItem *)node->next;
        if (item->delta < next_item->delta) {
            next_item->delta -= item->delta;
            alLink((ALLink *)item, node);
            break;
        }

        item->delta -= next_item->delta;
    }

    portAudioEventQueueNoteInserted(evtq);
    osSetIntMask(mask);
}

ALVoiceState *__mapVoice(ALSeqPlayer *seqp, u8 key, u8 vel, u8 channel)
{
    ALVoiceState *voice_state;

    if (seqp == NULL || seqp->vFreeList == NULL) {
        return NULL;
    }

    /*
     * DKR per-player polyphony cap (set_voice_limit), independent of the
     * synth's shared physical-voice pool. Exceeding it DROPS the note rather
     * than stealing — the sequence just loses that voice. The comparison is
     * deliberately `limit < mapped` rather than `mapped >= limit`, matching
     * the original's effective cap of voiceLimit + 1 simultaneous voices;
     * tightening it here would audibly thin dense passages.
     */
    if (seqp->voiceLimit < seqp->mappedVoices) {
        return NULL;
    }

    voice_state = seqp->vFreeList;
    seqp->vFreeList = voice_state->next;
    memset(voice_state, 0, sizeof(*voice_state));
    seqp->mappedVoices++;
    voice_state->channel = channel;
    voice_state->key = key;
    voice_state->velocity = vel;
    voice_state->voice.clientPrivate = voice_state;

    if (seqp->vAllocTail != NULL) {
        seqp->vAllocTail->next = voice_state;
    } else {
        seqp->vAllocHead = voice_state;
    }
    seqp->vAllocTail = voice_state;

    return voice_state;
}

void __unmapVoice(ALSeqPlayer *seqp, ALVoice *voice)
{
    ALVoiceState *prev = NULL;
    ALVoiceState *cur;

    if (seqp == NULL || voice == NULL) {
        return;
    }

    for (cur = seqp->vAllocHead; cur != NULL; cur = cur->next) {
        if (&cur->voice == voice) {
            if (prev != NULL) {
                prev->next = cur->next;
            } else {
                seqp->vAllocHead = cur->next;
            }

            if (seqp->vAllocTail == cur) {
                seqp->vAllocTail = prev;
            }

            cur->next = seqp->vFreeList;
            seqp->vFreeList = cur;
            if (seqp->mappedVoices != 0) {
                seqp->mappedVoices--;
            }
            return;
        }

        prev = cur;
    }
}

ALVoiceState *__lookupVoice(ALSeqPlayer *seqp, u8 key, u8 channel)
{
    ALVoiceState *voice_state;

    if (seqp == NULL) {
        return NULL;
    }

    for (voice_state = seqp->vAllocHead; voice_state != NULL;
         voice_state = voice_state->next) {
        if (voice_state->key == key &&
            voice_state->channel == channel &&
            voice_state->phase != AL_PHASE_RELEASE &&
            voice_state->phase != AL_PHASE_SUSTREL) {
            return voice_state;
        }
    }

    return NULL;
}

ALSound *__lookupSound(ALSeqPlayer *seqp, u8 key, u8 vel, u8 chan)
{
    ALInstrument *instrument;
    s32 i;

    if (seqp == NULL || chan >= seqp->maxChannels ||
        CSP_CHAN(seqp) == NULL) {
        return NULL;
    }

    instrument = CSP_CHAN(seqp)[chan].instrument;
    if (instrument == NULL) {
        return NULL;
    }

    for (i = 0; i < instrument->soundCount; i++) {
        ALSound *sound = instrument->soundArray[i];
        ALKeyMap *keymap;

        if (sound == NULL || sound->keyMap == NULL) {
            continue;
        }

        keymap = sound->keyMap;
        if (key >= keymap->keyMin && key <= keymap->keyMax &&
            vel >= keymap->velocityMin && vel <= keymap->velocityMax) {
            return sound;
        }
    }

    return NULL;
}

ALSound *__lookupSoundQuick(ALSeqPlayer *seqp, u8 key, u8 vel, u8 chan)
{
    ALInstrument *instrument;
    s32 left;
    s32 right;

    if (seqp == NULL || chan >= seqp->maxChannels ||
        CSP_CHAN(seqp) == NULL) {
        return NULL;
    }

    instrument = CSP_CHAN(seqp)[chan].instrument;
    if (instrument == NULL || instrument->soundCount <= 0) {
        return NULL;
    }

    left = 0;
    right = instrument->soundCount - 1;
    while (left <= right) {
        s32 mid = (left + right) / 2;
        ALSound *sound = instrument->soundArray[mid];
        ALKeyMap *keymap;

        if (sound == NULL || sound->keyMap == NULL) {
            return __lookupSound(seqp, key, vel, chan);
        }

        keymap = sound->keyMap;
        if (key >= keymap->keyMin && key <= keymap->keyMax &&
            vel >= keymap->velocityMin && vel <= keymap->velocityMax) {
            return sound;
        }

        if (key < keymap->keyMin ||
            (vel < keymap->velocityMin && key <= keymap->keyMax)) {
            right = mid - 1;
        } else {
            left = mid + 1;
        }
    }

    return NULL;
}

s16 __vsVol(ALVoiceState *voice_state, ALSeqPlayer *seqp)
{
    u32 note_gain;
    u32 mix_gain;

    if (voice_state == NULL || seqp == NULL || voice_state->sound == NULL ||
        voice_state->channel >= seqp->maxChannels || CSP_CHAN(seqp) == NULL) {
        return 0;
    }

    note_gain = ((u32)voice_state->tremelo * voice_state->velocity *
                 voice_state->envGain) >> 6;
    mix_gain = ((u32)voice_state->sound->sampleVolume * (u16)seqp->vol *
                CSP_CHAN(seqp)[voice_state->channel].vol) >> 14;
    note_gain *= mix_gain;
    note_gain >>= 15;

    return (s16)note_gain;
}

ALMicroTime __vsDelta(ALVoiceState *voice_state, ALMicroTime t)
{
    ALMicroTime remaining;

    if (voice_state == NULL) {
        return AL_GAIN_CHANGE_TIME;
    }

    remaining = voice_state->envEndTime - t;
    return remaining >= 0 ? remaining : AL_GAIN_CHANGE_TIME;
}

ALPan __vsPan(ALVoiceState *voice_state, ALSeqPlayer *seqp)
{
    s32 pan;

    if (voice_state == NULL || seqp == NULL || voice_state->sound == NULL ||
        voice_state->channel >= seqp->maxChannels || CSP_CHAN(seqp) == NULL) {
        return AL_PAN_CENTER;
    }

    pan = CSP_CHAN(seqp)[voice_state->channel].pan -
          AL_PAN_CENTER + voice_state->sound->samplePan;
    if (pan < AL_PAN_LEFT) {
        pan = AL_PAN_LEFT;
    } else if (pan > AL_PAN_RIGHT) {
        pan = AL_PAN_RIGHT;
    }

    return (ALPan)pan;
}

void __seqpReleaseVoice(ALSeqPlayer *seqp, ALVoice *voice,
                        ALMicroTime delta_time)
{
    ALEvent event;
    ALVoiceState *voice_state;

    if (seqp == NULL || voice == NULL) {
        return;
    }

    voice_state = (ALVoiceState *)voice->clientPrivate;
    if (voice_state == NULL) {
        return;
    }

    if (voice_state->envPhase == AL_PHASE_ATTACK) {
        native_seqp_remove_event_for_voice(seqp, voice, AL_SEQP_ENV_EVT);
    }

    voice_state->velocity = 0;
    voice_state->envPhase = AL_PHASE_RELEASE;
    voice_state->envGain = 0;
    voice_state->envEndTime = seqp->curTime + delta_time;

    alSynSetPriority(seqp->drvr, voice, 0);
    alSynSetVol(seqp->drvr, voice, 0, delta_time);

    event.type = AL_NOTE_END_EVT;
    event.msg.note.voice = voice;
    alEvtqPostEvent(&seqp->evtq, &event, delta_time);
}

char __voiceNeedsNoteKill(ALSeqPlayer *seqp, ALVoice *voice,
                          ALMicroTime kill_time)
{
    ALLink *node;
    ALMicroTime event_time = 0;

    if (seqp == NULL || voice == NULL) {
        return FALSE;
    }

    node = seqp->evtq.allocList.next;
    while (node != NULL) {
        ALLink *next = node->next;
        ALEventListItem *item = (ALEventListItem *)node;

        event_time += item->delta;
        if (item->evt.type == AL_NOTE_END_EVT &&
            item->evt.msg.note.voice == voice) {
            if (event_time <= kill_time) {
                return FALSE;
            }

            if (next != NULL) {
                ((ALEventListItem *)next)->delta += item->delta;
            }
            alUnlink(node);
            alLink(node, &seqp->evtq.freeList);
            portAudioEventQueueNoteRemoved(&seqp->evtq);
            return TRUE;
        }

        node = next;
    }

    return TRUE;
}

void __resetPerfChanState(ALSeqPlayer *seqp, s32 chan)
{
    if (seqp == NULL || CSP_CHAN(seqp) == NULL ||
        chan < 0 || chan >= seqp->maxChannels) {
        return;
    }

    CSP_CHAN(seqp)[chan].fxId = AL_FX_NONE;
    CSP_CHAN(seqp)[chan].fxmix = AL_DEFAULT_FXMIX;
    CSP_CHAN(seqp)[chan].pan = AL_PAN_CENTER;
    CSP_CHAN(seqp)[chan].vol = AL_VOL_FULL;
    CSP_CHAN(seqp)[chan].priority = AL_DEFAULT_PRIORITY;
    CSP_CHAN(seqp)[chan].sustain = 0;
    CSP_CHAN(seqp)[chan].bendRange = 200;
    CSP_CHAN(seqp)[chan].pitchBend = 1.0f;
    /*
     * Both volume lanes start at full so their product is AL_VOL_FULL, which
     * is the reset volume above. The SGI original never initialised the raw
     * CC7 lane at all and relied on a CC7 arriving before any CC8; a sequence
     * that faded in first would multiply by uninitialised memory and could
     * silence the channel. Seeding both removes that ordering dependency.
     */
    CSP_CHAN(seqp)[chan].fade = AL_VOL_FULL;
    CSP_CHAN(seqp)[chan].unk11 = AL_VOL_FULL;
}

void __setInstChanState(ALSeqPlayer *seqp, ALInstrument *instrument, s32 chan)
{
    if (seqp == NULL || CSP_CHAN(seqp) == NULL || instrument == NULL ||
        chan < 0 || chan >= seqp->maxChannels) {
        return;
    }

    CSP_CHAN(seqp)[chan].instrument = instrument;
    CSP_CHAN(seqp)[chan].pan = instrument->pan;
    CSP_CHAN(seqp)[chan].vol = instrument->volume;
    CSP_CHAN(seqp)[chan].priority = instrument->priority;
    CSP_CHAN(seqp)[chan].bendRange = instrument->bendRange;
}

void __initChanState(ALSeqPlayer *seqp)
{
    s32 i;

    if (seqp == NULL || CSP_CHAN(seqp) == NULL) {
        return;
    }

    for (i = 0; i < seqp->maxChannels; i++) {
        CSP_CHAN(seqp)[i].instrument = NULL;
        __resetPerfChanState(seqp, i);
    }
}

void __initFromBank(ALSeqPlayer *seqp, ALBank *bank)
{
    ALInstrument *first_instrument = NULL;
    s32 i;

    if (seqp == NULL || bank == NULL || CSP_CHAN(seqp) == NULL) {
        return;
    }

    for (i = 0; i < bank->instCount && first_instrument == NULL; i++) {
        first_instrument = bank->instArray[i];
    }
    if (first_instrument == NULL) {
        return;
    }

    for (i = 0; i < seqp->maxChannels; i++) {
        __resetPerfChanState(seqp, i);
        __setInstChanState(seqp, first_instrument, i);
    }

    if (bank->percussion != NULL && seqp->maxChannels > 9) {
        __resetPerfChanState(seqp, 9);
        __setInstChanState(seqp, bank->percussion, 9);
    }
}

void __seqpStopOsc(ALSeqPlayer *seqp, ALVoiceState *voice_state)
{
    ALLink *node;

    if (seqp == NULL || voice_state == NULL) {
        return;
    }

    node = seqp->evtq.allocList.next;
    while (node != NULL) {
        ALLink *next = node->next;
        ALEventListItem *item = (ALEventListItem *)node;
        ALEventListItem *next_item = (ALEventListItem *)next;

        if ((item->evt.type == AL_TREM_OSC_EVT ||
             item->evt.type == AL_VIB_OSC_EVT) &&
            item->evt.msg.osc.vs == voice_state) {
            if (seqp->stopOsc != NULL) {
                seqp->stopOsc(item->evt.msg.osc.oscState);
            }
            if (next_item != NULL) {
                next_item->delta += item->delta;
            }
            alUnlink(node);
            alLink(node, &seqp->evtq.freeList);
            portAudioEventQueueNoteRemoved(&seqp->evtq);
        }

        node = next;
    }
}

static void native_csp_set_uspt_from_tempo(ALCSPlayer *seqp, f32 tempo)
{
    if (seqp != NULL && seqp->target != NULL) {
        seqp->uspt = (s32)(tempo * seqp->target->qnpt);
    } else if (seqp != NULL) {
        seqp->uspt = 488;
    }
}

static void native_csp_update_channel_volumes(ALCSPlayer *seqp, u8 chan)
{
    ALVoiceState *voice_state;

    if (seqp == NULL) {
        return;
    }

    for (voice_state = seqp->vAllocHead; voice_state != NULL;
         voice_state = voice_state->next) {
        if (voice_state->channel == chan &&
            voice_state->envPhase != AL_PHASE_RELEASE) {
            alSynSetVol(seqp->drvr,
                        &voice_state->voice,
                        __vsVol(voice_state, (ALSeqPlayer *)seqp),
                        __vsDelta(voice_state, seqp->curTime));
        }
    }
}

static void native_csp_update_all_volumes(ALCSPlayer *seqp)
{
    ALVoiceState *voice_state;

    if (seqp == NULL) {
        return;
    }

    for (voice_state = seqp->vAllocHead; voice_state != NULL;
         voice_state = voice_state->next) {
        alSynSetVol(seqp->drvr,
                    &voice_state->voice,
                    __vsVol(voice_state, (ALSeqPlayer *)seqp),
                    __vsDelta(voice_state, seqp->curTime));
    }
}

void __CSPPostNextSeqEvent(ALCSPlayer *seqp)
{
    ALEvent event;
    s32 delta_ticks;

    if (seqp == NULL || seqp->state != AL_PLAYING || seqp->target == NULL) {
        return;
    }

    if (!__alCSeqNextDelta(seqp->target, &delta_ticks)) {
        return;
    }

    event.type = AL_SEQ_REF_EVT;
    alEvtqPostEvent(&seqp->evtq, &event, delta_ticks * seqp->uspt);
}

static void native_csp_handle_meta(ALCSPlayer *seqp, ALEvent *event)
{
    ALEventListItem *deferred_head = NULL;
    ALEventListItem *deferred_tail = NULL;
    ALEventListItem *node;
    ALMicroTime running_delta = 0;
    s32 old_uspt;
    s32 tempo;

    if (seqp == NULL || event == NULL ||
        event->msg.tempo.status != AL_MIDI_Meta ||
        event->msg.tempo.type != AL_MIDI_META_TEMPO) {
        return;
    }

    old_uspt = seqp->uspt;
    tempo = ((s32)event->msg.tempo.byte1 << 16) |
            ((s32)event->msg.tempo.byte2 << 8) |
            (s32)event->msg.tempo.byte3;
    native_csp_set_uspt_from_tempo(seqp, (f32)tempo);
    if (old_uspt <= 0) {
        return;
    }

    node = (ALEventListItem *)seqp->evtq.allocList.next;
    while (node != NULL) {
        ALEventListItem *next = (ALEventListItem *)node->node.next;

        running_delta += node->delta;
        if (node->evt.type == AL_CSP_NOTEOFF_EVT) {
            ALMicroTime absolute_delta = running_delta;

            if (next != NULL) {
                next->delta += node->delta;
                running_delta -= node->delta;
            }
            alUnlink((ALLink *)node);
            portAudioEventQueueNoteRemoved(&seqp->evtq);
            node->node.prev = NULL;
            node->node.next = NULL;
            if (deferred_tail != NULL) {
                deferred_tail->node.next = (ALLink *)node;
                node->node.prev = (ALLink *)deferred_tail;
            } else {
                deferred_head = node;
            }
            deferred_tail = node;
            node->delta = absolute_delta;
        }

        node = next;
    }

    while (deferred_head != NULL) {
        ALEventListItem *next = (ALEventListItem *)deferred_head->node.next;
        u32 ticks = (u32)(deferred_head->delta / old_uspt);

        deferred_head->delta = ticks * seqp->uspt;
        native_seqp_repost_event_item(&seqp->evtq, deferred_head);
        deferred_head = next;
    }
}

#ifdef NATIVE_PORT
static FILE *s_native_csp_trace_fp = NULL;
static int s_native_csp_trace_init = 0;

static FILE *native_csp_trace_fp(void)
{
    const char *path;

    if (s_native_csp_trace_init) {
        return s_native_csp_trace_fp;
    }

    s_native_csp_trace_init = 1;
    path = getenv("MDKR_MUSIC_MIDI_TRACE_JSONL");
    if (path != NULL && *path != '\0') {
        s_native_csp_trace_fp = fopen(path, "w");
    }

    return s_native_csp_trace_fp;
}

static s32 native_csp_program_for_instrument(ALCSPlayer *seqp,
                                             ALInstrument *instrument)
{
    s32 i;

    if (seqp == NULL || seqp->bank == NULL || instrument == NULL) {
        return -1;
    }

    for (i = 0; i < seqp->bank->instCount; i++) {
        if (seqp->bank->instArray[i] == instrument) {
            return i;
        }
    }

    return -1;
}

static int native_csp_program_list_contains(const char *list, s32 program)
{
    const char *cursor = list;

    if (program < 0 || list == NULL || *list == '\0') {
        return 0;
    }

    while (*cursor != '\0') {
        char *end = NULL;
        long value;

        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') {
            cursor++;
        }

        if (*cursor == '\0') {
            break;
        }

        value = strtol(cursor, &end, 10);
        if (end == cursor) {
            while (*cursor != '\0' && *cursor != ',') {
                cursor++;
            }
            continue;
        }

        if (value == program) {
            return 1;
        }

        cursor = end;
    }

    return 0;
}

static int native_csp_program_should_play(ALCSPlayer *seqp, u8 chan)
{
    static int initialized = 0;
    static const char *solo_programs = NULL;
    static const char *mute_programs = NULL;
    ALInstrument *instrument;
    s32 program;

    if (!initialized) {
        initialized = 1;
        solo_programs = getenv("MDKR_MUSIC_SOLO_PROGRAMS");
        mute_programs = getenv("MDKR_MUSIC_MUTE_PROGRAMS");
    }

    if ((solo_programs == NULL || *solo_programs == '\0') &&
        (mute_programs == NULL || *mute_programs == '\0')) {
        return 1;
    }

    if (seqp == NULL || CSP_CHAN(seqp) == NULL || chan >= seqp->maxChannels) {
        return 1;
    }

    instrument = CSP_CHAN(seqp)[chan].instrument;
    program = native_csp_program_for_instrument(seqp, instrument);

    if (solo_programs != NULL && *solo_programs != '\0' &&
        !native_csp_program_list_contains(solo_programs, program)) {
        return 0;
    }

    if (native_csp_program_list_contains(mute_programs, program)) {
        return 0;
    }

    return 1;
}

static s32 native_csp_sound_index(ALInstrument *instrument, ALSound *sound)
{
    s32 i;

    if (instrument == NULL || sound == NULL) {
        return -1;
    }

    for (i = 0; i < instrument->soundCount; i++) {
        if (instrument->soundArray[i] == sound) {
            return i;
        }
    }

    return -1;
}

static void native_csp_trace_control(ALCSPlayer *seqp, const char *event,
                                     u8 chan, u8 key, u8 value)
{
    FILE *fp = native_csp_trace_fp();

    if (fp == NULL || seqp == NULL) {
        return;
    }

    fprintf(fp,
            "{\"event\":\"%s\",\"cur_time\":%d,\"seq_ticks\":%u,"
            "\"chan\":%u,\"key\":%u,\"value\":%u}\n",
            event,
            seqp->curTime,
            seqp->target != NULL ? seqp->target->lastTicks : 0,
            (u32)chan,
            (u32)key,
            (u32)value);
}

static void native_csp_trace_note_on(ALCSPlayer *seqp, u8 chan, u8 key,
                                     u8 vel, ALSound *sound, f32 pitch,
                                     s16 vol, ALPan pan, u8 fxmix,
                                     ALMicroTime delta_time,
                                     u32 duration_ticks)
{
    ALInstrument *instrument = NULL;
    ALKeyMap *keymap = NULL;
    ALEnvelope *envelope = NULL;
    ALWaveTable *wave = NULL;
    ALADPCMloop *adpcm_loop = NULL;
    ALRawLoop *raw_loop = NULL;
    FILE *fp = native_csp_trace_fp();

    if (fp == NULL || seqp == NULL || sound == NULL) {
        return;
    }

    if (CSP_CHAN(seqp) != NULL && chan < seqp->maxChannels) {
        instrument = CSP_CHAN(seqp)[chan].instrument;
    }
    keymap = sound->keyMap;
    envelope = sound->envelope;
    wave = sound->wavetable;
    if (wave != NULL && wave->type == AL_ADPCM_WAVE) {
        adpcm_loop = wave->waveInfo.adpcmWave.loop;
    } else if (wave != NULL && wave->type == AL_RAW16_WAVE) {
        raw_loop = wave->waveInfo.rawWave.loop;
    }

    fprintf(fp,
            "{\"event\":\"note_on\",\"cur_time\":%d,\"seq_ticks\":%u,"
            "\"chan\":%u,\"key\":%u,\"velocity\":%u,\"duration_ticks\":%u,"
            "\"program\":%d,\"sound_index\":%d,"
            "\"chan_volume\":%u,\"chan_pan\":%u,\"chan_fxmix\":%u,"
            "\"computed_pitch\":%.9g,\"computed_volume\":%d,"
            "\"computed_pan\":%u,\"attack_delta_usec\":%d,"
            "\"sample_pan\":%u,\"sample_volume\":%u,"
            "\"key_min\":%u,\"key_max\":%u,\"vel_min\":%u,\"vel_max\":%u,"
            "\"key_base\":%u,\"detune\":%d,"
            "\"attack_time\":%d,\"decay_time\":%d,\"release_time\":%d,"
            "\"attack_volume\":%u,\"decay_volume\":%u,"
            "\"wave_type\":%u,\"wave_base\":%llu,\"wave_len\":%d,"
            "\"loop_start\":%d,\"loop_end\":%d,\"loop_count\":%d}\n",
            seqp->curTime,
            seqp->target != NULL ? seqp->target->lastTicks : 0,
            (u32)chan,
            (u32)key,
            (u32)vel,
            duration_ticks,
            native_csp_program_for_instrument(seqp, instrument),
            native_csp_sound_index(instrument, sound),
            (CSP_CHAN(seqp) != NULL && chan < seqp->maxChannels)
                ? CSP_CHAN(seqp)[chan].vol
                : 0,
            (CSP_CHAN(seqp) != NULL && chan < seqp->maxChannels)
                ? CSP_CHAN(seqp)[chan].pan
                : 0,
            fxmix,
            pitch,
            vol,
            (u32)pan,
            delta_time,
            sound->samplePan,
            sound->sampleVolume,
            keymap != NULL ? keymap->keyMin : 0,
            keymap != NULL ? keymap->keyMax : 0,
            keymap != NULL ? keymap->velocityMin : 0,
            keymap != NULL ? keymap->velocityMax : 0,
            keymap != NULL ? keymap->keyBase : 0,
            keymap != NULL ? keymap->detune : 0,
            envelope != NULL ? envelope->attackTime : 0,
            envelope != NULL ? envelope->decayTime : 0,
            envelope != NULL ? envelope->releaseTime : 0,
            envelope != NULL ? envelope->attackVolume : 0,
            envelope != NULL ? envelope->decayVolume : 0,
            wave != NULL ? wave->type : 0,
            wave != NULL ? (unsigned long long)(uintptr_t)wave->base : 0ULL,
            wave != NULL ? wave->len : 0,
            adpcm_loop != NULL ? adpcm_loop->start
                : raw_loop != NULL ? raw_loop->start : 0,
            adpcm_loop != NULL ? adpcm_loop->end
                : raw_loop != NULL ? raw_loop->end : 0,
            adpcm_loop != NULL ? adpcm_loop->count
                : raw_loop != NULL ? raw_loop->count : 0);
}

static void native_csp_trace_note_off(ALCSPlayer *seqp, u8 chan, u8 key)
{
    native_csp_trace_control(seqp, "note_off", chan, key, 0);
}
#else
#define native_csp_trace_control(seqp, event, chan, key, value) ((void)0)
#define native_csp_trace_note_on(seqp, chan, key, vel, sound, pitch, vol, pan, fxmix, delta_time, duration_ticks) ((void)0)
#define native_csp_trace_note_off(seqp, chan, key) ((void)0)
#define native_csp_program_should_play(seqp, chan) (1)
#endif

static void native_csp_handle_midi(ALCSPlayer *seqp, ALEvent *event)
{
    ALMIDIEvent *midi;
    s32 status;
    u8 chan;
    u8 key;
    u8 vel;

    if (seqp == NULL || event == NULL) {
        return;
    }

    midi = &event->msg.midi;
    status = midi->status & AL_MIDI_StatusMask;
    chan = midi->status & AL_MIDI_ChannelMask;
    key = midi->byte1;
    vel = midi->byte2;
    if (chan >= seqp->maxChannels || CSP_CHAN(seqp) == NULL) {
        return;
    }

    /*
     * DKR channel-mode messages. These share the Control Change status byte
     * (AL_MIDI_ChannelModeSelect == AL_MIDI_ControlChange == 0xB0) and are
     * distinguished by controller number. Note both act on the channel named
     * by byte2 (`vel`), NOT the status byte's channel nibble.
     */
    if (status == AL_MIDI_ControlChange &&
        (key == AL_MIDI_UNK_6A || key == AL_MIDI_UNK_6C) &&
        vel < seqp->maxChannels) {
        if (key == AL_MIDI_UNK_6C) {
            seqp->chanMask |= (u16)(1 << vel);
            return;
        }

        seqp->chanMask &= (u16)~(1 << vel);
        {
            ALVoiceState *voice_state;

            for (voice_state = seqp->vAllocHead; voice_state != NULL;
                 voice_state = voice_state->next) {
                if (voice_state->channel == vel &&
                    voice_state->sound != NULL &&
                    voice_state->sound->envelope != NULL) {
                    __seqpReleaseVoice(
                        (ALSeqPlayer *)seqp, &voice_state->voice,
                        voice_state->sound->envelope->releaseTime);
                }
            }
        }
        return;
    }

    /*
     * Disabled channels still track state: program change, control change and
     * pitch bend are applied so the channel is correct when it is re-enabled,
     * but nothing that could sound a note gets through.
     */
    if ((seqp->chanMask & (1 << chan)) == 0 &&
        status != AL_MIDI_ProgramChange &&
        status != AL_MIDI_ControlChange &&
        status != AL_MIDI_PitchBendChange) {
        return;
    }

    switch (status) {
        case AL_MIDI_NoteOn:
            if (vel != 0) {
                ALSound *sound;
                ALVoiceConfig config;
                ALVoiceState *voice_state;
                ALVoice *voice;
                ALEvent queued;
                ALMicroTime delta_time;
                s32 cents;
                f32 pitch;
                s16 vol;
                ALPan pan;

                if (seqp->state != AL_PLAYING) {
                    return;
                }

                sound =
                    __lookupSoundQuick((ALSeqPlayer *)seqp, key, vel, chan);
                if (sound == NULL || sound->keyMap == NULL ||
                    sound->envelope == NULL || sound->wavetable == NULL ||
                    CSP_CHAN(seqp)[chan].instrument == NULL) {
                    return;
                }

                if (!native_csp_program_should_play(seqp, chan)) {
                    return;
                }

                voice_state =
                    __mapVoice((ALSeqPlayer *)seqp, key, vel, chan);
                if (voice_state == NULL) {
                    return;
                }

                config.priority = CSP_CHAN(seqp)[chan].priority;
                config.fxBus = 0;
                config.unityPitch = 0;
                voice = &voice_state->voice;
                if (!alSynAllocVoice(seqp->drvr, voice, &config)) {
                    __unmapVoice((ALSeqPlayer *)seqp, voice);
                    return;
                }

                voice_state->sound = sound;
                voice_state->envPhase = AL_PHASE_ATTACK;
                voice_state->phase =
                    CSP_CHAN(seqp)[chan].sustain > AL_SUSTAIN
                        ? AL_PHASE_SUSTAIN
                        : AL_PHASE_NOTEON;
                cents = ((s32)key - sound->keyMap->keyBase) * 100 +
                        sound->keyMap->detune;
                voice_state->pitch = alCents2Ratio(cents);
                voice_state->envGain = sound->envelope->attackVolume;
                voice_state->envEndTime =
                    seqp->curTime + sound->envelope->attackTime;
                voice_state->flags = 0;
                voice_state->tremelo = AL_VOL_FULL;
                voice_state->vibrato = 1.0f;

                pitch = voice_state->pitch *
                        CSP_CHAN(seqp)[chan].pitchBend *
                        voice_state->vibrato;
                pan = __vsPan(voice_state, (ALSeqPlayer *)seqp);
                vol = __vsVol(voice_state, (ALSeqPlayer *)seqp);
                delta_time = sound->envelope->attackTime;
                native_csp_trace_note_on(seqp,
                                         chan,
                                         key,
                                         vel,
                                         sound,
                                         pitch,
                                         vol,
                                         pan,
                                         CSP_CHAN(seqp)[chan].fxmix,
                                         delta_time,
                                         midi->duration);
                alSynStartVoiceParams(seqp->drvr, voice, sound->wavetable,
                                      pitch, vol, pan,
                                      CSP_CHAN(seqp)[chan].fxmix,
                                      delta_time);

                queued.type = AL_SEQP_ENV_EVT;
                queued.msg.vol.voice = voice;
                queued.msg.vol.vol = sound->envelope->decayVolume;
                queued.msg.vol.delta = sound->envelope->decayTime;
                alEvtqPostEvent(&seqp->evtq, &queued, delta_time);

                if (midi->duration != 0) {
                    queued.type = AL_CSP_NOTEOFF_EVT;
                    queued.msg.midi.status = chan | AL_MIDI_NoteOff;
                    queued.msg.midi.byte1 = key;
                    queued.msg.midi.byte2 = 0;
                    queued.msg.midi.duration = 0;
                    alEvtqPostEvent(&seqp->evtq, &queued,
                                    seqp->uspt * midi->duration);
                }
                return;
            }
            /* Treat zero-velocity note-on as note-off. */
            /* FALLTHROUGH */
        case AL_MIDI_NoteOff:
        {
            ALVoiceState *voice_state =
                __lookupVoice((ALSeqPlayer *)seqp, key, chan);

            native_csp_trace_note_off(seqp, chan, key);
            if (voice_state == NULL || voice_state->sound == NULL ||
                voice_state->sound->envelope == NULL) {
                return;
            }

            if (voice_state->phase == AL_PHASE_SUSTAIN) {
                voice_state->phase = AL_PHASE_SUSTREL;
            } else {
                voice_state->phase = AL_PHASE_RELEASE;
                __seqpReleaseVoice((ALSeqPlayer *)seqp, &voice_state->voice,
                                   voice_state->sound->envelope->releaseTime);
            }
            break;
        }

        case AL_MIDI_PolyKeyPressure:
        {
            ALVoiceState *voice_state =
                __lookupVoice((ALSeqPlayer *)seqp, key, chan);

            if (voice_state == NULL) {
                return;
            }

            voice_state->velocity = vel;
            alSynSetVol(seqp->drvr,
                        &voice_state->voice,
                        __vsVol(voice_state, (ALSeqPlayer *)seqp),
                        __vsDelta(voice_state, seqp->curTime));
            break;
        }

        case AL_MIDI_ChannelPressure:
        {
            ALVoiceState *voice_state;

            for (voice_state = seqp->vAllocHead; voice_state != NULL;
                 voice_state = voice_state->next) {
                if (voice_state->channel == chan) {
                    voice_state->velocity = key;
                    alSynSetVol(seqp->drvr,
                                &voice_state->voice,
                                __vsVol(voice_state, (ALSeqPlayer *)seqp),
                                __vsDelta(voice_state, seqp->curTime));
                }
            }
            break;
        }

        case AL_MIDI_ControlChange:
            switch (key) {
                case AL_MIDI_PAN_CTRL:
                {
                    ALVoiceState *voice_state;

                    native_csp_trace_control(seqp, "control_change", chan,
                                             key, vel);
                    CSP_CHAN(seqp)[chan].pan = vel;
                    for (voice_state = seqp->vAllocHead; voice_state != NULL;
                         voice_state = voice_state->next) {
                        if (voice_state->channel == chan) {
                            alSynSetPan(
                                seqp->drvr,
                                &voice_state->voice,
                                __vsPan(voice_state, (ALSeqPlayer *)seqp));
                        }
                    }
                    break;
                }

                /*
                 * DKR splits channel volume into two lanes that are multiplied
                 * together: `unk11` is the raw CC7 volume and `fade` is the
                 * CC8 fade-in level, both 0..127. The effective channel volume
                 * driving the voices is (unk11 * fade) / 127, so a fade can
                 * duck a channel without destroying the volume the sequence
                 * set, and vice versa.
                 */
                case AL_MIDI_VOLUME_CTRL:
                    native_csp_trace_control(seqp, "control_change", chan,
                                             key, vel);
                    CSP_CHAN(seqp)[chan].unk11 = vel;
                    CSP_CHAN(seqp)[chan].vol =
                        (u8)(((u32)CSP_CHAN(seqp)[chan].fade * (u32)vel) / 127u);
                    native_csp_update_channel_volumes(seqp, chan);
                    break;

                case AL_MIDI_UNK_8:
                    native_csp_trace_control(seqp, "control_change", chan,
                                             key, vel);
                    CSP_CHAN(seqp)[chan].fade = vel;
                    CSP_CHAN(seqp)[chan].vol =
                        (u8)(((u32)CSP_CHAN(seqp)[chan].unk11 * (u32)vel) / 127u);
                    native_csp_update_channel_volumes(seqp, chan);
                    break;

                case AL_MIDI_PRIORITY_CTRL:
                    native_csp_trace_control(seqp, "control_change", chan,
                                             key, vel);
                    CSP_CHAN(seqp)[chan].priority = vel;
                    break;

                case AL_MIDI_SUSTAIN_CTRL:
                {
                    ALVoiceState *voice_state;

                    native_csp_trace_control(seqp, "control_change", chan,
                                             key, vel);
                    CSP_CHAN(seqp)[chan].sustain = vel;
                    for (voice_state = seqp->vAllocHead; voice_state != NULL;
                         voice_state = voice_state->next) {
                        if (voice_state->channel != chan ||
                            voice_state->phase == AL_PHASE_RELEASE) {
                            continue;
                        }
                        if (vel > AL_SUSTAIN) {
                            if (voice_state->phase == AL_PHASE_NOTEON) {
                                voice_state->phase = AL_PHASE_SUSTAIN;
                            }
                        } else if (voice_state->phase == AL_PHASE_SUSTAIN) {
                            voice_state->phase = AL_PHASE_NOTEON;
                        } else if (voice_state->phase == AL_PHASE_SUSTREL &&
                                   voice_state->sound != NULL &&
                                   voice_state->sound->envelope != NULL) {
                            voice_state->phase = AL_PHASE_RELEASE;
                            __seqpReleaseVoice(
                                (ALSeqPlayer *)seqp,
                                &voice_state->voice,
                                voice_state->sound->envelope->releaseTime);
                        }
                    }
                    break;
                }

                case AL_MIDI_FX1_CTRL:
                {
                    ALVoiceState *voice_state;

                    native_csp_trace_control(seqp, "control_change", chan,
                                             key, vel);
                    CSP_CHAN(seqp)[chan].fxmix = vel;
                    for (voice_state = seqp->vAllocHead; voice_state != NULL;
                         voice_state = voice_state->next) {
                        if (voice_state->channel == chan) {
                            alSynSetFXMix(seqp->drvr, &voice_state->voice,
                                          vel);
                        }
                    }
                    break;
                }

                /*
                 * DKR global FX-send floor. Unlike every other controller this
                 * one ignores its own channel nibble and sweeps ALL channels,
                 * clearing the stored fxmix of any channel currently sending
                 * less than `vel`.
                 *
                 * Faithful-to-original oddity: the stored fxmix is set to 0
                 * but the live voices are driven to `vel`, so stored and
                 * audible state deliberately diverge until something next
                 * touches that channel's fxmix. func_80063A90() is the only
                 * caller and it passes a CHANNEL INDEX as `vel`, so in
                 * practice the comparison runs against small values. This is
                 * reproduced as-is because the music mix depends on it.
                 */
                case AL_MIDI_UNK_5F:
                {
                    u8 i;

                    native_csp_trace_control(seqp, "control_change", chan,
                                             key, vel);
                    seqp->unk36 = vel;
                    for (i = 0; i < seqp->maxChannels; i++) {
                        ALVoiceState *voice_state;

                        if (CSP_CHAN(seqp)[i].fxmix >= vel) {
                            continue;
                        }
                        CSP_CHAN(seqp)[i].fxmix = 0;
                        for (voice_state = seqp->vAllocHead;
                             voice_state != NULL;
                             voice_state = voice_state->next) {
                            if (voice_state->channel == i) {
                                alSynSetFXMix(seqp->drvr, &voice_state->voice,
                                              vel);
                            }
                        }
                    }
                    break;
                }

                default:
                    native_csp_trace_control(seqp, "control_change_unhandled",
                                             chan, key, vel);
                    break;
            }
            break;

        case AL_MIDI_ProgramChange:
            native_csp_trace_control(seqp, "program_change", chan, key, 0);
            if (seqp->bank != NULL && key < seqp->bank->instCount &&
                seqp->bank->instArray[key] != NULL) {
                __setInstChanState((ALSeqPlayer *)seqp,
                                   seqp->bank->instArray[key],
                                   chan);
            }
            break;

        case AL_MIDI_PitchBendChange:
        {
            ALVoiceState *voice_state;
            s32 bend = (((s32)vel << 7) + key) - 8192;
            s32 cents = (CSP_CHAN(seqp)[chan].bendRange * bend) / 8192;
            f32 ratio = alCents2Ratio(cents);

            CSP_CHAN(seqp)[chan].pitchBend = ratio;
            native_csp_trace_control(seqp, "pitch_bend", chan, key, vel);
            for (voice_state = seqp->vAllocHead; voice_state != NULL;
                 voice_state = voice_state->next) {
                if (voice_state->channel == chan) {
                    alSynSetPitch(
                        seqp->drvr,
                        &voice_state->voice,
                        voice_state->pitch * ratio * voice_state->vibrato);
                }
            }
            break;
        }

        default:
            break;
    }
}

static void native_csp_handle_next_seq_event(ALCSPlayer *seqp)
{
    ALEvent event;

    if (seqp == NULL || seqp->target == NULL) {
        return;
    }

    alCSeqNextEvent(seqp->target, &event);
    switch (event.type) {
        case AL_SEQ_MIDI_EVT:
            native_csp_handle_midi(seqp, &event);
            __CSPPostNextSeqEvent(seqp);
            break;

        case AL_TEMPO_EVT:
            native_csp_handle_meta(seqp, &event);
            __CSPPostNextSeqEvent(seqp);
            break;

        case AL_SEQ_END_EVT:
            seqp->state = AL_STOPPING;
            event.type = AL_SEQP_STOP_EVT;
            alEvtqPostEvent(&seqp->evtq, &event, AL_EVTQ_END);
            break;

        case AL_TRACK_END:
        case AL_CSP_LOOPSTART:
        case AL_CSP_LOOPEND:
            __CSPPostNextSeqEvent(seqp);
            break;

        default:
            break;
    }
}

static void native_csp_stop_allocated_voices(ALCSPlayer *seqp)
{
    ALVoiceState *voice_state;

    if (seqp == NULL) {
        return;
    }

    while ((voice_state = seqp->vAllocHead) != NULL) {
        alSynStopVoice(seqp->drvr, &voice_state->voice);
        alSynFreeVoice(seqp->drvr, &voice_state->voice);
        if (voice_state->flags != 0) {
            __seqpStopOsc((ALSeqPlayer *)seqp, voice_state);
        }
        __unmapVoice((ALSeqPlayer *)seqp, &voice_state->voice);
    }
}

static ALMicroTime native_csp_voice_handler(void *node)
{
    ALCSPlayer *seqp = (ALCSPlayer *)node;
    ALEvent event;

    if (seqp == NULL) {
        return AL_USEC_PER_FRAME;
    }

    do {
        switch (seqp->nextEvent.type) {
            case AL_SEQ_REF_EVT:
                native_csp_handle_next_seq_event(seqp);
                break;

            case AL_SEQP_API_EVT:
                event.type = AL_SEQP_API_EVT;
                alEvtqPostEvent(&seqp->evtq, &event, seqp->frameTime);
                break;

            case AL_NOTE_END_EVT:
                if (seqp->nextEvent.msg.note.voice != NULL) {
                    ALVoice *voice = seqp->nextEvent.msg.note.voice;
                    ALVoiceState *voice_state =
                        (ALVoiceState *)voice->clientPrivate;

                    alSynStopVoice(seqp->drvr, voice);
                    alSynFreeVoice(seqp->drvr, voice);
                    if (voice_state != NULL && voice_state->flags != 0) {
                        __seqpStopOsc((ALSeqPlayer *)seqp, voice_state);
                    }
                    __unmapVoice((ALSeqPlayer *)seqp, voice);
                }
                break;

            case AL_SEQP_ENV_EVT:
                if (seqp->nextEvent.msg.vol.voice != NULL) {
                    ALVoice *voice = seqp->nextEvent.msg.vol.voice;
                    ALVoiceState *voice_state =
                        (ALVoiceState *)voice->clientPrivate;
                    ALMicroTime delta = seqp->nextEvent.msg.vol.delta;

                    if (voice_state != NULL) {
                        if (voice_state->envPhase == AL_PHASE_ATTACK) {
                            voice_state->envPhase = AL_PHASE_DECAY;
                        }
                        voice_state->envEndTime = seqp->curTime + delta;
                        voice_state->envGain = seqp->nextEvent.msg.vol.vol;
                        alSynSetVol(
                            seqp->drvr,
                            voice,
                            __vsVol(voice_state, (ALSeqPlayer *)seqp),
                            delta);
                    }
                }
                break;

            case AL_TREM_OSC_EVT:
            case AL_VIB_OSC_EVT:
                break;

            case AL_SEQP_MIDI_EVT:
            case AL_CSP_NOTEOFF_EVT:
                native_csp_handle_midi(seqp, &seqp->nextEvent);
                break;

            case AL_SEQP_META_EVT:
                native_csp_handle_meta(seqp, &seqp->nextEvent);
                break;

            case AL_SEQP_VOL_EVT:
                seqp->vol = seqp->nextEvent.msg.spvol.vol;
                native_csp_update_all_volumes(seqp);
                break;

            case AL_SEQP_PLAY_EVT:
                if (seqp->state != AL_PLAYING) {
                    seqp->state = AL_PLAYING;
                    __CSPPostNextSeqEvent(seqp);
                }
                break;

            case AL_SEQP_STOP_EVT:
                if (seqp->state == AL_STOPPING) {
                    native_csp_stop_allocated_voices(seqp);
                    seqp->state = AL_STOPPED;
                }
                break;

            case AL_SEQP_STOPPING_EVT:
                if (seqp->state == AL_PLAYING) {
                    ALVoiceState *voice_state;

                    alEvtqFlushType(&seqp->evtq, AL_SEQ_REF_EVT);
                    alEvtqFlushType(&seqp->evtq, AL_CSP_NOTEOFF_EVT);
                    alEvtqFlushType(&seqp->evtq, AL_SEQP_MIDI_EVT);
                    for (voice_state = seqp->vAllocHead;
                         voice_state != NULL;
                         voice_state = voice_state->next) {
                        if (__voiceNeedsNoteKill(
                                (ALSeqPlayer *)seqp,
                                &voice_state->voice,
                                KILL_TIME)) {
                            __seqpReleaseVoice((ALSeqPlayer *)seqp,
                                               &voice_state->voice,
                                               KILL_TIME);
                        }
                    }
                    seqp->state = AL_STOPPING;
                    event.type = AL_SEQP_STOP_EVT;
                    alEvtqPostEvent(&seqp->evtq, &event, AL_EVTQ_END);
                }
                break;

            case AL_SEQP_PRIORITY_EVT:
                if (seqp->nextEvent.msg.sppriority.chan < seqp->maxChannels) {
                    CSP_CHAN(seqp)[seqp->nextEvent.msg.sppriority.chan]
                        .priority =
                        seqp->nextEvent.msg.sppriority.priority;
                }
                break;

            case AL_SEQP_SEQ_EVT:
                seqp->target = (ALCSeq *)seqp->nextEvent.msg.spseq.seq;
                native_csp_set_uspt_from_tempo(seqp, 500000.0f);
                if (seqp->bank != NULL) {
                    __initFromBank((ALSeqPlayer *)seqp, seqp->bank);
                }
                break;

            case AL_SEQP_BANK_EVT:
                seqp->bank = seqp->nextEvent.msg.spbank.bank;
                __initFromBank((ALSeqPlayer *)seqp, seqp->bank);
                break;

            default:
                break;
        }

        seqp->nextDelta = portAudioEventQueueNextOrHeartbeat(
            &seqp->evtq,
            &seqp->nextEvent,
            AL_SEQP_API_EVT,
            seqp->frameTime);
    } while (seqp->nextDelta == 0);

    seqp->curTime += seqp->nextDelta;
    return seqp->nextDelta;
}

void alCSPNew(ALCSPlayer *seqp, ALSeqpConfig *config)
{
    ALHeap *heap;
    ALEventListItem *events;
    ALVoiceState *voices;
    s32 i;

    if (seqp == NULL || config == NULL || alGlobals == NULL) {
        return;
    }

    memset(seqp, 0, sizeof(*seqp));
    heap = config->heap;
    seqp->drvr = &alGlobals->drvr;
    seqp->chanMask = 0xFFFF;
    seqp->uspt = 488;
    seqp->state = AL_STOPPED;
    seqp->vol = 0x7fff;
    seqp->frameTime = AL_USEC_PER_FRAME;
    seqp->maxChannels = config->maxChannels;
    seqp->debugFlags = config->debugFlags;
    seqp->initOsc = (ALOscInit)config->initOsc;
    seqp->updateOsc = (ALOscUpdate)config->updateOsc;
    seqp->stopOsc = (ALOscStop)config->stopOsc;
    seqp->nextEvent.type = AL_SEQP_API_EVT;

    if (seqp->maxChannels == 0 || seqp->maxChannels > AL_MAX_CHANNELS) {
        seqp->maxChannels = AL_MAX_CHANNELS;
    }

    CSP_CHAN(seqp) =
        alHeapAlloc(heap, seqp->maxChannels, sizeof(ALChanState_Custom));
    __initChanState((ALSeqPlayer *)seqp);

    voices = alHeapAlloc(heap, config->maxVoices, sizeof(*voices));
    for (i = 0; voices != NULL && i < config->maxVoices; i++) {
        voices[i].next = seqp->vFreeList;
        seqp->vFreeList = &voices[i];
    }

    events = alHeapAlloc(heap, config->maxEvents, sizeof(*events));
    alEvtqNew(&seqp->evtq, events, config->maxEvents);

    seqp->node.next = NULL;
    seqp->node.handler = native_csp_voice_handler;
    seqp->node.clientData = seqp;
    alSynAddPlayer(&alGlobals->drvr, &seqp->node);
}

s32 alCSPGetState(ALCSPlayer *seqp)
{
    if (seqp == NULL) {
        return AL_STOPPED;
    }

    return seqp->state;
}

void alFilterNew(ALFilter *filter, ALCmdHandler handler, ALSetParam set_param,
                 s32 type)
{
    if (filter == NULL) {
        return;
    }

    filter->source = NULL;
    filter->handler = handler;
    filter->setParam = set_param;
    filter->inp = 0;
    filter->outp = 0;
    filter->type = type;
}

#define NATIVE_ADPCM_FRAME_BYTES 9
#define NATIVE_ADPCM_FRAME_SHIFT 4

#ifdef NATIVE_PORT
static FILE *s_native_audio_filter_trace_fp = NULL;
static int s_native_audio_filter_trace_init = 0;
static uintptr_t s_native_audio_filter_trace_wave_base = 0;
static int s_native_audio_filter_trace_has_wave_base = 0;

static FILE *native_audio_filter_trace_fp(void)
{
    const char *path;
    const char *wave_base;

    if (s_native_audio_filter_trace_init) {
        return s_native_audio_filter_trace_fp;
    }

    s_native_audio_filter_trace_init = 1;
    path = getenv("MDKR_AUDIO_FILTER_TRACE_JSONL");
    if (path != NULL && *path != '\0') {
        s_native_audio_filter_trace_fp = fopen(path, "w");
    }

    wave_base = getenv("MDKR_AUDIO_FILTER_TRACE_WAVE_BASE");
    if (wave_base != NULL && *wave_base != '\0') {
        s_native_audio_filter_trace_wave_base =
            (uintptr_t)strtoull(wave_base, NULL, 0);
        s_native_audio_filter_trace_has_wave_base = 1;
    }

    return s_native_audio_filter_trace_fp;
}

static int native_audio_filter_trace_matches(ALWaveTable *table)
{
    if (native_audio_filter_trace_fp() == NULL || table == NULL) {
        return 0;
    }

    if (!s_native_audio_filter_trace_has_wave_base) {
        return 1;
    }

    return (uintptr_t)table->base == s_native_audio_filter_trace_wave_base;
}

static void native_audio_filter_trace_adpcm(ALLoadFilter *load,
                                            s32 out_count,
                                            s32 requested_samples,
                                            s32 samples_to_decode,
                                            s32 frame_count,
                                            s32 byte_count,
                                            s32 overflow,
                                            s32 overflow_samples,
                                            s32 samples_left_from_frame,
                                            s32 looped)
{
    FILE *fp;
    ALWaveTable *table;
    ALADPCMBook *book = NULL;

    if (load == NULL) {
        return;
    }

    table = load->table;
    if (!native_audio_filter_trace_matches(table)) {
        return;
    }

    fp = native_audio_filter_trace_fp();
    if (fp == NULL) {
        return;
    }

    if (table != NULL && table->type == AL_ADPCM_WAVE) {
        book = table->waveInfo.adpcmWave.book;
    }

    fprintf(fp,
            "{\"event\":\"adpcm_pull\",\"wave_base\":%llu,"
            "\"wave_len\":%d,\"out_count\":%d,\"requested_samples\":%d,"
            "\"samples_to_decode\":%d,\"frame_count\":%d,"
            "\"byte_count\":%d,\"overflow\":%d,"
            "\"overflow_samples\":%d,\"samples_left_from_frame\":%d,"
            "\"load_sample\":%d,\"lastsam\":%d,\"first\":%d,"
            "\"looped\":%d,\"loop_start\":%d,\"loop_end\":%d,"
            "\"loop_count\":%d,\"memin\":%llu,"
            "\"book_order\":%d,\"book_np\":%d,\"book_size\":%d}\n",
            table != NULL ? (unsigned long long)(uintptr_t)table->base : 0ULL,
            table != NULL ? table->len : 0,
            out_count,
            requested_samples,
            samples_to_decode,
            frame_count,
            byte_count,
            overflow,
            overflow_samples,
            samples_left_from_frame,
            load->sample,
            load->lastsam,
            load->first,
            looped,
            load->loop.start,
            load->loop.end,
            load->loop.count,
            (unsigned long long)(uintptr_t)load->memin,
            book != NULL ? book->order : 0,
            book != NULL ? book->npredictors : 0,
            load->bookSize);
}

static void native_audio_filter_trace_resample(ALResampler *resampler,
                                               ALLoadFilter *load,
                                               s32 out_count,
                                               s32 input_count,
                                               s32 increment,
                                               f32 float_input_count,
                                               f32 previous_delta)
{
    FILE *fp;
    ALWaveTable *table = NULL;

    if (load != NULL) {
        table = load->table;
    }
    if (!native_audio_filter_trace_matches(table)) {
        return;
    }

    fp = native_audio_filter_trace_fp();
    if (fp == NULL) {
        return;
    }

    fprintf(fp,
            "{\"event\":\"resample_pull\",\"wave_base\":%llu,"
            "\"wave_len\":%d,\"out_count\":%d,\"input_count\":%d,"
            "\"ratio\":%.9g,\"increment\":%d,\"first\":%d,"
            "\"upitch\":%d,\"previous_delta\":%.9g,"
            "\"float_input_count\":%.9g,\"next_delta\":%.9g,"
            "\"load_sample\":%d,\"lastsam\":%d,\"memin\":%llu}\n",
            table != NULL ? (unsigned long long)(uintptr_t)table->base : 0ULL,
            table != NULL ? table->len : 0,
            out_count,
            input_count,
            resampler != NULL ? resampler->ratio : 0.0f,
            increment,
            resampler != NULL ? resampler->first : 0,
            resampler != NULL ? resampler->upitch : 0,
            previous_delta,
            float_input_count,
            resampler != NULL ? resampler->delta : 0.0f,
            load != NULL ? load->sample : 0,
            load != NULL ? load->lastsam : 0,
            load != NULL ? (unsigned long long)(uintptr_t)load->memin : 0ULL);
}
#else
#define native_audio_filter_trace_adpcm(load, out_count, requested_samples, samples_to_decode, frame_count, byte_count, overflow, overflow_samples, samples_left_from_frame, looped) ((void)0)
#define native_audio_filter_trace_resample(resampler, load, out_count, input_count, increment, float_input_count, previous_delta) ((void)0)
#endif

static s32 min_s32(s32 a, s32 b)
{
    return a < b ? a : b;
}

static s32 load_buffer_dma_count(s32 byte_count)
{
    return byte_count + 8 - (byte_count & 7);
}

static Acmd *_decodeChunk(Acmd *cmd, ALLoadFilter *load, s32 samples,
                                s32 byte_count, s16 output, s16 input,
                                u32 flags)
{
    Acmd *ptr = cmd;
    intptr_t dram_location;
    intptr_t dram_align;

    if (byte_count > 0) {
        dram_location = load->dma(load->memin, byte_count, load->dmaState);
        dram_align = dram_location & 7;
        byte_count += (s32)dram_align;
        aSetBuffer(ptr++, 0, input, 0, load_buffer_dma_count(byte_count));
        aLoadBuffer(ptr++, (void *)(uintptr_t)(dram_location - dram_align));
    } else {
        dram_align = 0;
    }

    if (flags & A_LOOP) {
        aSetLoop(ptr++, osVirtualToPhysical(load->lstate));
    }

    aSetBuffer(ptr++, 0, input + (s16)dram_align, output, samples << 1);
    aADPCMdec(ptr++, flags, osVirtualToPhysical(load->state));
    load->first = 0;
    return ptr;
}

Acmd *alAdpcmPull(void *filter, s16 *outp, s32 out_count,
                  s32 sample_offset, Acmd *cmd)
{
    ALLoadFilter *load = (ALLoadFilter *)filter;
    Acmd *ptr = cmd;
    ALADPCMBook *book;
    s16 input = AL_DECODER_IN;
    s32 samples_to_decode;
    s32 frame_count;
    s32 byte_count;
    s32 overflow;
    s32 zero_start;
    s32 overflow_samples;
    s32 requested_samples;
    s32 output_pos;
    s32 samples_left_from_frame;
    s32 loop_boundary_output;
    s32 decoded = 0;
    s32 looped;

    (void)sample_offset;

    if (load == NULL || outp == NULL || ptr == NULL || out_count == 0 ||
        load->table == NULL || load->dma == NULL ||
        load->table->type != AL_ADPCM_WAVE ||
        load->table->waveInfo.adpcmWave.book == NULL) {
        return ptr;
    }

    book = load->table->waveInfo.adpcmWave.book;
    aLoadADPCM(ptr++, load->bookSize, osVirtualToPhysical(book->book));

    looped =
        (out_count + load->sample > (s32)load->loop.end) &&
        load->loop.count != 0;
    requested_samples =
        looped ? (s32)load->loop.end - load->sample : out_count;

    samples_left_from_frame =
        load->lastsam != 0 ? ADPCMFSIZE - load->lastsam : 0;
    samples_to_decode = requested_samples - samples_left_from_frame;
    if (samples_to_decode < 0) {
        samples_to_decode = 0;
    }

    frame_count =
        (samples_to_decode + ADPCMFSIZE - 1) >> NATIVE_ADPCM_FRAME_SHIFT;
    byte_count = frame_count * NATIVE_ADPCM_FRAME_BYTES;

    if (looped) {
        ptr = _decodeChunk(ptr, load, samples_to_decode, byte_count,
                                 *outp, input, (u32)load->first);

        if (load->lastsam != 0) {
            *outp += (s16)(load->lastsam << 1);
        } else {
            *outp += ADPCMFSIZE << 1;
        }

        load->lastsam = load->loop.start & 0xf;
        load->memin = (intptr_t)load->table->base +
                      NATIVE_ADPCM_FRAME_BYTES *
                          ((s32)(load->loop.start >> NATIVE_ADPCM_FRAME_SHIFT) +
                           1);
        load->sample = load->loop.start;

        loop_boundary_output = *outp;
        while (out_count > requested_samples) {
            out_count -= requested_samples;
            output_pos =
                (loop_boundary_output +
                 ((frame_count + 1) << (NATIVE_ADPCM_FRAME_SHIFT + 1))) &
                ~0x1f;
            loop_boundary_output += requested_samples << 1;

            if (load->loop.count != -1 && load->loop.count != 0) {
                load->loop.count--;
            }

            requested_samples =
                min_s32(out_count, (s32)load->loop.end - (s32)load->loop.start);
            samples_to_decode =
                requested_samples - ADPCMFSIZE + load->lastsam;
            if (samples_to_decode < 0) {
                samples_to_decode = 0;
            }
            frame_count =
                (samples_to_decode + ADPCMFSIZE - 1) >>
                NATIVE_ADPCM_FRAME_SHIFT;
            byte_count = frame_count * NATIVE_ADPCM_FRAME_BYTES;

            ptr = _decodeChunk(ptr, load, samples_to_decode, byte_count,
                                     (s16)output_pos, input,
                                     (u32)(load->first | A_LOOP));
            aDMEMMove(ptr++, output_pos + (load->lastsam << 1),
                      loop_boundary_output, requested_samples << 1);
        }

        load->lastsam = (out_count + load->lastsam) & 0xf;
        load->sample += out_count;
        load->memin += NATIVE_ADPCM_FRAME_BYTES * frame_count;
        native_audio_filter_trace_adpcm(load, out_count, requested_samples,
                                        samples_to_decode, frame_count,
                                        byte_count, 0, 0,
                                        samples_left_from_frame, looped);
        return ptr;
    }

    requested_samples = frame_count << NATIVE_ADPCM_FRAME_SHIFT;
    overflow = (s32)(load->memin + byte_count -
                     ((intptr_t)load->table->base + load->table->len));
    if (overflow < 0) {
        overflow = 0;
    }

    overflow_samples =
        (overflow / NATIVE_ADPCM_FRAME_BYTES) << NATIVE_ADPCM_FRAME_SHIFT;
    if (overflow_samples > requested_samples + samples_left_from_frame) {
        overflow_samples = requested_samples + samples_left_from_frame;
    }

    byte_count -= overflow;
    native_audio_filter_trace_adpcm(load, out_count, requested_samples,
                                    samples_to_decode, frame_count,
                                    byte_count, overflow, overflow_samples,
                                    samples_left_from_frame, looped);

    if ((overflow_samples - (overflow_samples & 0xf)) < out_count) {
        decoded = 1;
        ptr = _decodeChunk(ptr, load, requested_samples - overflow_samples,
                                 byte_count, *outp, input, (u32)load->first);

        if (load->lastsam != 0) {
            *outp += (s16)(load->lastsam << 1);
        } else {
            *outp += ADPCMFSIZE << 1;
        }

        load->lastsam = (out_count + load->lastsam) & 0xf;
        load->sample += out_count;
        load->memin += NATIVE_ADPCM_FRAME_BYTES * frame_count;
    } else {
        load->lastsam = 0;
        load->memin += NATIVE_ADPCM_FRAME_BYTES * frame_count;
    }

    if (overflow_samples != 0) {
        load->lastsam = 0;
        zero_start = decoded
            ? (samples_left_from_frame + requested_samples - overflow_samples)
                  << 1
            : 0;
        aClearBuffer(ptr++, zero_start + *outp, overflow_samples << 1);
    }

    return ptr;
}

Acmd *alRaw16Pull(void *filter, s16 *outp, s32 out_count,
                  s32 sample_offset, Acmd *cmd)
{
    ALLoadFilter *load = (ALLoadFilter *)filter;
    Acmd *ptr = cmd;
    s32 byte_count;
    intptr_t dram_location;
    intptr_t dram_align;
    intptr_t dmem_align;
    s32 overflow;
    s32 zero_start;
    s32 sample_count;
    s32 output_pos;

    (void)sample_offset;

    if (load == NULL || outp == NULL || ptr == NULL || out_count == 0 ||
        load->table == NULL || load->dma == NULL ||
        load->table->type != AL_RAW16_WAVE) {
        return ptr;
    }

    if (out_count + load->sample > (s32)load->loop.end &&
        load->loop.count != 0) {
        sample_count = (s32)load->loop.end - load->sample;
        byte_count = sample_count << 1;

        if (sample_count > 0) {
            dram_location = load->dma(load->memin, byte_count, load->dmaState);
            dram_align = dram_location & 7;
            byte_count += (s32)dram_align;
            aSetBuffer(ptr++, 0, *outp, 0,
                       load_buffer_dma_count(byte_count));
            aLoadRaw16Buffer(ptr++,
                        (void *)(uintptr_t)(dram_location - dram_align));
        } else {
            dram_align = 0;
        }

        *outp += (s16)dram_align;
        load->memin =
            (intptr_t)load->table->base + ((intptr_t)load->loop.start << 1);
        load->sample = load->loop.start;
        output_pos = *outp;

        while (out_count > sample_count) {
            output_pos += sample_count << 1;
            out_count -= sample_count;

            if (load->loop.count != -1 && load->loop.count != 0) {
                load->loop.count--;
            }

            sample_count =
                min_s32(out_count, (s32)load->loop.end - (s32)load->loop.start);
            byte_count = sample_count << 1;
            dram_location = load->dma(load->memin, byte_count, load->dmaState);
            dram_align = dram_location & 7;
            byte_count += (s32)dram_align;
            dmem_align = (output_pos & 7) != 0 ? 8 - (output_pos & 7) : 0;

            aSetBuffer(ptr++, 0, output_pos + (s16)dmem_align, 0,
                       load_buffer_dma_count(byte_count));
            aLoadRaw16Buffer(ptr++,
                        (void *)(uintptr_t)(dram_location - dram_align));

            if (dram_align != 0 || dmem_align != 0) {
                aDMEMMove(ptr++, output_pos + (s32)dram_align + (s32)dmem_align,
                          output_pos, sample_count << 1);
            }
        }

        load->sample += out_count;
        load->memin += out_count << 1;
        return ptr;
    }

    byte_count = out_count << 1;
    overflow = (s32)(load->memin + byte_count -
                     ((intptr_t)load->table->base + load->table->len));
    if (overflow < 0) {
        overflow = 0;
    }
    if (overflow > byte_count) {
        overflow = byte_count;
    }

    if (overflow < byte_count) {
        if (out_count > 0) {
            byte_count -= overflow;
            dram_location = load->dma(load->memin, byte_count, load->dmaState);
            dram_align = dram_location & 7;
            byte_count += (s32)dram_align;
            aSetBuffer(ptr++, 0, *outp, 0,
                       load_buffer_dma_count(byte_count));
            aLoadRaw16Buffer(ptr++,
                        (void *)(uintptr_t)(dram_location - dram_align));
        } else {
            dram_align = 0;
        }

        *outp += (s16)dram_align;
        load->sample += out_count;
        load->memin += out_count << 1;
    } else {
        load->memin += out_count << 1;
    }

    if (overflow != 0) {
        zero_start = (out_count << 1) - overflow;
        if (zero_start < 0) {
            zero_start = 0;
        }
        aClearBuffer(ptr++, zero_start + *outp, overflow);
    }

    return ptr;
}

s32 alLoadParam(void *filter, s32 param_id, void *param)
{
    ALLoadFilter *load = (ALLoadFilter *)filter;
    ALFilter *base = (ALFilter *)filter;
    ALWaveTable *table;
    ALADPCMBook *book;

    if (load == NULL) {
        return 0;
    }

    switch (param_id) {
        case AL_FILTER_SET_WAVETABLE:
            table = (ALWaveTable *)param;
            load->table = table;
            load->sample = 0;
            load->memin = table != NULL ? (intptr_t)table->base : 0;

            if (table == NULL) {
                load->loop.start = 0;
                load->loop.end = 0;
                load->loop.count = 0;
                break;
            }

            switch (table->type) {
                case AL_ADPCM_WAVE:
                    base->handler = alAdpcmPull;
                    table->len = NATIVE_ADPCM_FRAME_BYTES *
                                 (table->len / NATIVE_ADPCM_FRAME_BYTES);
                    book = table->waveInfo.adpcmWave.book;
                    load->bookSize = book != NULL
                        ? 2 * book->order * book->npredictors * ADPCMVSIZE
                        : 0;

                    if (table->waveInfo.adpcmWave.loop != NULL) {
                        load->loop.start =
                            table->waveInfo.adpcmWave.loop->start;
                        load->loop.end = table->waveInfo.adpcmWave.loop->end;
                        load->loop.count =
                            table->waveInfo.adpcmWave.loop->count;
                        if (load->lstate != NULL) {
                            memcpy(load->lstate,
                                   table->waveInfo.adpcmWave.loop->state,
                                   sizeof(ADPCM_STATE));
                        }
                    } else {
                        load->loop.start = 0;
                        load->loop.end = 0;
                        load->loop.count = 0;
                    }
                    break;

                case AL_RAW16_WAVE:
                    base->handler = alRaw16Pull;
                    if (table->waveInfo.rawWave.loop != NULL) {
                        load->loop.start = table->waveInfo.rawWave.loop->start;
                        load->loop.end = table->waveInfo.rawWave.loop->end;
                        load->loop.count = table->waveInfo.rawWave.loop->count;
                    } else {
                        load->loop.start = 0;
                        load->loop.end = 0;
                        load->loop.count = 0;
                    }
                    break;

                default:
                    break;
            }
            break;

        case AL_FILTER_RESET:
            load->lastsam = 0;
            load->first = 1;
            load->sample = 0;

            if (load->table != NULL) {
                load->memin = (intptr_t)load->table->base;
                if (load->table->type == AL_ADPCM_WAVE &&
                    load->table->waveInfo.adpcmWave.loop != NULL) {
                    load->loop.count =
                        load->table->waveInfo.adpcmWave.loop->count;
                } else if (load->table->type == AL_RAW16_WAVE &&
                           load->table->waveInfo.rawWave.loop != NULL) {
                    load->loop.count = load->table->waveInfo.rawWave.loop->count;
                }
            }
            break;

        default:
            break;
    }

    return 0;
}

Acmd *alAuxBusPull(void *filter, s16 *outp, s32 out_count, s32 sample_offset,
                   Acmd *cmd)
{
    ALAuxBus *bus = (ALAuxBus *)filter;
    Acmd *ptr = cmd;
    s32 i;

    if (bus == NULL || ptr == NULL) {
        return ptr;
    }

    aClearBuffer(ptr++, AL_AUX_L_OUT, out_count << 1);
    aClearBuffer(ptr++, AL_AUX_R_OUT, out_count << 1);

    for (i = 0; i < bus->sourceCount; i++) {
        ALFilter *source = bus->sources[i];

        if (source != NULL && source->handler != NULL) {
            ptr = source->handler(source, outp, out_count, sample_offset, ptr);
        }
    }

    return ptr;
}

s32 alAuxBusParam(void *filter, s32 param_id, void *param)
{
    ALAuxBus *bus = (ALAuxBus *)filter;

    if (bus == NULL) {
        return 0;
    }

    if (param_id == AL_FILTER_ADD_SOURCE &&
        bus->sourceCount < bus->maxSources) {
        bus->sources[bus->sourceCount++] = (ALFilter *)param;
    } else if (param_id == AL_FILTER_UNK11) {
        /* DKR: remove a source (func_80065A80's re-parent). Swap-remove —
         * bus order carries no meaning, the pull just sums every source. */
        s32 i;

        for (i = 0; i < bus->sourceCount; i++) {
            if (bus->sources[i] == (ALFilter *)param) {
                bus->sourceCount--;
                bus->sources[i] = bus->sources[bus->sourceCount];
                break;
            }
        }
    }

    return 0;
}

Acmd *alMainBusPull(void *filter, s16 *outp, s32 out_count, s32 sample_offset,
                    Acmd *cmd)
{
    ALMainBus *bus = (ALMainBus *)filter;
    Acmd *ptr = cmd;
    s32 i;

    if (bus == NULL || ptr == NULL) {
        return ptr;
    }

    aClearBuffer(ptr++, AL_MAIN_L_OUT, out_count << 1);
    aClearBuffer(ptr++, AL_MAIN_R_OUT, out_count << 1);

    for (i = 0; i < bus->sourceCount; i++) {
        ALFilter *source = bus->sources[i];

        if (source != NULL && source->handler != NULL) {
            ptr = source->handler(source, outp, out_count, sample_offset, ptr);
            aSetBuffer(ptr++, 0, 0, 0, out_count << 1);
            aMix(ptr++, 0, 0x7fff, AL_AUX_L_OUT, AL_MAIN_L_OUT);
            aMix(ptr++, 0, 0x7fff, AL_AUX_R_OUT, AL_MAIN_R_OUT);
        }
    }

    return ptr;
}

s32 alMainBusParam(void *filter, s32 param_id, void *param)
{
    ALMainBus *bus = (ALMainBus *)filter;

    if (bus == NULL) {
        return 0;
    }

    if (param_id == AL_FILTER_ADD_SOURCE &&
        bus->sourceCount < bus->maxSources) {
        bus->sources[bus->sourceCount++] = (ALFilter *)param;
    }

    return 0;
}

Acmd *alSavePull(void *filter, s16 *outp, s32 out_count, s32 sample_offset,
                 Acmd *cmd)
{
    ALSave *save = (ALSave *)filter;
    ALFilter *source;
    Acmd *ptr = cmd;

    if (save == NULL || ptr == NULL) {
        return ptr;
    }

    source = save->filter.source;
    if (source == NULL || source->handler == NULL) {
        return ptr;
    }

    ptr = source->handler(source, outp, out_count, sample_offset, ptr);
    aSetBuffer(ptr++, 0, 0, 0, out_count << 1);
    aInterleave(ptr++, AL_MAIN_L_OUT, AL_MAIN_R_OUT);
    aSetBuffer(ptr++, 0, 0, 0, out_count << 2);
    aSaveBuffer(ptr++, save->dramout);
    return ptr;
}

s32 alSaveParam(void *filter, s32 param_id, void *param)
{
    ALSave *save = (ALSave *)filter;

    if (save == NULL) {
        return 0;
    }

    switch (param_id) {
        case AL_FILTER_SET_SOURCE:
            save->filter.source = (ALFilter *)param;
            break;

        case AL_FILTER_SET_DRAM:
            save->dramout = (intptr_t)param;
            break;

        default:
            break;
    }

    return 0;
}

Acmd *alResamplePull(void *filter, s16 *outp, s32 out_count,
                     s32 sample_offset, Acmd *cmd)
{
    ALResampler *resampler = (ALResampler *)filter;
    ALFilter *source;
    ALLoadFilter *load = NULL;
    Acmd *ptr = cmd;
    s16 input = AL_DECODER_OUT;
    s32 input_count;
    f32 float_input_count;
    f32 previous_delta;
    s32 increment;

    if (resampler == NULL || ptr == NULL || out_count == 0) {
        return ptr;
    }

    source = resampler->filter.source;
    if (source == NULL || source->handler == NULL) {
        return ptr;
    }
    if (source->type == AL_ADPCM || source->type == AL_BUFFER) {
        load = (ALLoadFilter *)source;
    }

    if (resampler->upitch) {
        ptr = source->handler(source, &input, out_count, sample_offset, ptr);
        aDMEMMove(ptr++, input, *outp, out_count << 1);
        return ptr;
    }

    if (resampler->ratio > MAX_RATIO) {
        resampler->ratio = MAX_RATIO;
    }

    resampler->ratio = (s32)(resampler->ratio * UNITY_PITCH);
    resampler->ratio = resampler->ratio / UNITY_PITCH;

    previous_delta = resampler->delta;
    float_input_count = previous_delta + (resampler->ratio * (f32)out_count);
    input_count = (s32)float_input_count;
    resampler->delta = float_input_count - (f32)input_count;

    ptr = source->handler(source, &input, input_count, sample_offset, ptr);
    increment = (s32)(resampler->ratio * UNITY_PITCH);
    native_audio_filter_trace_resample(resampler, load, out_count,
                                       input_count, increment,
                                       float_input_count, previous_delta);
    aSetBuffer(ptr++, 0, input, *outp, out_count << 1);
    aResample(ptr++, resampler->first, increment,
              osVirtualToPhysical(resampler->state));
    resampler->first = 0;
    return ptr;
}

s32 alResampleParam(void *filter, s32 param_id, void *param)
{
    ALResampler *resampler = (ALResampler *)filter;
    ALFilter *base = (ALFilter *)filter;

    if (resampler == NULL) {
        return 0;
    }

    switch (param_id) {
        case AL_FILTER_SET_SOURCE:
            base->source = (ALFilter *)param;
            break;

        case AL_FILTER_RESET:
            resampler->delta = 0.0f;
            resampler->first = 1;
            resampler->motion = AL_STOPPED;
            resampler->upitch = 0;
            if (base->source != NULL && base->source->setParam != NULL) {
                base->source->setParam(base->source, AL_FILTER_RESET, NULL);
            }
            break;

        case AL_FILTER_START:
            resampler->motion = AL_PLAYING;
            if (base->source != NULL && base->source->setParam != NULL) {
                base->source->setParam(base->source, AL_FILTER_START, NULL);
            }
            break;

        case AL_FILTER_SET_PITCH:
            resampler->ratio = alParamToF32Bits(param);
            break;

        case AL_FILTER_SET_UNITY_PITCH:
            resampler->upitch = 1;
            break;

        default:
            if (base->source != NULL && base->source->setParam != NULL) {
                base->source->setParam(base->source, param_id, param);
            }
            break;
    }

    return 0;
}

void alSynAddPlayer(ALSynth *s, ALPlayer *client)
{
    OSIntMask mask;

    if (s == NULL || client == NULL) {
        return;
    }

    mask = osSetIntMask(OS_IM_NONE);
    client->samplesLeft = s->curSamples;
    client->next = s->head;
    s->head = client;
    osSetIntMask(mask);
}

s32 alSynAllocVoice(ALSynth *s, ALVoice *voice, ALVoiceConfig *config)
{
    PVoice *physical;

    if (s == NULL || voice == NULL || config == NULL) {
        return 0;
    }

    voice->priority = config->priority;
    voice->unityPitch = config->unityPitch;
    voice->table = NULL;
    voice->fxBus = config->fxBus;
    voice->state = AL_STOPPED;
    voice->pvoice = NULL;

    physical = take_first_voice(&s->pLameList, &s->pAllocList);
    if (physical == NULL) {
        physical = take_first_voice(&s->pFreeList, &s->pAllocList);
    }

    if (physical != NULL) {
        physical->offset = 0;
    } else {
        physical = find_stealable_voice(s, config->priority);
        if (physical != NULL) {
            ALVoice *old_voice = physical->vvoice;
            ALFilter *filter = physical->channelKnob;
            ALParam *fade = __allocParam();
            ALParam *stop = __allocParam();

            if (fade == NULL || stop == NULL) {
                __freeParam(fade);
                __freeParam(stop);
                return 0;
            }

            physical->offset = 512;
            if (old_voice != NULL) {
                old_voice->pvoice = NULL;
            }

            fade->next = NULL;
            fade->delta = s->paramSamples;
            fade->type = AL_FILTER_SET_VOLUME;
            fade->data.i = 0;
            fade->moredata.i = physical->offset - 64;
            enqueue_filter_update(filter, fade);

            stop->next = NULL;
            stop->delta = s->paramSamples + physical->offset;
            stop->type = AL_FILTER_STOP_VOICE;
            enqueue_filter_update(filter, stop);
        }
    }

    if (physical == NULL) {
        return 0;
    }

    physical->vvoice = voice;
    voice->pvoice = physical;
    return 1;
}

void alSynSetVol(ALSynth *s, ALVoice *voice, s16 volume, ALMicroTime delta)
{
    ALParam *update = alloc_voice_update(s, voice, AL_FILTER_SET_VOLUME);

    if (update == NULL) {
        return;
    }

    /* NOTE: mgb64 scales SFX-voice volume by its Audio.SfxVolume bus here. DKR
     * has no such bus — game/src/audio.c applies its own music/SFX volumes
     * before they reach the synth — so the volume passes through unmodified. */
    update->data.i = volume;
    update->moredata.i = _timeToSamples(s, delta);
    enqueue_voice_update(voice, update);
}

void alSynSetPan(ALSynth *s, ALVoice *voice, ALPan pan)
{
    ALParam *update = alloc_voice_update(s, voice, AL_FILTER_SET_PAN);

    if (update == NULL) {
        return;
    }

    update->data.i = modify_panning(pan);
    enqueue_voice_update(voice, update);
}

void alSynSetPitch(ALSynth *s, ALVoice *voice, f32 pitch)
{
    ALParam *update = alloc_voice_update(s, voice, AL_FILTER_SET_PITCH);

    if (update == NULL) {
        return;
    }

    update->data.f = pitch;
    enqueue_voice_update(voice, update);
}

void alSynSetFXMix(ALSynth *s, ALVoice *voice, u8 fxmix)
{
    ALParam *update = alloc_voice_update(s, voice, AL_FILTER_SET_FXAMT);

    if (update == NULL) {
        return;
    }

    update->data.i = fxmix;
    enqueue_voice_update(voice, update);
}

void alSynStartVoice(ALSynth *s, ALVoice *voice, ALWaveTable *table)
{
    ALStartParam *update =
        (ALStartParam *)alloc_voice_update(s, voice, AL_FILTER_START_VOICE);

    if (update == NULL) {
        return;
    }

    update->wave = table;
    update->unity = voice->unityPitch;
    enqueue_voice_update(voice, (ALParam *)update);
}

void alSynStartVoiceParams(ALSynth *s, ALVoice *voice, ALWaveTable *table,
                           f32 pitch, s16 vol, ALPan pan, u8 fxmix,
                           ALMicroTime delta)
{
    ALStartParamAlt *update = (ALStartParamAlt *)alloc_voice_update(
        s, voice, AL_FILTER_START_VOICE_ALT);

    if (update == NULL) {
        return;
    }

    update->unity = voice->unityPitch;
    update->pan = (ALPan)modify_panning(pan);
    update->volume = vol;
    update->fxMix = fxmix;
    update->pitch = pitch;
    update->samples = _timeToSamples(s, delta);
    update->wave = table;
    enqueue_voice_update(voice, (ALParam *)update);
}

void alSynStopVoice(ALSynth *s, ALVoice *voice)
{
    ALParam *update = alloc_voice_update(s, voice, AL_FILTER_STOP_VOICE);

    if (update == NULL) {
        return;
    }

    enqueue_voice_update(voice, update);
}

void alSynFreeVoice(ALSynth *s, ALVoice *voice)
{
    ALFreeParam *update;

    if (s == NULL || voice == NULL || voice->pvoice == NULL) {
        return;
    }

    if (voice->pvoice->offset == 0) {
        _freePVoice(s, voice->pvoice);
        voice->pvoice = NULL;
        return;
    }

    update = (ALFreeParam *)alloc_voice_update(s, voice, AL_FILTER_FREE_VOICE);
    if (update == NULL) {
        return;
    }

    update->pvoice = voice->pvoice;
    enqueue_voice_update(voice, (ALParam *)update);
    voice->pvoice = NULL;
}

ALFxRef *alSynAllocFX(ALSynth *s, s16 bus, ALSynConfig *config, ALHeap *heap)
{
    ALAuxBus *aux;

    if (s == NULL || s->auxBus == NULL || s->mainBus == NULL ||
        bus < 0 || bus >= s->maxAuxBusses) {
        return NULL;
    }

    aux = &s->auxBus[bus];
    alFxNew(&aux->fx[0], config, heap);
    alFxParam(&aux->fx[0], AL_FILTER_SET_SOURCE, aux);
    alMainBusParam(s->mainBus, AL_FILTER_ADD_SOURCE, &aux->fx[0]);
    return (ALFxRef *)&aux->fx[0];
}

void alSeqpSetBank(ALSeqPlayer *seqp, ALBank *bank)
{
    ALEvent event;

    if (seqp == NULL) {
        return;
    }

    event.type = AL_SEQP_BANK_EVT;
    event.msg.spbank.bank = bank;
    alEvtqPostEvent(&seqp->evtq, &event, 0);
}

void alCSPPlay(ALCSPlayer *seqp)
{
    ALEvent event;

    if (seqp == NULL) {
        return;
    }

    event.type = AL_SEQP_PLAY_EVT;
    alEvtqPostEvent(&seqp->evtq, &event, 0);
}

void alCSPSetSeq(ALCSPlayer *seqp, ALCSeq *seq)
{
    ALEvent event;

    if (seqp == NULL) {
        return;
    }

    event.type = AL_SEQP_SEQ_EVT;
    event.msg.spseq.seq = seq;
    alEvtqPostEvent(&seqp->evtq, &event, 0);
}

void alCSPSetVol(ALCSPlayer *seqp, s16 vol)
{
    ALVoiceState *voice;

    if (seqp == NULL) {
        return;
    }

    seqp->vol = vol;
    for (voice = seqp->vAllocHead; voice != NULL; voice = voice->next) {
        alSynSetVol(seqp->drvr,
                    &voice->voice,
                    __vsVol(voice, (ALSeqPlayer *)seqp),
                    __vsDelta(voice, seqp->curTime));
    }
}

void alCSPStop(ALCSPlayer *seqp)
{
    ALVoiceState *voice;

    if (seqp == NULL) {
        return;
    }

    for (voice = seqp->vAllocHead; voice != NULL;) {
        ALVoiceState *next = voice->next;

        if (seqp->drvr != NULL) {
            alSynStopVoice(seqp->drvr, &voice->voice);
            alSynFreeVoice(seqp->drvr, &voice->voice);
        }

        if (voice->flags != 0 && seqp->stopOsc != NULL) {
            __seqpStopOsc((ALSeqPlayer *)seqp, voice);
        }

        __unmapVoice((ALSeqPlayer *)seqp, &voice->voice);
        voice = next;
    }

    alEvtqFlush(&seqp->evtq);
    seqp->state = AL_STOPPED;
    seqp->nextDelta =
        seqp->frameTime > 0 ? seqp->frameTime : AL_USEC_PER_FRAME;
    seqp->nextEvent.type = AL_SEQP_API_EVT;
}

/* ===================================================================
 * DKR EXTENSIONS
 *
 * Everything below is specific to Diddy Kong Racing and has no
 * counterpart in the mgb64 engine above: the extra per-channel volume
 * lanes, the sequence-player accessors DKR's game code calls, the
 * aux-bus re-parent helper, and the reverb bounds-guard readout.
 *
 * The mutating accessors post MIDI events onto the player's queue
 * rather than writing channel state directly, exactly as the routines
 * they replace did. That keeps them ordered against the sequence's own
 * events and routes them through the one MIDI handler, so a programmatic
 * volume change and a sequenced one cannot disagree.
 * =================================================================== */

/* ---- reverb ------------------------------------------------------ */

void alFxReverbSet(u8 setting)
{
    alFXEnabled = setting;
}

u8 _alFxEnabled(void)
{
    return alFXEnabled;
}

/*
 * Reverb delay-line bounds-guard trip count. Non-zero means a delay-line
 * transfer tried to leave its allocation and was clamped; the audio
 * fixtures assert this stays 0. The counter itself lives with the
 * transfer planner in audio_fx_transfer.c.
 */
u32 alFxGuardTrips(void)
{
    return (u32)portAudioFxGuardTripCount();
}

/* ---- sequence player: polyphony ---------------------------------- */

void set_voice_limit(ALCSPlayer *seqp, u8 voiceLimit)
{
    if (seqp != NULL) {
        seqp->voiceLimit = voiceLimit;
    }
}

/*
 * DKR never constructs a stock ALSeqPlayer — both the music and jingle
 * players are ALCSPlayers — so this exists only to satisfy the libaudio
 * ABI, matching the empty definition it replaces.
 */
void alSeqpNew(ALSeqPlayer *seqp, ALSeqpConfig *config)
{
    (void)seqp;
    (void)config;
}

/* ---- sequence player: accessors ---------------------------------- */

static void csp_post_midi(ALCSPlayer *seqp, u8 status, u8 byte1, u8 byte2)
{
    ALEvent evt;

    if (seqp == NULL) {
        return;
    }

    memset(&evt, 0, sizeof(evt));
    evt.type = AL_SEQP_MIDI_EVT;
    evt.msg.midi.ticks = 0;
    evt.msg.midi.status = status;
    evt.msg.midi.byte1 = byte1;
    evt.msg.midi.byte2 = byte2;
    alEvtqPostEvent(&seqp->evtq, &evt, 0);
}

void alCSPSetBank(ALCSPlayer *seqp, ALBank *b)
{
    if (seqp == NULL) {
        return;
    }

    seqp->bank = b;
    if (b != NULL) {
        __initFromBank((ALSeqPlayer *)seqp, b);
    }
}

u8 alCSPGetChlVol(ALCSPlayer *seqp, u8 chan)
{
    if (seqp == NULL || CSP_CHAN(seqp) == NULL || chan >= seqp->maxChannels) {
        return 0;
    }

    return CSP_CHAN(seqp)[chan].vol;
}

void alCSPSetChlVol(ALCSPlayer *seqp, u8 chan, u8 vol)
{
    csp_post_midi(seqp, (u8)(AL_MIDI_ControlChange | chan),
                  AL_MIDI_VOLUME_CTRL, vol);
}

void alCSPSetChlPan(ALCSPlayer *seqp, u8 chan, ALPan pan)
{
    csp_post_midi(seqp, (u8)(AL_MIDI_ControlChange | chan),
                  AL_MIDI_PAN_CTRL, pan);
}

u8 alCSPGetFadeIn(ALCSPlayer *seqp, u8 chan)
{
    if (seqp == NULL || CSP_CHAN(seqp) == NULL || chan >= seqp->maxChannels) {
        return 0;
    }

    return CSP_CHAN(seqp)[chan].fade;
}

/* `pan` is a misnomer inherited from the original prototype: the payload is
 * the 0..127 fade level. */
void alCSPSetFadeIn(ALCSPlayer *seqp, u8 chan, ALPan pan)
{
    csp_post_midi(seqp, (u8)(AL_MIDI_ControlChange | chan),
                  AL_MIDI_UNK_8, pan);
}

u8 alSeqpGetChlFXMix(ALSeqPlayer *seqp, u8 chan)
{
    if (seqp == NULL || CSP_CHAN(seqp) == NULL || chan >= seqp->maxChannels) {
        return 0;
    }

    return CSP_CHAN(seqp)[chan].fxmix;
}

/*
 * Tempo is exchanged in microseconds per quarter note (the MIDI tempo
 * meta-event unit); the player stores microseconds per TICK. Going out we
 * divide by the sequence's quarter-notes-per-tick, going in the meta handler
 * multiplies by it — and also re-times any queued note-offs so they keep
 * their position in ticks.
 */
s32 alCSPGetTempo(ALCSPlayer *seqp)
{
    if (seqp == NULL || seqp->target == NULL || seqp->target->qnpt == 0.0f) {
        return 0;
    }

    return (s32)((f32)seqp->uspt / seqp->target->qnpt);
}

void alCSPSetTempo(ALCSPlayer *seqp, s32 tempo)
{
    ALEvent evt;

    if (seqp == NULL) {
        return;
    }

    memset(&evt, 0, sizeof(evt));
    evt.type = AL_SEQP_META_EVT;
    evt.msg.tempo.ticks = 0;
    evt.msg.tempo.type = AL_MIDI_META_TEMPO;
    evt.msg.tempo.byte1 = (u8)((tempo >> 16) & 0xFF);
    evt.msg.tempo.byte2 = (u8)((tempo >> 8) & 0xFF);
    evt.msg.tempo.byte3 = (u8)(tempo & 0xFF);
    alEvtqPostEvent(&seqp->evtq, &evt, 0);
}

/* ---- channel enable/disable -------------------------------------- */

void alSeqChOff(ALCSPlayer *seqp, u8 chan)
{
    csp_post_midi(seqp, AL_MIDI_ControlChange, AL_MIDI_UNK_6A, chan);
}

void alSeqChOn(ALCSPlayer *seqp, u8 chan)
{
    csp_post_midi(seqp, AL_MIDI_ControlChange, AL_MIDI_UNK_6C, chan);
}

/*
 * Post the global FX-send floor described in the AL_MIDI_UNK_5F handler.
 * The status byte carries no channel nibble because the handler sweeps
 * every channel regardless.
 */
void func_80063A90(ALCSPlayer *seqp, u8 chan)
{
    csp_post_midi(seqp, AL_MIDI_ControlChange, AL_MIDI_UNK_5F, chan);
}

/* ---- aux bus ------------------------------------------------------ */

/*
 * Re-parent a physical voice's envmixer onto aux bus `bus`.
 *
 * Physical voices are pooled and shared across players, so a slot reused
 * from another owner may still be wired to the bus that owner wanted.
 * Every CSP note-on calls this with bus 0 to guarantee the voice is on the
 * bus its new owner expects before it sounds. No-op when it already is.
 */
void func_80065A80(ALSynth *drvr, PVoice *pvoice, s16 bus)
{
    if (drvr == NULL || pvoice == NULL || drvr->auxBus == NULL) {
        return;
    }

    if (bus < 0 || bus >= drvr->maxAuxBusses || bus == pvoice->unkDC) {
        return;
    }

    alAuxBusParam(&drvr->auxBus[pvoice->unkDC], AL_FILTER_UNK11,
                  &pvoice->envmixer);
    alAuxBusParam(&drvr->auxBus[bus], AL_FILTER_ADD_SOURCE, &pvoice->envmixer);
    pvoice->unkDC = bus;
}
