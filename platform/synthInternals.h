#ifndef __audioInternals__
#define __audioInternals__

/*
 * Clean-room internal audio synthesizer declarations shared by the native
 * audio compatibility layer and matching-target libultra audio sources.
 */

#include <PR/os.h>
#include <libaudio.h>
#include <PR/abi.h>

enum {
    AL_FILTER_FREE_VOICE,
    AL_FILTER_SET_SOURCE,
    AL_FILTER_ADD_SOURCE,
    AL_FILTER_ADD_UPDATE,
    AL_FILTER_RESET,
    AL_FILTER_SET_WAVETABLE,

    AL_FILTER_SET_DRAM,
    AL_FILTER_SET_PITCH,
    AL_FILTER_SET_UNITY_PITCH,
    AL_FILTER_START,

    AL_FILTER_SET_STATE,
    AL_FILTER_SET_VOLUME,
    AL_FILTER_SET_PAN,
    AL_FILTER_START_VOICE_ALT,
    AL_FILTER_START_VOICE,
    AL_FILTER_STOP_VOICE,
    AL_FILTER_SET_FXAMT,
    /* DKR: aux-bus source removal (alAuxBusParam), used by func_80065A80 to
     * re-parent a physical voice's envmixer from one aux bus to another. */
    AL_FILTER_UNK11
};

#define AL_MAX_RSP_SAMPLES      160

/*
 * Legacy libaudio diagnostic ID used by ALFailIf in debug builds. Keep the
 * one ABI value needed by these clean-room synth helpers local instead of
 * depending on the broader legacy ultraerror.h catalog.
 */
#define AL_ERR_SYN_NO_UPDATE    106

#define AL_DECODER_IN	        0
#define	AL_RESAMPLER_OUT	0
#define AL_TEMP_0	        0
#define	AL_DECODER_OUT	        320
#define	AL_TEMP_1	        320
#define	AL_TEMP_2	        640
#define	AL_MAIN_L_OUT	        1088
#define	AL_MAIN_R_OUT	        1408
#define	AL_AUX_L_OUT	        1728
#define	AL_AUX_R_OUT	        2048

enum {
    AL_ADPCM,
    AL_RESAMPLE,
    AL_BUFFER,
    AL_SAVE,
    AL_ENVMIX,
    AL_FX,
    AL_AUXBUS,
    AL_MAINBUS
};

typedef struct ALParam_s {
    struct ALParam_s    *next;
    s32                 delta;
    s16                 type;
    union {
        f32             f;
        s32             i;
    } data;
    union {
        f32             f;
        s32             i;
    } moredata;
    union {
        f32             f;
        s32             i;
    } stillmoredata;
    union {
        f32             f;
        s32             i;
    } yetstillmoredata;
} ALParam;

typedef struct {
    struct ALParam_s            *next;
    s32                         delta;
    s16                         type;
    s16                         unity;
    f32                         pitch;
    s16                         volume;
    ALPan                       pan;
    u8                          fxMix;
    s32                         samples;
    struct ALWaveTable_s        *wave;
} ALStartParamAlt;

typedef struct {
    struct ALParam_s            *next;
    s32                         delta;
    s16                         type;
    s16                         unity;
    struct ALWaveTable_s        *wave;
} ALStartParam;

typedef struct {
    struct ALParam_s    *next;
    s32                 delta;
    s16                 type;
    struct PVoice_s     *pvoice;
} ALFreeParam;

typedef Acmd *(*ALCmdHandler)(void *, s16 *, s32, s32, Acmd *);
typedef s32   (*ALSetParam)(void *, s32, void *);

#ifdef TARGET_N64
#define AL_PARAM_INLINE static
#else
#define AL_PARAM_INLINE static inline
#endif

AL_PARAM_INLINE void *alParamFromS32(s32 value)
{
    return (void *)(uintptr_t)(u32)value;
}

AL_PARAM_INLINE s32 alParamToS32(void *param)
{
    return (s32)(u32)(uintptr_t)param;
}

AL_PARAM_INLINE void *alParamFromF32Bits(f32 value)
{
    union {
        f32 f;
        u32 u;
    } data;

    data.f = value;
    return (void *)(uintptr_t)data.u;
}

AL_PARAM_INLINE f32 alParamToF32Bits(void *param)
{
    union {
        f32 f;
        u32 u;
    } data;

    data.u = (u32)(uintptr_t)param;
    return data.f;
}

#undef AL_PARAM_INLINE

typedef struct ALFilter_s {
    struct ALFilter_s   *source;
    ALCmdHandler        handler;
    ALSetParam          setParam;
    s16                 inp;
    s16                 outp;
    s32                 type;
} ALFilter;

void    alFilterNew(ALFilter *f, ALCmdHandler h, ALSetParam s, s32 type);

#define AL_MAX_ADPCM_STATES     3
typedef struct {
    ALFilter                    filter;
    ADPCM_STATE                 *state;
    ADPCM_STATE                 *lstate;
    ALRawLoop                   loop;
    struct ALWaveTable_s        *table;
    s32                         bookSize;
    ALDMAproc                   dma;
    void                        *dmaState;
    s32                         sample;
    s32                         lastsam;
    s32                         first;
#ifdef NATIVE_PORT
    intptr_t                    memin;
#else
    s32                         memin;
#endif
} ALLoadFilter;

void    alLoadNew(ALLoadFilter *f, ALDMANew dma, ALHeap *hp);
Acmd    *alAdpcmPull(void *f, s16 *outp, s32 byteCount, s32 sampleOffset, Acmd *p);
Acmd    *alRaw16Pull(void *f, s16 *outp, s32 byteCount, s32 sampleOffset, Acmd *p);
s32     alLoadParam(void *filter, s32 paramID, void *param);

