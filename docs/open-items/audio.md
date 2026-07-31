# Open items — Audio

> Part of the split [open-items index](README.md). Every defect this port has
> hit is recorded with its mechanism, the measurement that found it, the fix, and
> the check that would catch a regression. **Nothing is deleted when it is
> fixed** — a closed entry is the only warning the next person gets that the same
> trap exists, and retractions are recorded in place rather than removed.


## FIXED: RAW16 bass was decoded as host-endian noise — wave "raw16"

**Report (playtest log, ordinary play):** the bass sounded garbled, buzzy, or like the
wrong MIDI instrument.

### Mechanism

The ROM stores uncompressed RAW16 waves as big-endian signed PCM. The native
software mixer consumes host-representation `s16`, but all three load sites in
`alRaw16Pull()` used the ordinary byte-copying `aLoadBuffer()`. On little-endian
native and wasm builds, each sample therefore arrived byte-reversed.

The fourth nearby `aLoadBuffer()` is in `_decodeChunk()` and carries compressed
ADPCM bytes. It must remain a byte copy. Treating all four alike would fix the
minority RAW16 format by corrupting the 651 unique ADPCM waves.

This escaped the broad audio gate because the broken capture still had plausible
RMS, stereo, tempo, clipping, and reverb. Those measurements prove that synthesis
is active and timed; they cannot prove that an instrument waveform has the right
timbre. The pre-fix binary passed `check_audio_output.py`.

### Content and signal evidence

`tests/check_raw16_audio.py` independently parses the serialized US/PAL v80 bank
layout with bounds checks and keeps no ROM-derived output:

| bank | instruments | sound references | unique waves | encoding |
|---|---:|---:|---:|---|
| music | 128 | 297 | 233 | 208 ADPCM + **25 RAW16** |
| SFX | 1 | 784 | 444 | 443 ADPCM + **1 RAW16** |

The principal 18,432-byte bass sample has normalized roughness **0.05179** when
read as big-endian PCM and **1.41581** under the old interpretation: **27.34x**
rougher. The extra 150-byte SFX RAW16 wave was absent from the original handoff's
music-only count and is now part of the required census.

### Fix

Commit `a651cdc` gives the three RAW16 sites a native-port-only
`aLoadRaw16Buffer()` mapping backed by `mdkr_copy_be16_to_host()`. Matching N64
builds retain their original calls, and the ADPCM load is unchanged.

The endian helper constructs values from serialized bytes rather than asking
whether the current host should swap. That makes it defined for unaligned input
and correct on both little- and big-endian CPUs. Literal `bswap16`/`bswap32`
helpers remain unconditional reversal operations; `read_be*` and `GE_SWAP*`
now express data conversion instead.

`MDKR_RAW16=fixed|legacy` selects production behavior or the exact former
byte interpretation in one binary. It is a test seam, not a compatibility
recommendation: `fixed` is the default, and invalid values warn and use `fixed`.

### Both-direction gate

The same 4,300-frame route runs both modes. It requires:

- exactly three converted source call sites and one untouched ADPCM call;
- the complete bank census above;
- equal **9,441-load / 676,240-byte** traces and the same first RAW16 block;
- a bit-identical PCM prefix before that block;
- divergence soon afterward, with 17.68% of post-boundary samples changed,
  difference RMS 1,280.5, and a legacy block at least 4x rougher (measured
  **8.51x**).

If the conversion disappears, the fixed and legacy captures become identical
and the test fails. If the route stops reaching RAW16, the nonzero load and
boundary assertions fail. Debug, Release, and ASan pass; the broad audio check
still passes after the repair. The wasm build compiles the same path, and actual
Chromium reaches it through the AudioWorklet with `mode=fixed`, **11 loads / 816
bytes** in the first active block.

`tests/test_endian_utils.c` separately checks aligned/misaligned integer and
floating readers, `GE_SWAP*`, raw reversal, PCM conversion, invalid sizes and
pointers, and transactional no-write-on-error behavior. A real big-endian
runtime was not available, so executing there remains matrix debt even though
the conversion itself is host-neutral by construction.

## FIXED: browser audio event-queue drops — measured in the real runtime

Also present in that player's console, before the crash and then rate-limited:
`[EVTQ] post DROPPED (free list empty) q=0x100d0b38 type=2` plus one `type=9`.

