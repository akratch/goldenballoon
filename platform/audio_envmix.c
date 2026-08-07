/*
 * Clean-room native audio engine: the envelope mixer.
 *
 * Per-voice attack/decay/release ramps, the equal-power pan table the mixer
 * indexes, and the subframe pull that applies them. Split out of
 * audio_compat.c; see audio_compat_internal.h.
 */

#include "audio_compat_internal.h"

#define NATIVE_EQPOWER_LENGTH 128
#define NATIVE_HALF_PI 1.57079632679489661923
static s16 s_native_eqpower[NATIVE_EQPOWER_LENGTH];
static int s_native_eqpower_ready = 0;

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
