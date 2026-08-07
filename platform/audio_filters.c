/*
 * Clean-room native audio engine: the sample-loading and routing filters.
 *
 * ADPCM and RAW16 decoding, the resampler, the aux and main buses, the save
 * filter, and the constructors that wire them into a chain. Split out of
 * audio_compat.c; see audio_compat_internal.h.
 */

#include "audio_compat_internal.h"

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
        s_native_audio_filter_trace_fp = mdkr_fopen_utf8(path, "w");
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

            if (load->loop.count != (u32)-1 && load->loop.count != 0) {
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

            if (load->loop.count != (u32)-1 && load->loop.count != 0) {
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