**Original verdict:** a real defect, not benign — but unrelated to the crash above,
and no longer able to hang the game. Evidence:

- Type numbers decode via `enum ALMsg` (`game/include/PR/libaudio.h`) to
  `AL_SEQP_MIDI_EVT` (2) and `AL_SEQP_API_EVT` (9). The second is the sequence
  player's **self-perpetuating keepalive** — precisely the event whose loss used to
  break the player chain, drain the queue and spin forever on the empty-queue
  sentinel. That path is hardened (`__CSPVoiceHandler` re-arms
  `AL_SEQP_API_EVT` and returns a nonzero delta), so a drop now costs audio, not the
  process.
- A drop still means audio was genuinely lost, and per `game/src/audio.c` it
  "silences that player's music until the next music change".
- **It did not reproduce in the old native control.** A 3000-frame muted attract
  run with `MDKR_EVTQ_STATS=1` recorded 0 drops and peaks 58/256 (music), 3/50
  (jingle), and 12/150 (SFX). That control uses the deterministic headless pump,
  not a live sink.

**The missing measurement now exists.** `tests/check_browser_runtime.py` drives
the shipped shell and actual wasm/WebGPU build in Chrome with the real
AudioWorklet active. With the stock 150-entry SFX budget:

- three passing runs reached SFX peaks 139, 141, and 145;
- a fourth run emitted `[EVTQ] post DROPPED` and failed the gate;
- music stayed at 74–75/256 and jingle at 3/50.

Removing the truncating SFX cap exposed live peaks of 162, 175, and finally 195.
The variation comes from the live sink's occupancy-controlled synthesis batches;
the fixed headless pump masks that runtime pressure. The original anonymous
type-2/type-9 sequence-player report did not recur on current head, so it is not
retrospectively reattributed. It is covered now: any drop from any queue fails.

**Fix and permanent gate.** The native-port SFX queue is 512 entries, 2.62× the
largest observed live demand. Every browser release run enables queue telemetry
and requires all of the following:

- budgets exactly 256 music / 50 jingle / 512 SFX;
- no `[EVTQ] post DROPPED` marker from any queue;
- every measured peak at or below half its capacity;
- a forced one-entry SFX arm must emit the structured drop diagnostic, proving
  the failure direction.

The diagnostic now includes queue index/address, event type, peak, capacity, and
total drops. A final production run measured 74/256, 3/50, and 175/512 with zero
drops while the AudioWorklet consumed about 1.32 million sample-frames. The same
route under the native fixed pump measured 71/256, 3/50, and 19/512. The full
143.5-second audio-content/tempo/reverb check remains green.

## M5 audio — DONE (music + SFX via the aspMain software mixer)

> **Provenance, since this section predates the clean-room swap.** The
> synthesiser described below was the decompiled SGI one. It has since been
> **deleted** and replaced by the first-party clean-room engine in
> `platform/audio_compat.c` / `audio_event_queue.c` / `audio_fx_transfer.c`,
> extended with DKR-specific behaviour. The replacement measured within 0.5 dB
> of the pre-swap baseline (spectral cosine 1.000). The wiring described here
> — macro override, per-frame pump, host sink, and the LP64 addressing model —
> is unchanged and still current. See
> [`docs/architecture/audio.md`](../architecture/audio.md) for current state
> and [NOTICE.md](../../NOTICE.md) / [THIRD_PARTY.md](../../THIRD_PARTY.md)
> for canonical provenance.