typedef struct ALResampler_s {
    ALFilter            filter;
    RESAMPLE_STATE      *state;
    f32                 ratio;
    s32			upitch;
    f32		        delta;
    s32			first;
    ALParam		*ctrlList;
    ALParam		*ctrlTail;
    s32                 motion;
} ALResampler;

typedef struct {
    s16		        fc;
    s16		        fgain;
    union {
        s16		fccoef[16];
        s64             force_aligned;
    } fcvec;
    POLEF_STATE		*fstate;
    s32			first;
} ALLowPass;

typedef struct {
    u32		input;
    u32		output;
    s16		ffcoef;
    s16		fbcoef;
    s16		gain;
    f32		rsinc;
    f32		rsval;
    s32		rsdelta;
    f32		rsgain;
    ALLowPass	*lp;
    ALResampler	*rs;
} ALDelay;

typedef s32   (*ALSetFXParam)(void *, s32, void *);
typedef struct {
    struct ALFilter_s   filter;
    s16			*base;
    s16			*input;
    u32			length;
    ALDelay		*delay;
    u8			section_count;
    ALSetFXParam        paramHdl;
} ALFx;

void    alFxNew(ALFx *r, ALSynConfig *c, ALHeap *hp);
Acmd    *alFxPull(void *f, s16 *outp, s32 out, s32 sampleOffset, Acmd *p);
s32     alFxParam(void *filter, s32 paramID, void *param);
s32     alFxParamHdl(void *filter, s32 paramID, void *param);

#define AL_MAX_MAIN_BUS_SOURCES       1
typedef struct ALMainBus_s {
    ALFilter            filter;
    s32                 sourceCount;
    s32                 maxSources;
    ALFilter            **sources;
} ALMainBus;

void    alMainBusNew(ALMainBus *m, void *ptr, s32 len);
Acmd    *alMainBusPull(void *f, s16 *outp, s32 outCount, s32 sampleOffset, Acmd *p);
s32     alMainBusParam(void *filter, s32 paramID, void *param);

#define AL_MAX_AUX_BUS_SOURCES       8
#define AL_MAX_AUX_BUS_FX	     1
typedef struct ALAuxBus_s {
    ALFilter            filter;
    s32                 sourceCount;
    s32                 maxSources;
    ALFilter            **sources;
    ALFx		fx[AL_MAX_AUX_BUS_FX];
} ALAuxBus;

void    alAuxBusNew(ALAuxBus *m, void *ptr, s32 len);
Acmd    *alAuxBusPull(void *f, s16 *outp, s32 outCount, s32 sampleOffset, Acmd *p);
s32     alAuxBusParam(void *filter, s32 paramID, void *param);

void    alResampleNew(ALResampler *r, ALHeap *hp);
Acmd    *alResamplePull(void *f, s16 *outp, s32 out, s32 sampleOffset, Acmd *p);
s32     alResampleParam(void *f, s32 paramID, void *param);

typedef struct ALSave_s {
    ALFilter            filter;
#ifdef NATIVE_PORT
    intptr_t            dramout;
#else
    s32	       		dramout;
#endif
    s32                 first;
} ALSave;

void    alSaveNew(ALSave *r);
Acmd    *alSavePull(void *f, s16 *outp, s32 outCount, s32 sampleOffset, Acmd *p);
s32     alSaveParam(void *f, s32 paramID, void *param);

typedef struct ALEnvMixer_s {
    ALFilter            filter;
    ENVMIX_STATE	*state;
    s16		        pan;
    s16		        volume;
    s16		        cvolL; // 0x1c
    s16		        cvolR; // 0x1e
    s16		        dryamt; // 0x20
    s16		        wetamt; // 0x22
    u16                 lratl;  // 0x24
    s16                 lratm; // 0x26
    s16                 ltgt; // 0x28
    u16                 rratl; // 0x2a
    s16                 rratm; // 0x2c
    s16                 rtgt; // 0x2e
    s32                 delta; // 0x30
    s32                 segEnd; // 0x34
    s32			first; // 0x38
    ALParam		*ctrlList; // 0x3c
    ALParam		*ctrlTail; // 0x40
    ALFilter            **sources; // 0x44
    s32                 motion; // 0x48
} ALEnvMixer;

void    alEnvmixerNew(ALEnvMixer *e, ALHeap *hp);
Acmd    *alEnvmixerPull(void *f, s16 *outp, s32 out, s32 sampleOffset, Acmd *p);
s32     alEnvmixerParam(void *filter, s32 paramID, void *param);

typedef struct {
    s32         magic;
    s32         size;
    u8          *file;
    s32         line;
    s32         count;
    s32         pad0;
    s32         pad1;
    s32         pad2;
} HeapInfo;

#define AL_CACHE_ALIGN  15

typedef struct PVoice_s {
    ALLink               node;
    struct ALVoice_s    *vvoice;
    ALFilter            *channelKnob;
    ALLoadFilter        decoder;
    ALResampler         resampler;
    ALEnvMixer		envmixer;
    s32                 offset;
    /* DKR: index of the aux bus this voice's envmixer is currently a source
     * of, so func_80065A80() can re-parent it. Zero-initialised by the audio
     * heap, which matches the bus every voice is wired to at construction. */
    u8                  unkDC;
} PVoice;

ALParam         *__allocParam(void);
void            __freeParam(ALParam *param);
void            _freePVoice(ALSynth *drvr, PVoice *pvoice);
void            _collectPVoices(ALSynth *drvr);

s32             _timeToSamples(ALSynth *ALSynth, s32 micros);
ALMicroTime     _samplesToTime(ALSynth *synth, s32 samples);

#endif
