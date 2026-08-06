# Open items — Audio

> One subsystem of the split [open-items index](README.md), which states how these files are kept.

**Currently open** (per [`README.md`](README.md#still-open)'s open/closed
table): none — every entry below is closed or resolved; kept as the
append-only historical record.


## FIXED: an AudioWorklet underrun left the repair envelope out of the loop

The worklet's `process()` loop treated an underrun as an early `continue`: it
wrote silence, incremented `under`, and skipped the rest of the sample's work.
Two invariants lived in the part it skipped.

- `lastL`/`lastR` mean *"the last sample the speaker actually received"*, and they
  are the anchor a later overflow crossfades **from**. Skipping their update
  anchored the repair on a pre-gap sample that had stopped playing several quanta
  earlier, so the repair itself produced the click it exists to remove.
- `this.fade` is the crossfade countdown. Skipping its decrement froze a
  mid-underrun ramp at a partial mix permanently: `completedRecoveries` never
  advanced and the `fade === 0` re-arm never fired again, so the next real gap had
  no recovery at all.

**Fix:** an underrun is now an ordinary sample that happens to be silence. It runs
the same envelope, updates the anchors, and drains the fade; only the read pointer
stands still, because there is nothing to read
(`platform/web_audio_worklet.c`, `webAudioOutputInit`).

Related, on the native side: dropped-audio telemetry conflated two different
quantities into one counter that in fact counted *polls*. `s_droppedBuffers`
(blocks this port refused at `osAiSetNextBuffer`) and `s_droppedFrames` (frames
lost with them) are now separate named quantities on both the native and web
paths (`platform/audi_port_dkr.c`). Sequence tempo meta events are validated
rather than truncated to 24 bits (`platform/audio_compat.c`).

## FIXED: audio table indices and the vehicle-sound teardown reached recycled arena memory

The game-core memory-safety wave closed a set of audio boundaries that shared one
shape — an index or a walk trusted past the live count:

- **Vehicle sound teardown left `spinoutSound` attached.** Teardown now stops and
  detaches every handle, so a finished sound can no longer write through a stale
  `userHandle` into arena memory the allocator has already recycled.
- Every table index that reaches `gSoundTable`, `gSpatialSoundTable` or the
  composite chain is bounded at use.
- The delayed-sound queue's compaction shift is corrected.
- The tempo `-1` sentinel wrap loop is guarded.
- The reverb segment walk is bounded.
- `audspat_point_stop()` no longer matches stale entries above the live count.

Alongside them, colour-cycle lights stopped reading one past their frame table and
stopped dividing by an out-of-bounds byte, and ambient lights write a defined light
direction for shaded objects.

## PARTIALLY CLOSED: native SDL sink had no ROM-free live qualification

Every game-audio check deliberately ran with `--headless-frames` and
`MDKR_AUDIO=0`. That is the correct copyright/CI boundary and fully exercises
synthesis, but it left the native queue plumbing inferred: no automated test
opened the SDL device, observed its queue drain, or proved pause and clear.
The queue-size controller was private to `audi_port_dkr.c`, so a unit test could
only have copied the algorithm instead of qualifying production code.

The controller now lives in `platform/audio_queue_controller.c` and the game,
browser worklet path, deterministic unit, and SDL probe all call that one
implementation. The pure `audio_queue_controller` CTest simulates 20 seconds
at each of 30, 60, 120, 144, 240 and 1000 Hz; requires exact production/drain
conservation and a queue below three target blocks; and covers counter wrap,
16-sample alignment, synth capacity, zero counter rate, first refill and a
one-second stall guard. Headless calls remain host-time independent.

`audio_sink_contract` then opens SDL queue mode using silence—not ROM PCM—with
the production 22050 Hz/stereo/s16 request. CI pins `SDL_AUDIODRIVER=dummy` and
requires exact obtained format, active drain, no enqueue failure or stall
guard, bounded queue occupancy, pause stability and explicit clear. The dummy
driver is no longer only an environment pin: on native, `dkr_audio_out_init()`
reads `SDL_HINT_AUDIODRIVER` and refuses the `MDKR_TEST_HEADLESS_AUDIO=1` opt-in
unless the driver really is `dummy`, saying so on stderr, so a real device can no
longer be opened under `--headless-frames` while looking identical in the log. The same
binary passed for five seconds against the workstation's physical CoreAudio
default: 476 controller calls, 214 observed application-queue drains, and a
1,193-sample-frame high-water. A 1,000-tick pre/post extraction headless PCM
capture also remained byte-identical (`bf2c44b9...97b7f1`).

This closes the production-controller and available-macOS SDL plumbing gap.
It does **not** close the hidden device-buffer half: SDL exposes application
queue bytes, not hardware underrun counters, and silence cannot prove speaker
output or DAC drift. Audible or loopback game-audio qualification on macOS,
Windows and Linux therefore remains in the physical release matrix.

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
- **Independent audio-time service:** the audio thread (__amMain) never runs in the cooperative
  model, so amAudioSynthFrame() (audiomgr.c) reproduces __amHandleFrameMsg's core
  (__clearAudioDMA + alAudioFrame straight into an arena PCM buffer) and
  dkr_audio_service_tick() (platform/audi_port_dkr.c) consumes at most one due
  quantum after each ordered game tick. Host time is credited at real presentation
  boundaries, so presentation count cannot create PCM. Frame sizing:
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

## MEASURED: absolute output level vs the real ROM — port/console is +0.016 dB

**The gap this closes.** Every audio gate in this tree was scale-blind.
`check_audio_output.py` asserts a *floor* on RMS and a *ceiling* on saturation, so
anything between "clearly alive" and "clearly destroyed" passed; its tempo, stereo and
spectral-change assertions are ratios and correlations, which a flat gain leaves
exactly unchanged. `check_raw16_audio.py` compares two arms of the same build.
`[EVTQ]` telemetry counts events; the resource-plateau `voicePeak` law counts voices.
The clean-room engine's "within 0.5 dB of the pre-swap baseline" was a **port-vs-port**
number. Absolute level against the real ROM had never been measured, so a systematic
loudness bias — one wrong shift, an extra `/2`, a master trim added to stop the
clipping — would have passed the entire suite.

### A console reference now exists

The ares oracle (`docs/ORACLE.md`) grew an audio lane. `MDKR64_ARES_AUDIO_DUMP` taps
`AI::sample()` in the instrumented ares, i.e. the exact s16 word the ROM DMA'd to the
audio interface — upstream of ares' float conversion, upstream of the analogue-hold
decay on the idle branch, and upstream of every host resampler and output driver. It
is the real ROM's own synthesiser output at full scale, and it is deterministic for a
given input route. Only DMA-active samples are written; the idle decay is an
output-stage artefact and including it would bias exactly the measurement the lane
exists for. The rate comes out of `AI_DACRATE` (22047 Hz, not the 22050 the ROM asks
for) and is written to a sidecar rather than assumed.

Both runners were driven down the **same** oracle route, `race_state_oracle` (title →
menus → Ancient Lake, one player), the port on that route's own native arm
(`original` cadence, 2 synth fields, 4800 frames), ares for 9505 presented frames.

The refreshed local-only console capture contains **3 501 171** stereo frames at
22047 Hz (158.80 s), SHA-256
`2da39cc3a93e2038e09a88ab874f8d2844a971a7cd196a2182a5c2cdfe125b0e`.
The raw capture itself remains uncommitted.

Envelope-aligned (lag −5.65 s, correlation **+0.7816**) over a **153.16 s** overlap:

```
RMS console 8303.7 (−11.924 dBFS)   port 8318.6 (−11.908 dBFS)
port / console = +0.016 dB
```

Per band, port minus console:

| band | console | port | delta |
|---|---:|---:|---:|
| 0–100 Hz | −15.686 | −15.703 | −0.017 |
| 100–200 Hz | −17.984 | −18.094 | −0.110 |
| 200–400 Hz | −21.174 | −21.485 | −0.311 |
| 400–800 Hz | −21.565 | −21.452 | +0.112 |
| 800–1600 Hz | −22.542 | −23.079 | −0.537 |
| 1600–3200 Hz | −27.033 | −26.615 | +0.417 |
| 3200–6400 Hz | −29.582 | −28.573 | +1.010 |
| 6400–11025 Hz | −36.488 | −33.063 | **+3.425** |

**Verdict: there is no systematic loudness bias.** Broadband level agrees within
0.016 dB after the restored vehicle path. The residual per-band differences are
small except for a +3.425 dB top-band tilt at roughly −33 dBFS. Because the route
is envelope-aligned rather than sample-locked, that remains an observation rather
than evidence for a gain-stage defect.

### Gain-staging audit — every stage checked against the SGI reference, zero discrepancies

Done independently of the measurement, against the decompiled libultra audio sources
and the DKR decomp, stage by stage. Nothing was copied; this is a numeric equivalence
audit.

| stage | port | reference | verdict |
|---|---|---|---|
| pan / fx-mix law | `native_eqpower_at()` computes `cos(πi/254)·32767` | `env.c`'s 128-entry `eqpower[]` literal | **all 128 entries bit-identical**, max abs diff 0 |
| per-voice note gain | `__vsVol`: `(tremelo·velocity·envGain)>>6`, `(sampleVolume·seqp->vol·chanVol)>>14`, product `>>15` | `seqplayer.c __vsVol` | identical shift-for-shift |
| envelope volume map | applied **linearly** | `fVol = (fVol+fVol)/2` — the vestigial identity | unity either way |
| envelope targets | `(volume·eqpower[pan])>>15`, mirrored index for R | `env.c:146-148, 175-177, 395-399` | identical |
| dry/wet split | `eqpower[fxMix]` / `eqpower[127−fxMix]` | `env.c:135-136, 224-225` | identical |
| envelope rate | `native_env_rate`: `((tgt−vol)/count)·8`, truncate, low word from the fraction | `_getRate` | same result; port uses `f32` where SGI used `f64`, a sub-LSB rate difference, not a level one |
| aux bus | clear + sum sources, no gain | `alAuxBusPull` | identical |
| **master bus** | `aMix(0x7fff, AUX_L→MAIN_L)` and R, per source | `mainbus.c:41-42` | identical |
| reverb input fold | `aMix(0xda83, AUX_L→in)`, `aMix(0x5a82, AUX_R→in)` | `reverb.c:95-96` | byte-identical constants |
| reverb coefficients | `ffcoef`/`fbcoef`/`gain` from the ROM's `AL_FX_CUSTOM` parameter asset (`ASSET_AUDIO_8`) | same asset, same `fxType[0]` | the port carries **no** built-in preset table to get wrong |
| RSP op emulation | envmixer `dst += (in·gain)>>15` with `gain = (vol·voldry + 0x4000)>>15`; `aMix` = `((out·0x7fff + in·gain) + 0x4000)>>15`; resampler `(in·tbl + 0x4000)>>15`; pole filter Q2.14 | aspMain | standard forms |
| final s16 | `mixerSaveBuffer` is a `memcpy`; the dump/sink copy verbatim | `alSavePull` → `aInterleave` + `aSaveBuffer`, no scaling | unity |

**No `/2`, `>>1`, or float scale exists anywhere in the port's chain that the reference
does not also have.** The only non-unity factor found is `aMix`'s `0x7fff/0x8000` on
the destination — **−0.000265 dB** per pass, applied twice on the master bus, i.e.
−0.00053 dB total. It is faithful: the RSP does the same thing.

The earlier −0.49 dB result was superseded when the retail vehicle-audio path was
restored. The refreshed +0.016 dB result, together with the stage audit, leaves no
evidence of a gain-stage error; the remaining differences are consistent with two
implementations observing nearby rather than sample-locked program material.

### The gate: `tests/check_audio_level_reference.py`

Registered in `tools/run_checks.py` as `audio_level_reference`; full assertion table in
[`tests/README.md`](../../tests/README.md). Frozen port-side baseline on the
`race_drive_time_trial.txt` 4300-frame route (3 164 064 sample-frames, 143.49 s):

- whole RMS 7457.9 = **−12.857 dBFS**; L −12.857, R −12.856
- sample peak 32768 (0.000 dBFS), **crest 12.857 dB**, rails 0.07248 %
- engine `mainbus clip` 4578/6329600 = 0.07233 %, worst pre-clamp 35951 = **+0.81 dBFS**
- true peak (4x oversampled) **L +1.002 / R +1.478 dBFS**
- 8 absolute band RMS values and 15 ten-second slice RMS values, all recorded

Sample peak is deliberately part of the frozen set, pinned at 0.000 dBFS within
0.01 dB, with the crest factor reported beside it rather than bounded. Bounding
the crest only restated the whole-RMS assertion — crest is peak minus RMS, and
both carried the same tolerance against the same baseline — so the check asserts
the fact the crest reasoning rests on: the program already touches full scale, so
a build that got **louder** cannot raise its peak, it closes the crest instead.
Under the +3 dB control the peak is bit-identical at 32768 while the crest falls
2.8 dB, and the whole-RMS assertion is what fails on it; the −3 dB control drops
the peak and fails the peak assertion directly.

**Both directions, engine-level.** `MDKR_AUDIO_TEST_GAIN_DB` scales the synthesised PCM
inside `dkr_audio_service_tick()` — before the engine's own RMS accounting, before the dump,
before the sink. It **refuses to act whenever a host output device is open**, so it is
file-domain only and can never make sound; with the variable unset the capture is
byte-identical to a build compiled without the seam (verified by `cmp`).

| arm | whole RMS | crest | rails | port/console | result |
|---|---|---|---|---|---|
| reference | −12.857 dBFS | 12.857 dB | 0.07248 % | +0.016 dB | PASS |
| `+3 dB` engine control | −10.058 dBFS | 10.058 dB | 1.21170 % | — | FAIL ×28 |
| `−3 dB` engine control | −15.857 dBFS | 12.857 dB | 0.00000 % | — | FAIL ×28 |

Four signal-level controls (±3.0 and ±1.5 dB applied in memory) also run on every
invocation and the check fails if any of them passes. The ±1.5 pair is there so the
band cannot quietly become a rubber ruler.

### What this still does not settle

- **The reference is an emulator, not silicon.** ares' RSP audio implementation is an
  implementation. It is the same reference this repo already trusts for video and
  racer state, and it is enormously better than nothing, but a hardware line-out
  capture would be a stronger reference and none was obtainable here.
- **Alignment is coarse.** The two runners do not hold frame-precise alignment through
  a multi-tap menu route (`docs/ORACLE.md` limitation 3); the aligned overlap is the
  same *program* but not the same *instants*, correlation +0.76. That is why the
  console tolerance is ±1.5 dB broadband / ±6 dB per band rather than something tight,
  and why the per-band deltas above should be read as a tilt, not as calibrated
  numbers. The refreshed correlation is +0.7816.
- **The console capture is ROM-derived and cannot be committed**, so the `--reference`
  lane is opt-in and the registered gate runs the frozen-baseline half only. A CI run
  proves the port has not *drifted*; it does not re-prove absolute correctness.
- **Nothing here touches the host sinks.** No device is opened, so the SDL queue path
  and the browser AudioWorklet's own level handling remain unmeasured.
- **What would close the remaining gap:** a real N64 line-out capture of the same
  route, digitised at known gain, to replace the emulator reference; and a
  sample-accurate route (single deterministic tap, no multi-screen menu drift) so the
  per-band comparison stops carrying alignment noise and the spectral tilt above can be
  attributed rather than merely observed.