The synthesiser runs on the host; its
abi.h `a*` command macros are redefined by platform/mixer.h to call the vendored
software mixer (platform/mixer.c) directly, so "building the Acmd list" IS the
synthesis. Wiring:
- **Macro override ordering:** `#include "mixer.h"` at the tail of PR/abi.h under
  NATIVE_PORT (1 edit) guarantees the `#undef`+redefine of the `a*` macros wins in
  every synth TU regardless of its include order — cleaner than a `-include` (which
  lands BEFORE the TU's own `<libaudio.h>` → `<mbi.h>` → `<abi.h>` re-defines them).
- **Per-frame pump:** the audio thread (__amMain) never runs in the cooperative
  model, so amAudioSynthFrame() (audiomgr.c) reproduces __amHandleFrameMsg's core
  (__clearAudioDMA + alAudioFrame straight into an arena PCM buffer) and
  dkr_audio_pump() (platform/audi_port_dkr.c) drives it once per rendered frame
  from the stubs_dkr.c video-queue frame boundary (right after
  platform_frame_sync, so it doesn't inflate the paced frame time). Frame sizing:
  fixed frameSize headless (deterministic dump), host-counter occupancy controller
  windowed.
- **Host out:** SDL2 queue-mode device @ OUTPUT_RATE(22050) stereo s16
  (audi_port_dkr.c); osAiSetFrequency/osAiGetLength/osAiSetNextBuffer moved there
  from stubs_dkr.c. The SDL device is skipped under --headless-frames (synthesis
  still runs — CI exercises the DSP); MDKR_AUDIO_DUMP=out.wav captures the PCM,
  MDKR_AUDIO_RMS=1 prints running RMS/peak. Production is decoupled from the sink
  (M8 web AudioWorklet drop-in).
- **THE LP64 ADDRESS PROBLEM + fix.** The synth ABI is 32-bit throughout:
  osVirtualToPhysical→u32 (envelope/resample/reverb), the ALDMAproc
  return→s32 (audiomgr __amDMA), ALSave.dramout→s32, K0_TO_PHYS→29-bit
  mask. Every buffer/state/sample address reaches the mixer truncated (and
  the s32 ones sign-extended). Fix = make ALL audio memory arena-resident (the
  audio heap is arena-backed; banks parse into the arena; output/cmd buffers from
  the arena) and reconstruct every mixer address from its low-32 via
  dkr_lo32_to_ptr() (platform/mixer.h MIXER_RESOLVE) — exact because the arena is
  size-aligned. K0_TO_PHYS's `& 0x1FFFFFFF` mask had to be dropped for the audio
  path (load.c, NATIVE_PORT) — it stripped bits 29-31 of the arena base's low-32
  that the reconstruction needs.
- **ALParam free-list LP64 size lock.** __allocParam() hands out sizeof(ALParam)
  blocks that callers reinterpret as ALStartParam/ALStartParamAlt (libaudio's
  "alternate view of one allocation" idiom). ALStartParamAlt embeds two 8-byte
  pointers → 40 bytes here vs ALParam's 32; alSynStartVoiceParams writing
  update->wave overran the block and clobbered the next pooled param's `next`,
  derailing __allocParam. Fixed by padding ALParam (synthInternals.h) to cover the
  largest variant, locked with _Static_assert.
- **Objective evidence:** headless PCM dump — race RMS ~6800 (per-second segments
  ranging 3000-12000, i.e. dynamic real audio, not a tone; peak full-scale),
  menu-nav RMS ~4300. ASan-clean over a menu+race synth run. race_drive 20x +
  title 300f = 0 crashes. The robustness-sweep sound-handle truncation fixes hold
  now that handles are non-NULL (sndp_stop derefs a live handle every race).

### M5 open items
- [x] **Native reverb (ALFx delay lines) — CLOSED, ON BY DEFAULT.**
  (`MDKR_AUDIO_REVERB=0` now *disables* it, for A/B captures.)

  **Root cause was NOT the struct growth / pool layout** (the earlier hypothesis
  above; it was wrong). It is a single LP64 pointer-arithmetic bug in the delay
  taps, in the reverb delay-line code (then the decompiled `reverb.c`, now
  `platform/audio_fx_transfer.c`):

  ```c
  in_ptr  = &r->input[-d->input];      /* ALDelay.input/.output are u32 */
  ```

  `-d->input` is an **unsigned** 32-bit value (2^32 − N). On the N64 (ILP32) that
  added to a 32-bit pointer wraps mod 2^32 and lands exactly N samples *below*
  `r->input` — deliberately, possibly below `r->base`, which `_loadBuffer` /
  `_saveBuffer` then correct with `if (curr_ptr < r->base) curr_ptr += r->length`.
  On LP64 the array subscript **zero-extends** the u32, so the tap lands ~8 GiB
  *above* `r->input`. Measured (`base=0xd121408d0`, `r->input` at offset 0,
  `d->input`=160): `in_ptr` at delta **+4294967136 samples**. Consequences:
  * the below-base correction never fires (`curr_ptr < r->base` is false);
  * `updated_ptr > delay_end` is *always* true, so both helpers always take the
    wrap branch, and the two split lengths come out as
    `before_end = length − off + tap` and `after_end = off + count − length − tap`
    (each truncated to s32) — i.e. **`before_end` instead of `count`**. Measured:
    `count=160 → before_end=6400`, an aSetBuffer of **12800 bytes** where 320 was
    wanted (and a negative `after_end`, which `(u16)` wraps to garbage);
  * the DRAM *address* is fine — `osVirtualToPhysical` truncates it to 32 bits and
    `dkr_lo32_to_ptr` rebuilds the correct in-arena pointer, i.e. the truncation
    restores the modular arithmetic. So the write **starts in the right place** and
    then runs thousands of samples off the end of `r->base` into whatever mempool
    allocated next — which is exactly the `ALLowPass`/`ALResampler` structs that
    `alFxNew` allocates immediately after the delay line (observed: bus 0's
    `delay_end` == `d->lp` for section 1, byte-identical addresses). `_filterBuffer`
    then dereferences the smashed `d->lp` → SIGSEGV, which is what the watchpoint
    saw as "4 s16 sample values written into the slot".

  **Fix** (`reverb.c`, `AL_FX_TAP()`): make the tap offset signed before negating,
  so the subscript sign-extends and reproduces the N64 result exactly. All four
  tap sites go through the macro. (`prev_out_ptr = &r->input[d->output]` is left
  alone — that subscript is positive on the N64 too; a stock-libultra quirk that
  only feeds a buffer-reuse optimisation.)

  **Two hardening changes shipped with it**, so this class is loud next time:
  * `AL_FX_TAIL_SLACK` (synthInternals.h, 4 samples = 8 bytes) is added to the
    `r->base` allocation in `alFxNew`. The software mixer emulates the RSP's
    8-byte DMA granularity (`platform/mixer.c` `ROUND_UP_8(sb_count)`), and
    `_loadBuffer`/`_saveBuffer` legitimately issue transfers that end *exactly* at
    `&r->base[r->length]`, plus the chorus path issues odd `count + ramalign`
    loads — so a non-multiple-of-8 byte count could touch up to 6 bytes past the
    allocation even with correct taps. `_Static_assert` locks the slack ≥ 8 bytes.
  * `_fxClampXfer()` bounds-guards **every** DRAM side of a delay-line transfer
    against `[r->base, r->base + length + slack]`, prints one loud `[FX BUG]` line
    and clamps rather than corrupting. `alFxGuardTrips()` is printed as
    `[AUDIO] fx-guard trips=N` under `MDKR_AUDIO_RMS=1`; **N must be 0**. It
    earned its keep immediately: it caught a 4th tap site (`_loadOutputBuffer`'s
    non-chorus branch) that the first pass missed. ASan cannot do this job — the
    arena is one host allocation, so an intra-arena overrun has no redzone.

  **Evidence the wet path is really contributing** (deterministic headless
  captures, `MDKR_AUDIO_REVERB=0` vs default, same fixture/frame count):
  * *Energy where the dry run is bit-exactly zero.* 12000-frame attract soak:
    2 047 843 samples (23.19 % of the run) are exactly 0 with reverb off; with
    reverb on the same instants carry **rms 147.8, peak 9125**. 4300-frame race:
    147 712 dry-zero samples → **rms 73.5, peak 1077** wet.
  * *A measured decay curve.* Through a 4.7 s stretch (soak @201.3 s) where the
    reverb-off capture is bit-exactly silent, the reverb-on envelope (256-sample
    windows) reads
    `1093 → 1239 → 786 → 843 → 774 → 687 → 485 → 495 → 422 → 440 → 374 → 299 →
     325 → 241 → 254 → 214` over 348 ms — a clean ~−14 dB exponential tail
    (RT60 ≈ 1.5 s) against an exactly-zero dry reference.
  * *System identification.* Normalised cross-correlation of `(wet − dry)` against
    `dry` over the 4300-frame race: of all lags in 0..6400, the twelve largest by
    |value| are **{0,1,2,3,4,5,6, 159,160, 320,321,322}** — lag 0 is the
    cancellation of the raw aux send (−0.531, 26× the background rms) and 160/320
    are literally bus 0's first two configured delay taps (−0.146 / +0.146, 7.2×
    background). Nothing else in the range is close.

  **No pool overrun** (evidence): `fx-guard trips=0` across the whole regression
  matrix (204 runs incl. the 12000-frame multi-level soak). Independent temporary
  canary: `r->base` over-allocated by 4096 bytes filled with 0xA5 and verified
  after every `alFxPull`, **with the clamp guard disabled** so the canary tested
  the tap math and not the clamp — intact through the 12000-frame soak and the
  4300-frame race. Positive controls, same build: reverting one tap to
  `&r->input[-d->input]` gives `fx-guard trips=214950` with the guard on, and
  SIGSEGV in `_filterBuffer` with it off (the originally reported crash).
- [x] **Output level / clipping — CLOSED. A gain stage was genuinely wrong; it
  was the disabled reverb itself.** With `alFXEnabled == FALSE`, `alFxPull`'s
  early return happens *after* the source pull, so the aux bus's voice sum — the
  **wet send** — was left in `AL_AUX_L/R_OUT` and `alMainBusPull` then mixed it
  into the main bus at unity (`mainbus.c:41-42`). Every voice's wet send was thus
  added to the master **undelayed and unattenuated**, on top of its own dry. That
  is not "dry playback", it is a spurious extra bus.

  Measured with `PortMixerStats.mainMix*` (new; counts saturation *on the master
  bus only* and records the worst pre-clamp magnitude). The percentage is over
  master-bus **mix-op** samples — `alMainBusPull` issues one `aMix` per aux
  source per channel, so each emitted sample is touched twice — which makes it a
  clean A/B ratio; the absolute clipping numbers are the railed-sample counts
  from the dumped WAVs below.

  | fixture | reverb OFF: master clips / worst pre-clamp | reverb ON |
  |---|---|---|
  | race_drive_time_trial 4300f | 7028 (0.0555 %) / 50470 = **+3.75 dBFS** | 1587 (0.0125 %) / 38017 = **+1.29 dBFS** |
  | nav_to_character_select 2900f | 13504 (0.1582 %) / 51891 = **+3.99 dBFS** | 1904 (0.0223 %) / 37980 = **+1.28 dBFS** |
  | nav_to_time_trial_race 2900f | 3843 (0.0450 %) / 50470 = **+3.75 dBFS** | 515 (0.0060 %) / 38017 = **+1.29 dBFS** |
  | attract soak 12000f | 28624 (0.0810 %) / 52805 = **+4.14 dBFS** | 6627 (0.0188 %) / 41191 = **+1.99 dBFS** |

  Railed samples in the emitted PCM, 12000-frame soak: **3633 (0.0206 %), longest
  run 20 samples (0.91 ms), 82.4 ms clipped total** → **583 (0.0033 %), longest
  run 12 samples (0.54 ms), 13.2 ms clipped total**. Whole-run RMS 5410 → 4833.
  (The 4300-frame race goes the other way on final-rail count, 8 → 339, while its
  *master-bus clamp events* drop 4.4×: a clamp during accumulation can be pulled
  back below full scale by the second aux source's contribution, so the two
  metrics are not the same thing. Clipped time there is 7.7 ms of 143 s.)

  **No blanket master trim was added, deliberately.** After the gain-stage fix the
  residual is +1.3 … +2.0 dB of overshoot on 0.006–0.022 % of master-bus samples,
  in runs of ≤12 samples. Eliminating it outright needs −2.0 dB
  (32768/41191 = 0.796) applied to 99.997 % of the program to protect 0.003 % of
  it — and the saturation is *hardware-faithful*: `mixerMix` reproduces the RSP's
  saturating accumulate, so an N64 clips the same content the same way. Without an
  audio oracle (no ares lane) a trim would be an unverifiable deviation from the
  reference. The telemetry is now permanent, so the moment an oracle exists the
  comparison is a one-liner: run with `MDKR_AUDIO_RMS=1` and read
  `[AUDIO] mainbus clip: …`.
- [ ] **Title/attract music timing.** In the pure-boot (no-input) run the first
  ~1200 frames are silent (boot logos); the interactive menus + race produce
  audio. Whether the title-screen attract loop should self-start music without
  input is unverified (needs a reference); menu SFX + race audio are confirmed.
