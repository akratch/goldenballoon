/*
 * Clean-room native audio engine: the reverb/FX unit.
 *
 * The delay-line ring and its bounds guard, the chorus/pitch modulation, the
 * low-pass filter, and the FX filter's pull and parameter paths. Split out of
 * audio_compat.c; see audio_compat_internal.h.
 */

#include "audio_compat_internal.h"

#define NATIVE_FX_FILTER_SCALE 16384
#define NATIVE_FX_RATE_RANGE 2.0f
#define NATIVE_FX_CENTS_DENOM 173123.404906676f

static s32 s_native_fx_bypass_params[2] = {
    0,
    AL_FX_BUFFER_SIZE,
};

/*
 * AL_FX_BIGROOM reverb preset — aux bus 1.
 *
 * Diddy Kong Racing declares two FX buses (game/src/audio.c): bus 0 is its
 * ROM-authored AL_FX_CUSTOM music reverb, and bus 1 is the fixed libaudio
 * AL_FX_BIGROOM room reverb that every SFX voice is re-parented onto for the
 * cave/tunnel echo (audiosfx.c). This is the same delay-line reverb the CUSTOM
 * preset drives; only the tap/coefficient table differs, so the crossfade and
 * delay-line DSP consume it unchanged.
 *
 * Layout is the flat s32[] alFxNew() parses: section count, ring length, then
 * eight fields per section — input tap, output tap, fbcoef, ffcoef, gain,
 * chorus rate, chorus depth, low-pass cutoff. Delay taps are in samples on the
 * synth's 44.1 kHz-derived clock, where the preset table expresses a delay in
 * milliseconds as ms * 40 (44 truncated to an 8-sample boundary): the ring is
 * 100 ms and the taps are 66/22/54/66/91/94 ms. The final section adds the
 * 0x5000 low-pass cutoff that gives the big room its darker tail.
 */
static s32 s_native_fx_bigroom_params[2 + 4 * 8] = {
    /* sections  ring length (100 ms) */
    4,           4000,
    /* input  output  fbcoef  ffcoef    gain  chorusRate chorusDepth  lpCoef */
       0,       2640,   9830,  -9830,      0,          0,          0,       0,
     880,       2160,   3276,  -3276, 0x3fff,          0,          0,       0,
    2640,       3640,   3276,  -3276, 0x3fff,          0,          0,       0,
       0,       3760,   8000,      0,      0,          0,          0,  0x5000,
};

static s32 *native_fx_params_for_config(ALSynConfig *config, s16 bus)
{
    /* DKR's ALSynConfig carries one ALFxId PER AUX BUS (fxType[2]), so the
     * effect a bus should build is fxType[bus]. Reading it as a scalar silently
     * compared an array address against the enum, always missed AL_FX_CUSTOM,
     * and fell through to the bypass params — which allocate the reverb but give
     * it no delay line, so the wet path is inaudible and reverb-on and
     * reverb-off captures come out identical. Select per bus instead: bus 0's
     * AL_FX_CUSTOM reads the ROM params, bus 1's AL_FX_BIGROOM uses the fixed
     * preset above. */
    if (config == NULL || bus < 0 || bus > 1) {
        return s_native_fx_bypass_params;
    }

    switch (config->fxType[bus]) {
    case AL_FX_CUSTOM:
        if (config->params != NULL) {
            return config->params;
        }
        break;
    case AL_FX_BIGROOM:
        return s_native_fx_bigroom_params;
    default:
        break;
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

void alFxNew(ALFx *fx, ALSynConfig *config, s16 bus, ALHeap *heap)
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

    params = native_fx_params_for_config(config, bus);
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
