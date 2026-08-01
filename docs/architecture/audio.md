# Audio — the clean-room engine and the software mixer

Music, SFX and reverb are live. This document describes how audio actually
works today; [NOTICE.md](../../NOTICE.md) and
[THIRD_PARTY.md](../../THIRD_PARTY.md) are canonical for provenance and
licensing, and are the documents to trust if this one ever drifts.

## What runs

DKR's audio is synthesised in software on the host. There is no RSP and no
audio thread.

1. **The engine.** `platform/audio_compat.c`, `platform/audio_event_queue.c`
   and `platform/audio_fx_transfer.c` implement the libaudio surface — bank
   and sequence-file parsing, the compact sequence player, the event queue,
   envelope mixing, and the reverb delay lines. This is first-party
   clean-room code, brought over from the sister **mgb64** project and
   extended here with a DKR extensions section (19 functions: the CC7/CC8
   dual channel-volume lanes and their accessors, the reverb gate and
   guard-trip readout, the aux-bus re-parent helper, channel on/off, voice
   limit, and pan modes), plus in-engine adaptations for DKR's `fxType[2]`
   dual aux buses, the `CSP_CHAN()` channel-state stride, and an
   arena-resident `alHeapDBAlloc`.

2. **The mixer.** The engine builds an `Acmd` list using libultra's `abi.h`
   command macros (`aADPCMdec`, `aEnvMixer`, `aResample`, `aSetBuffer`,
   `aMix`, …). `platform/mixer.h` `#undef`s those macros and redefines each to
   call the software RSP audio-microcode emulator in `platform/mixer.c`
   directly (`mixerADPCMdec`, `mixerEnvMixer`, …). So on this port "building
   the Acmd list" **is** "executing the synthesis" — there is no separate Acmd
   interpreter. `platform/mixer.c` is adapted from the perfect_dark port; DKR,
   GoldenEye and Perfect Dark all share the libultra N-Audio ABI.

3. **Addressing.** The synth ABI truncates every address to 32 bits
   (`osVirtualToPhysical` → `u32`, the `ALDMAproc` return → `s32`,
   `ALSave.dramout` → `(s32)param`). All audio memory is therefore
   arena-resident, and `MIXER_RESOLVE` / `dkr_lo32_to_ptr` reconstructs each
   mixer address from its low 32 bits — the same pattern the graphics path
   uses.

4. **The pump and the sink.** An independently due synchronous pump
   (`platform/audi_port_dkr.c` plus `amAudioSynthFrame`) replaces the
   never-run audio thread. Exact host-audio time accrues in two-field quanta;
   each ordered game tick consumes at most one, so extra presentation frames
   cannot manufacture PCM and catch-up retains cue order. Native output is an
   SDL2 queue-mode device at 22050 Hz stereo s16, gated off under
   `--headless-frames`. `platform/audio_queue_controller.c` replaces the
   measured sink drain and corrects the application queue toward a bounded
   target; the same pure controller drives deterministic cadence tests and a
   ROM-free SDL sink probe. The browser build swaps the sink for
   `web_audio_worklet.c`; sample production is decoupled from the sink so both
   reuse it.

## Sink qualification

Two ROM-free CTests separate deterministic control from operating-system
plumbing. `audio_queue_controller` simulates exact 30, 60, 120, 144, 240 and
1000 Hz service schedules plus counter wrap, capacity/alignment, and a long
host stall. `audio_sink_contract` opens the real SDL queue API with silence,
requires the exact 22050 Hz/stereo/s16 contract, observes active drain, and
checks bounded occupancy plus pause and clear behavior. CI selects SDL's
silent `dummy` driver. The same executable can exercise a physical default
device without copyrighted PCM or audible output:

```bash
env -u SDL_AUDIODRIVER build/mdkr_audio_sink_contract_test --duration-ms 10000
```

The 2026-08-01 CoreAudio witness passed for 5 seconds: 476 controller calls,
214 observed queue drains, zero queue failures or stall guards, and a
1,193-sample-frame application-queue high-water. SDL queue occupancy does not
expose the hidden hardware buffer and silence cannot prove speaker output, so
this is not represented as DAC underrun, audible-output, or cross-platform
hardware qualification. Those remain part of the physical release matrix.

## Provenance: what the engine replaced

Audio originally ran on 49 decompiled SGI-legend synthesiser sources under
`game/libultra/src/audio/`. **Those were deleted** at the clean-room swap
(commit `2a180eb`); nothing derived from the SGI implementation ships. The
engine described above replaced them wholesale.

The swap was validated as a behavioural no-op, not asserted to be one. Against
the pre-swap capture baseline:

| Measure | Result |
|---|---|
| Whole-capture RMS | within **0.27 dB** of baseline |
| Reverb-off RMS | within **0.43 dB** of baseline |
| Spectral cosine similarity | **1.000** |
| Per-band mean absolute error | **0.36 dB** |

Measured by `tools/compare_audio.py` / `tools/compare_audio_reference.py`
(themselves ported from mgb64) via the `check_audio_output` and
`check_raw16_audio` fixtures.

One behavioural difference had to be corrected during that validation and is
worth knowing about: mgb64's `alEnvmixerParam` applies a square-law
perceptual volume curve `(v*v)>>15`; DKR's synthesiser applies volume
**linearly**. Inheriting the curve cost roughly 4 dB overall and, being
quadratic, attenuated quiet voices far more than loud ones. It is now linear
here (commit `cdeb3e5`), which is what closed the gap in the table above.

## Related

- [`docs/MGB64_BACKFLOW.md`](../MGB64_BACKFLOW.md) "M5 audio" — the adaptation
  history and the integration hazards worth back-porting.
- [`docs/open-items/audio.md`](../open-items/audio.md) — the audio defect
  record, including the reverb LP64 delay-tap fix.
- [`tests/README.md`](../../tests/README.md) — `check_audio_output`,
  `check_raw16_audio`, the sink contracts and the rest of the audio fixtures.
