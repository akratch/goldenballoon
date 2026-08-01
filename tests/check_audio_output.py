#!/usr/bin/env python3
"""The synthesiser really produces music, SFX and reverb — asserted on the PCM.

Why this exists
---------------
README.md and CHANGELOG.md list Audio as working: "music, SFX and reverb, via the
decompiled synthesiser driving a software mixer". Until this check landed, **not one
fixture asserted on audio at all**. Every check runs muted, by the hard rule in
CONTRIBUTING.md, so the claim rested entirely on somebody having listened to it — the
only green row in the status table with no check behind it.

Nothing here opens an audio device. `--headless-frames` returns before SDL audio is
touched, and `MDKR_AUDIO=0` is set on top of that. Synthesis still runs headlessly, so
the whole DSP path is exercised; `MDKR_AUDIO_DUMP` taps the PCM to a file and
`MDKR_AUDIO_RMS=1` prints the engine's own RMS/peak/clip/fx-guard accounting. The
capture is **ROM-derived** and is written to a temp dir that is deleted unless
--keep-audio is given; it must never be committed (`.gitignore` covers `*.wav`).

Runtime: ~12 s — two 4300-frame headless runs (~4 s each, reverb on and reverb off)
plus pure-Python analysis of 2 x 12.7 MB of PCM. No third-party modules.

What is asserted (measured values are from the reference run, US 1.1)
--------------------------------------------------------------------
 1. FORMAT. The WAV is 22050 Hz / 2 ch / 16-bit, and the synth's own frame size is
    736.0 sample-frames per pump = 33.379 ms = two NTSC fields (33.367 ms), 0.04 %
    apart. The second half matters: a WAV header can say anything, but if the
    synthesiser's output rate were misconfigured the sample count per pump would move
    with it. The native and web sinks agree on 22050/2 (DKR_OUTPUT_RATE).

 2. NOT SILENT, and silent exactly where it should be. Whole-capture RMS 6580.4
    against a floor of 1500, every one of the eight music windows above that floor
    (2372-8908), and the pre-boot window genuinely silent: RMS 65.1 with 99.61 % of
    its samples exactly zero. That last part is what stops the RMS floor from being a
    vacuous test of "some bytes arrived" — it is real, engine-produced silence sitting
    in the same capture, and the check asserts on both.

 3. NOT SATURATED. `[AUDIO] mainbus clip` is the mixer's own count of how often the
    final sum wanted to exceed s16 full scale: 1372/12656256 = 0.01084 %, worst
    pre-clamp magnitude 38017 = 1.29 dBFS over. The WAV-side rail count agrees:
    110/6328128 = 0.00174 % of samples at +32767/-32768. Both are asserted below
    ceilings, as is the overshoot in dB.
    NOTE: the peak IS full scale (32768, i.e. a -32768 sample). This check therefore
    does NOT assert "peak below full scale" — that assertion would FAIL on correct
    audio. DKR's mix legitimately touches the rail and the RSP mixer saturates on
    hardware too. What distinguishes healthy from destroyed is the *fraction* at the
    rail and the pre-clamp overshoot, which is what is asserted.

 4. BOTH CHANNELS ALIVE AND DECORRELATED. L RMS 6594.2, R RMS 6566.5, both above the
    floor; and crucially rms(L-R) = 2864.1 with corr(L,R) = +0.9053 — not a mono
    duplicate, which gives rms(L-R) exactly 0.0 and corr exactly 1.0. The reverb and
    pan paths are what decorrelate them: with MDKR_AUDIO_REVERB=0 the same route
    measures rms(L-R) 3475.5 / corr +0.8876, so both arms are genuinely stereo.

 5. THE MUSIC CHANGES WHEN IT SHOULD. The route crosses six music sequences and a
    level load. For each pair of adjacent windows the 8-band spectral energy
    distribution must differ by an L1 distance above a floor: measured 0.525, 0.537,
    0.614, 0.747, 0.979, 0.996 and 1.145 across the seven adjacent pairs, against a
    floor of 0.15. A synthesiser stuck replaying one buffer would pass 1-4 and fail
    this — which is exactly what the `static` control below demonstrates.

 6. THE BEAT GRID SITS AT THE TEMPO THE SEQUENCER PROGRAMMED. This is the assertion
    that separates "the synth is running" from "the synth is running at the wrong
    rate", i.e. the class of bug behind the "crazy fast dance" report.

    The reference is the LIVE tempo, read out of the sequence player each frame by
    the `[AUDIO] music ... uspt=` trace added to audi_port_dkr.c: `alCSPGetTempo()`
    returns microseconds per quarter note currently in force, so it is a pure
    control-side value with no dependence on the sample clock. Comparing it against a
    period measured in samples is exactly a test of the sequencer-clock-to-sample-clock
    relationship.

    Method: 256-sample RMS envelope -> log -> half-wave-rectified first difference
    (onset strength) -> normalised autocorrelation -> comb score = mean ACF over the
    candidate period and its first four multiples. For each window the comb is scanned
    over 0.75x-1.35x the declared quarter note; the argmax must land within 2 % of
    1.00x, and the score at 1.00x must exceed the score at 0.95x/1.05x/1.12x by a
    margin. Measured:

      seq 26  182.11 BPM  q=329.47 ms  21.6 s  argmax 1.0041x  comb@q +0.3850  margin +0.3683
      seq 24  120.01 BPM  q=499.97 ms  19.6 s  argmax 0.9981x  comb@q +0.3923  margin +0.3853
      seq 10  126.01 BPM  q=476.16 ms  36.8 s  argmax 0.9996x  comb@q +0.3053  margin +0.2871

    i.e. the beat grid is within 0.4 % of the programmed tempo in every case, and the
    +-5 % detunings score *negative* (-0.02 to -0.04) — a 10x margin, not a threshold
    scraped past. 182.11 BPM for the title sequence independently confirms the 182 BPM
    / 659 ms figure that check_menu_anim_rate.py was built around.

    The live readout also exposes something worth knowing: `music_tempo()`
    (sMusicTempo) is only the BPM audio.c last programmed from gSeqSoundTable, and it
    goes STALE. The attract-mode sequence 35 carries its own AL_MIDI_META_TEMPO events
    and walks 130.1 -> 140.0 -> 120.0 BPM while music_tempo() still reports 110. Using
    music_tempo() as the reference would have made this assertion measure the wrong
    number on that window. alCSPGetTempo() is the correct reference.

    Two honest limits. (a) A window with no percussive onsets has no beat grid to
    find; such windows are reported and skipped, and at least MIN_TEMPO_WINDOWS must
    remain testable so a global loss of beat structure cannot pass vacuously
    (MIN_TEMPO_WINDOWS is deliberately equal to the count the reference route yields,
    so losing any one of them is a failure). The attract-mode intro (seq 35) is one of
    these: its comb profile is flat at 0.00-0.03 across candidate periods from 60 ms to
    4 s, i.e. no periodicity anywhere rather than periodicity in the wrong place — it
    is a sustained texture, not a defect. (b) Autocorrelation cannot distinguish a
    period from an exact octave of it, so a rate error of exactly 2x or 0.5x is not
    caught here — assertion 1's samples-per-pump check is what bounds that.

 7. THE REVERB PATH ACTUALLY CONTRIBUTES. The second run with MDKR_AUDIO_REVERB=0 is
    a true engine-level A/B (audio.c gDkrReverbEnabled). 322974 samples (5.10 %) are
    bit-exactly zero in the dry capture; at those same instants the wet capture carries
    RMS 100.9 with peak 5011 — energy that can only have come from the delay lines.
    86.8 % of all samples differ between the two runs. This reproduces, as a standing
    check, the methodology recorded in docs/OPEN_ITEMS.md "M5 open items".
    `[AUDIO] fx-guard trips` must read 0 in both arms: non-zero means a delay-line DMA
    tried to leave its allocation and was clamped, which is the LP64 delay-tap overrun
    documented in the same place.

    Incidental confirmation of a documented gain-stage effect: with the FX bypassed the
    aux wet send is still summed to the main bus undelayed at unity, and the check's own
    numbers show it — reverb OFF raises the main-bus clip rate 5.1x (7019 vs 1372
    events) and the worst pre-clamp overshoot from 1.29 to 3.75 dBFS. Reverb ON is the
    better-behaved configuration, which is why it is the default.

Self-validation — this check is proven to be able to fail
---------------------------------------------------------
An audio check that cannot fail is worse than none, because it would license the claim
it is supposed to test. Two kinds of positive control, both run and both recorded:

ENGINE-LEVEL (`--control`, run manually; each must exit 1):

    --control dry-vs-dry   both arms MDKR_AUDIO_REVERB=0
                           -> assertion 7 fails: "the wet path contributes nothing
                              where the dry path is silent (rms 0.00 < 10.0)" and
                              "only 0.0 % of samples differ between reverb-on and
                              reverb-off"
    --control boot-only    200 frames, 176 of them real engine silence
                           -> assertion 2 fails on whole-capture rms 390.5 < 1500,
                              and 4, 5, 6 and 7 fall over with it

SIGNAL-LEVEL (run by default; --no-controls to skip). The same analysis is re-run over
deliberately corrupted copies of the real capture, and each control MUST trip the
assertion it targets — this check FAILS if any control passes:

    silence   all samples zeroed                      -> trips 2
    mono      R := L                                  -> trips 4
    clip      x6 gain into the rail                   -> trips 3
    static    one 0.5 s block repeated for the file   -> trips 5
    detune    resampled by 1.12x                      -> trips 6

The pre-boot silent window in assertion 2 is a third kind: real silence produced by the
game, in the same capture, asserted to be silent.

What this does NOT cover — needs a device or a human ear
-------------------------------------------------------
 * That anything is audible at all: no device is ever opened by this check.
   The separate ROM-free `audio_sink_contract` CTest proves the shared queue
   controller plus SDL open/format/drain/pause/clear path with silence (and can
   run on a physical device); real speaker output still needs a human or loopback.
 * Timbre, tuning and instrument assignment. A bank that loaded the wrong instrument,
   or an envelope with the wrong decay, produces a healthy RMS, a healthy beat grid
   and the wrong sound. Only an ear (or a golden-master comparison against hardware,
   which would be ROM-derived and cannot be committed) catches that.
 * Whether the full-scale peaks in assertion 3 are audible distortion. 0.011 % of
   samples clamping is very likely inaudible, but this check measures it rather than
   judging it.
 * Realtime game-audio sink behavior. Headless synthesis uses one deterministic block per due
   two-field audio quantum, so original NTSC game time and capture time now advance
   together (736 samples = 33.38 ms against two fields = 33.37 ms). Enhanced
   one-field simulation services audio every second game pass; grouped lateness and
   presentation-rate invariance are covered by the scheduler gates. The SDL
   contract measures bounded application-queue backlog and drain independently;
   hidden hardware-buffer underruns, audible game output, and drift against a
   physical DAC remain outside this PCM-only check.
 * An exact 2x/0.5x global rate error, per assertion 6(b).

Usage:
    tests/check_audio_output.py
    tests/check_audio_output.py --keep-audio /tmp/a   # keep the captures (ROM-derived!)
    tests/check_audio_output.py --no-controls         # skip the self-validation arms
    tests/check_audio_output.py --control dry-vs-dry  # positive control; must exit 1
    tests/check_audio_output.py --control boot-only   # positive control; must exit 1

Always runs muted + headless (MDKR_AUDIO=0 and --headless-frames), per
tests/README.md.
"""
import argparse
import array
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import wave

from harness_utils import resolve_binary

# ---- fixture ---------------------------------------------------------------
SCRIPT = "tests/input_scripts/race_drive_time_trial.txt"
FRAMES = 4300           # boot -> attract -> title -> char select -> track select -> race

# ---- assertion 1: format --------------------------------------------------
WANT_RATE = 22050       # audi_port_dkr.c DKR_OUTPUT_RATE (the web sink reports the same)
WANT_CHANNELS = 2
WANT_WIDTH = 2
NTSC_FIELD_MS = 1000.0 / 59.94
PUMP_FIELDS = 2         # amAudioGetFrameSize() is ~2 VI fields
PUMP_TOL = 0.01

# ---- assertion 2: level ---------------------------------------------------
RMS_FLOOR = 1500.0      # whole capture and every music window
SILENT_RMS_CEIL = 400.0 # the pre-boot window must be below this
SILENT_ZERO_FRAC = 0.90 # ...and at least this fraction of it exactly zero

# ---- assertion 3: saturation ---------------------------------------------
RAIL_FRAC_CEIL = 0.005      # 0.5 % of samples at +-full scale (measured 0.00015)
CLIP_FRAC_CEIL = 0.005      # engine's own mainbus clip rate  (measured 0.00011)
OVERSHOOT_DB_CEIL = 8.0     # worst pre-clamp magnitude, dBFS (measured 1.29)

# ---- assertion 4: channels ----------------------------------------------
CHAN_RMS_FLOOR = 1000.0
LR_DIFF_RMS_FLOOR = 200.0   # measured 2687; a mono duplicate gives exactly 0
LR_CORR_CEIL = 0.995        # measured 0.875; a mono duplicate gives exactly 1.0

# ---- assertion 5: the music changes -------------------------------------
BANDS = (0, 100, 200, 400, 800, 1600, 3200, 6400, WANT_RATE // 2)
SPEC_L1_FLOOR = 0.15        # measured 0.396-1.316 between adjacent windows
CHANGE_WINDOW_MIN_S = 3.0   # windows shorter than this are not compared

# ---- assertion 6: tempo -------------------------------------------------
ENV_WIN = 256               # 11.61 ms hop at 22050 Hz
TEMPO_WINDOW_MIN_S = 12.0
TEMPO_TRIM_S = 0.35         # trimmed off each window edge (the trace is +-1 pump)
TEMPO_SCAN_LO = 0.75
TEMPO_SCAN_HI = 1.35
TEMPO_SCAN_N = 400
TEMPO_HARMONICS = 4
TEMPO_DETECT_MIN = 0.10     # below this there is no beat grid to test
TEMPO_ARGMAX_TOL = 0.02     # argmax must be within 2 % of the declared quarter
TEMPO_MARGIN = 0.10         # comb(1.00x) - max(comb(rival detunings))
TEMPO_RIVALS = (0.95, 1.05, 1.0 / 1.12, 1.12)
MIN_TEMPO_WINDOWS = 3       # anti-vacuity: this many windows must stay testable

# ---- assertion 7: reverb -------------------------------------------------
WET_AT_DRY_ZERO_RMS_FLOOR = 10.0   # measured 85.2
DRY_ZERO_FRAC_FLOOR = 0.005        # measured 0.068 — the A/B needs dry zeros to exist
WET_DRY_DIFF_FRAC_FLOOR = 0.20     # measured 0.868


try:                                    # Python 3.12+: C-speed sum of products,
    from math import sumprod             # which is what keeps this pure-stdlib
except ImportError:                     # check fast over 12.7 MB of PCM.
    def sumprod(a, b):
        return sum(x * y for x, y in zip(a, b))


# ==========================================================================
# capture
# ==========================================================================
def run_capture(build, rom, script, frames, wav, reverb=True, timeout=600):
    """One headless run. NEVER without --headless-frames; MDKR_AUDIO=0 on top."""
    env = dict(os.environ)
    env["MDKR_AUDIO"] = "0"             # belt-and-braces; --headless-frames is the guarantee
    env["MDKR_AUDIO_RMS"] = "1"         # engine RMS/peak/clip/fx-guard + the music trace
    env["MDKR_AUDIO_DUMP"] = wav
    # Set explicitly in BOTH directions: an inherited MDKR_AUDIO_REVERB=0 in the
    # caller's environment would otherwise make the wet arm dry and turn the A/B in
    # assertion 7 into a comparison of a run against itself.
    env["MDKR_AUDIO_REVERB"] = "1" if reverb else "0"
    cmd = [build, "--headless-frames", str(frames), "--input-script", script,
           "--rom", rom]
    proc = subprocess.run(cmd, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=timeout)
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


TRACE_RE = re.compile(r"\[AUDIO\] music seq=(-?\d+) tempo=(-?\d+) uspt=(-?\d+) "
                      r"playing=(\d+) at sample=(\d+)")
CLIP_RE = re.compile(r"\[AUDIO\] mainbus clip: (\d+)/(\d+) samples .*? "
                     r"magnitude (\d+) \(([-0-9.]+) dBFS\)")
GUARD_RE = re.compile(r"\[AUDIO\] fx-guard trips=(\d+)")
TOTAL_RE = re.compile(r"\[AUDIO\] TOTAL frames=(\d+) samples=(\d+) "
                      r"rms=([0-9.]+) peak=(\d+)")


def parse_engine(out):
    """Pull the engine's own accounting out of stdout."""
    info = {"windows": [], "clip": None, "guard": None, "total": None}
    ev = []
    for line in out.splitlines():
        m = TRACE_RE.search(line)
        if m:
            seq, bpm, uspt, play, s = (int(g) for g in m.groups())
            ev.append({"seq": seq, "table_bpm": bpm, "uspt": uspt,
                       "playing": bool(play), "start": s})
            continue
        m = CLIP_RE.search(line)
        if m:
            info["clip"] = (int(m.group(1)), int(m.group(2)),
                            int(m.group(3)), float(m.group(4)))
            continue
        m = GUARD_RE.search(line)
        if m:
            info["guard"] = int(m.group(1))
            continue
        m = TOTAL_RE.search(line)
        if m:
            info["total"] = (int(m.group(1)), int(m.group(2)),
                             float(m.group(3)), int(m.group(4)))
    for i, w in enumerate(ev):
        w["end"] = ev[i + 1]["start"] if i + 1 < len(ev) else None
    info["windows"] = ev
    return info


def load_wav(path):
    w = wave.open(path, "rb")
    try:
        rate, ch, width, n = (w.getframerate(), w.getnchannels(),
                              w.getsampwidth(), w.getnframes())
        raw = w.readframes(n)
    finally:
        w.close()
    a = array.array("h")
    a.frombytes(raw)
    if sys.byteorder == "big":
        a.byteswap()
    return rate, ch, width, a


# ==========================================================================
# measurement primitives (pure stdlib, C-speed via math.sumprod / array methods)
# ==========================================================================
def rms(a):
    return math.sqrt(sumprod(a, a) / len(a)) if len(a) else 0.0


def peak(a):
    return max(max(a), -min(a)) if len(a) else 0


def rail_count(a):
    """Samples pinned at s16 full scale, either sign."""
    return a.count(32767) + a.count(-32768)


def corr(x, y):
    n = len(x)
    if n == 0:
        return 0.0
    sx, sy = sum(x), sum(y)
    sxx, syy, sxy = sumprod(x, x), sumprod(y, y), sumprod(x, y)
    cov = sxy - sx * sy / n
    vx, vy = sxx - sx * sx / n, syy - sy * sy / n
    if vx <= 0 or vy <= 0:
        return 1.0 if cov >= 0 else -1.0
    return cov / math.sqrt(vx * vy)


def diff_rms(x, y):
    """rms(x - y) without materialising the difference."""
    n = len(x)
    if n == 0:
        return 0.0
    v = (sumprod(x, x) - 2.0 * sumprod(x, y) + sumprod(y, y)) / n
    return math.sqrt(v) if v > 0 else 0.0


def mono_env(a, lo, hi, win):
    """RMS envelope of the L+R mean over sample-frames [lo,hi), one point per win."""
    out = []
    i = lo
    while i + win <= hi:
        s = a[2 * i:2 * (i + win)]
        out.append(math.sqrt(sumprod(s, s) / len(s)))
        i += win
    return out


def onset_strength(env):
    """log-compress, then half-wave-rectified first difference."""
    lg = [math.log1p(v) for v in env]
    return [max(0.0, lg[i + 1] - lg[i]) for i in range(len(lg) - 1)]


def autocorr(x, max_lag):
    """Normalised autocorrelation of a mean-removed signal, lags 0..max_lag."""
    n = len(x)
    if n < 4:
        return [0.0]
    mu = sum(x) / n
    d = [v - mu for v in x]
    r0 = sumprod(d, d)
    if r0 <= 0:
        return [0.0]
    max_lag = min(max_lag, n - 2)
    return [sumprod(d[:n - k], d[k:]) / r0 for k in range(max_lag + 1)]


def comb_score(acf, period_s, hop_s, harmonics=TEMPO_HARMONICS):
    """Mean interpolated ACF at the period and its first `harmonics` multiples."""
    tot, cnt = 0.0, 0
    for h in range(1, harmonics + 1):
        lag = period_s * h / hop_s
        i = int(lag)
        f = lag - i
        if i + 1 >= len(acf):
            break
        tot += acf[i] * (1.0 - f) + acf[i + 1] * f
        cnt += 1
    return tot / cnt if cnt else 0.0


def band_spectrum(env_signal_a, lo, hi, rate):
    """8-band energy fractions via a direct Goertzel-style DFT on a decimated mono
    signal. Pure stdlib; only 8 numbers are needed, so a full FFT is unnecessary."""
    # Decimate to ~5512 Hz mono so the band edges up to 2756 Hz stay meaningful and
    # the transform is cheap; the top two bands are folded in from the pre-decimation
    # energy so the fractions still sum to 1.
    step = 4
    n = (hi - lo) // step
    n = min(n, 16384)
    if n < 64:
        return None
    x = [0.0] * n
    for i in range(n):
        j = 2 * (lo + i * step)
        x[i] = (env_signal_a[j] + env_signal_a[j + 1]) * 0.5
    mu = sum(x) / n
    for i in range(n):
        x[i] -= mu
    dec_rate = rate / step
    # Power at a set of probe frequencies per band (Goertzel), averaged per band.
    energies = []
    for b in range(len(BANDS) - 1):
        f0, f1 = BANDS[b], BANDS[b + 1]
        f1 = min(f1, dec_rate / 2.0 - 1.0)
        if f1 <= f0:
            energies.append(0.0)
            continue
        probes = 6
        tot = 0.0
        for p in range(probes):
            f = f0 + (f1 - f0) * (p + 0.5) / probes
            w = 2.0 * math.pi * f / dec_rate
            c = 2.0 * math.cos(w)
            s1 = s2 = 0.0
            for v in x:
                s0 = v + c * s1 - s2
                s2, s1 = s1, s0
            tot += s1 * s1 + s2 * s2 - c * s1 * s2
        energies.append(max(0.0, tot / probes))
    t = sum(energies)
    return [e / t for e in energies] if t > 0 else None


def l1(a, b):
    return sum(abs(x - y) for x, y in zip(a, b))


# ==========================================================================
# window bookkeeping
# ==========================================================================
def frames_of(a):
    return len(a) // WANT_CHANNELS


def music_windows(info, nframes):
    """Constant-(seq,uspt) windows where the music player is playing."""
    out = []
    for w in info["windows"]:
        end = w["end"] if w["end"] is not None else nframes
        end = min(end, nframes)
        if w["playing"] and w["uspt"] > 0 and end > w["start"]:
            out.append((w["seq"], w["table_bpm"], w["uspt"], w["start"], end))
    return out


def preboot_window(info, nframes):
    """The leading window before any sequence starts — real, engine-produced silence."""
    for w in info["windows"]:
        if not w["playing"]:
            end = w["end"] if w["end"] is not None else nframes
            if end > w["start"]:
                return (w["start"], min(end, nframes))
    return None


# ==========================================================================
# assertions
# ==========================================================================
def assert_format(rate, ch, width, a, info, fail, note):
    if (rate, ch, width) != (WANT_RATE, WANT_CHANNELS, WANT_WIDTH):
        fail("format: capture is %d Hz / %d ch / %d-bit, engine claims %d / %d / %d"
             % (rate, ch, width * 8, WANT_RATE, WANT_CHANNELS, WANT_WIDTH * 8))
        return
    pumps = info["total"][0] if info["total"] else 0
    if pumps <= 0:
        fail("format: no [AUDIO] TOTAL line — the engine synthesised nothing")
        return
    per_pump = frames_of(a) / pumps
    ms = 1000.0 * per_pump / rate
    want_ms = PUMP_FIELDS * NTSC_FIELD_MS
    note("format: %d Hz / %d ch / %d-bit; %.1f sample-frames per pump = %.3f ms "
         "(%d NTSC fields = %.3f ms)" % (rate, ch, width * 8, per_pump, ms,
                                         PUMP_FIELDS, want_ms))
    if abs(ms - want_ms) / want_ms > PUMP_TOL:
        fail("format: the synth produces %.3f ms per pump, not the %.3f ms of %d VI "
             "fields — the output rate is misconfigured (%.2f %% off)"
             % (ms, want_ms, PUMP_FIELDS, 100.0 * (ms / want_ms - 1.0)))


def assert_level(a, info, wins, fail, note):
    whole = rms(a)
    note("level: whole-capture rms %.1f (floor %.0f), peak %d"
         % (whole, RMS_FLOOR, peak(a)))
    if whole < RMS_FLOOR:
        fail("level: the capture is silent or near-silent (rms %.1f < %.1f)"
             % (whole, RMS_FLOOR))
    quiet = []
    for seq, bpm, uspt, s0, s1 in wins:
        r = rms(a[2 * s0:2 * s1])
        if r < RMS_FLOOR:
            quiet.append((seq, r))
    if quiet:
        fail("level: music window(s) below the floor while the sequence player says "
             "it is playing: %s" % ", ".join("seq %d rms %.1f" % q for q in quiet))
    pb = preboot_window(info, frames_of(a))
    if pb is None:
        fail("level: no pre-boot window in the trace — cannot show the floor is not "
             "vacuous")
        return
    s0, s1 = pb
    seg = a[2 * s0:2 * s1]
    r = rms(seg)
    zf = seg.count(0) / len(seg)
    note("level: pre-boot window %.2f-%.2f s is silent: rms %.1f, %.2f %% exact zeros"
         % (s0 / WANT_RATE, s1 / WANT_RATE, r, 100.0 * zf))
    if r > SILENT_RMS_CEIL or zf < SILENT_ZERO_FRAC:
        fail("level: the pre-boot window is NOT silent (rms %.1f > %.1f or only "
             "%.2f %% zeros) — either the engine emits garbage before audio starts, "
             "or the RMS floor above proves nothing" % (r, SILENT_RMS_CEIL, 100.0 * zf))


def assert_saturation(a, info, fail, note):
    rails = rail_count(a)
    frac = rails / len(a)
    note("saturation: %d/%d samples at the rail (%.5f %%, ceiling %.3f %%)"
         % (rails, len(a), 100.0 * frac, 100.0 * RAIL_FRAC_CEIL))
    if frac > RAIL_FRAC_CEIL:
        fail("saturation: %.4f %% of samples are pinned at s16 full scale (ceiling "
             "%.3f %%) — the mix is clipping, which a non-silence test would pass"
             % (100.0 * frac, 100.0 * RAIL_FRAC_CEIL))
    if info["clip"] is None:
        fail("saturation: no [AUDIO] mainbus clip line to cross-check against")
    else:
        hits, tot, over, db = info["clip"]
        cf = hits / tot if tot else 0.0
        note("saturation: engine mainbus clip %d/%d (%.5f %%), worst pre-clamp "
             "magnitude %d (%.2f dBFS)" % (hits, tot, 100.0 * cf, over, db))
        if cf > CLIP_FRAC_CEIL:
            fail("saturation: the mixer's own main-bus clip rate is %.4f %% "
                 "(ceiling %.3f %%)" % (100.0 * cf, 100.0 * CLIP_FRAC_CEIL))
        if db > OVERSHOOT_DB_CEIL:
            fail("saturation: the final sum overshot full scale by %.2f dB (ceiling "
                 "%.1f dB) — a gain-stage error upstream of the clamp"
                 % (db, OVERSHOOT_DB_CEIL))


def assert_channels(a, fail, note):
    L, R = a[0::2], a[1::2]
    lr, rr = rms(L), rms(R)
    d = diff_rms(L, R)
    c = corr(L, R)
    note("channels: L rms %.1f, R rms %.1f, rms(L-R) %.1f, corr(L,R) %+.4f"
         % (lr, rr, d, c))
    if lr < CHAN_RMS_FLOOR or rr < CHAN_RMS_FLOOR:
        fail("channels: a channel is dead or nearly so (L rms %.1f, R rms %.1f, "
             "floor %.0f)" % (lr, rr, CHAN_RMS_FLOOR))
    if d < LR_DIFF_RMS_FLOOR:
        fail("channels: L and R are the same signal (rms(L-R) %.2f < %.0f) — a mono "
             "duplicate, where the reverb and pan paths should decorrelate them"
             % (d, LR_DIFF_RMS_FLOOR))
    if c > LR_CORR_CEIL:
        fail("channels: corr(L,R) = %.5f > %.3f — the channels carry the same signal"
             % (c, LR_CORR_CEIL))


def assert_music_changes(a, wins, rate, fail, note):
    usable = [w for w in wins if (w[4] - w[3]) / rate >= CHANGE_WINDOW_MIN_S]
    if len(usable) < 2:
        fail("change: only %d music window(s) longer than %.0f s — the route does not "
             "cross a transition, so nothing was compared" % (len(usable),
                                                              CHANGE_WINDOW_MIN_S))
        return
    specs = []
    for seq, bpm, uspt, s0, s1 in usable:
        sp = band_spectrum(a, s0, s1, rate)
        if sp is not None:
            specs.append((seq, s0, s1, sp, rms(a[2 * s0:2 * s1])))
    if len(specs) < 2:
        fail("change: fewer than two analysable windows")
        return
    worst = None
    for i in range(len(specs) - 1):
        (sa, a0, a1, spa, ra) = specs[i]
        (sb, b0, b1, spb, rb) = specs[i + 1]
        d = l1(spa, spb)
        note("change: seq %d (%.1f-%.1f s, rms %.0f) -> seq %d (%.1f-%.1f s, rms %.0f)"
             ": spectral L1 %.3f" % (sa, a0 / rate, a1 / rate, ra, sb, b0 / rate,
                                     b1 / rate, rb, d))
        if worst is None or d < worst[0]:
            worst = (d, sa, sb)
    if worst[0] < SPEC_L1_FLOOR:
        fail("change: adjacent music windows seq %d and seq %d are spectrally "
             "identical (L1 %.4f < %.2f) — the synthesiser is replaying one buffer, "
             "not following the sequence" % (worst[1], worst[2], worst[0],
                                             SPEC_L1_FLOOR))


def tempo_report(a, wins, rate):
    """Per-window comb tempogram against the live sequencer tempo."""
    hop = ENV_WIN / rate
    trim = int(TEMPO_TRIM_S * rate)
    rows = []
    for seq, table_bpm, uspt, s0, s1 in wins:
        lo, hi = s0 + trim, s1 - trim
        if (hi - lo) / rate < TEMPO_WINDOW_MIN_S:
            continue
        q = uspt / 1e6
        env = mono_env(a, lo, hi, ENV_WIN)
        acf = autocorr(onset_strength(env),
                       int((q * TEMPO_HARMONICS * TEMPO_SCAN_HI) / hop) + 4)
        best = (None, -2.0)
        for i in range(TEMPO_SCAN_N):
            mult = TEMPO_SCAN_LO + (TEMPO_SCAN_HI - TEMPO_SCAN_LO) * i / (TEMPO_SCAN_N - 1)
            sc = comb_score(acf, q * mult, hop)
            if sc > best[1]:
                best = (mult, sc)
        at_q = comb_score(acf, q, hop)
        rivals = max(comb_score(acf, q * m, hop) for m in TEMPO_RIVALS)
        rows.append({"seq": seq, "table_bpm": table_bpm, "live_bpm": 60e6 / uspt,
                     "q_ms": q * 1000.0, "secs": (hi - lo) / rate,
                     "argmax": best[0], "best": best[1], "at_q": at_q,
                     "margin": at_q - rivals})
    return rows


def assert_tempo(rows, fail, note):
    testable = [r for r in rows if r["best"] >= TEMPO_DETECT_MIN]
    for r in rows:
        state = "testable" if r["best"] >= TEMPO_DETECT_MIN else "NO BEAT GRID (skipped)"
        note("tempo: seq %2d live %6.2f BPM (q %.2f ms, table says %d) over %4.1f s: "
             "argmax %.4fx comb %+.4f, comb@q %+.4f, margin %+.4f  [%s]"
             % (r["seq"], r["live_bpm"], r["q_ms"], r["table_bpm"], r["secs"],
                r["argmax"], r["best"], r["at_q"], r["margin"], state))
    if len(testable) < MIN_TEMPO_WINDOWS:
        fail("tempo: only %d of %d music windows have a detectable beat grid "
             "(need %d) — either the route changed or the synthesiser has lost its "
             "rhythmic structure entirely" % (len(testable), len(rows),
                                              MIN_TEMPO_WINDOWS))
    for r in testable:
        if abs(r["argmax"] - 1.0) > TEMPO_ARGMAX_TOL:
            fail("tempo: seq %d plays at %.4fx the tempo the sequencer programmed "
                 "(%.2f BPM declared, beat grid found at %.2f BPM) — the synthesiser "
                 "is running at the wrong rate"
                 % (r["seq"], 1.0 / r["argmax"], r["live_bpm"],
                    r["live_bpm"] / r["argmax"]))
        elif r["margin"] < TEMPO_MARGIN:
            fail("tempo: seq %d has a beat grid but it is not sharply at the declared "
                 "quarter note (comb@q %+.4f, best rival detuning within %+.4f, need "
                 "%+.4f) — the tempo is within a few percent of wrong"
                 % (r["seq"], r["at_q"], r["at_q"] - r["margin"], TEMPO_MARGIN))


def assert_reverb(wet, dry, wet_info, dry_info, fail, note):
    for name, info in (("reverb-on", wet_info), ("reverb-off", dry_info)):
        if info["guard"] is None:
            fail("reverb: no [AUDIO] fx-guard line in the %s run" % name)
        elif info["guard"] != 0:
            fail("reverb: fx-guard tripped %d times in the %s run — a delay-line DMA "
                 "tried to leave its allocation and was clamped (the LP64 overrun in "
                 "docs/OPEN_ITEMS.md 'M5 open items')" % (info["guard"], name))
    n = min(len(wet), len(dry))
    if n == 0:
        fail("reverb: one of the two captures is empty")
        return
    w, d = wet[:n], dry[:n]
    # Samples the dry mix leaves bit-exactly zero; the wet mix must carry the tail.
    zi = [i for i in range(n) if d[i] == 0]
    zf = len(zi) / n
    if zf < DRY_ZERO_FRAC_FLOOR:
        fail("reverb: only %.4f %% of the reverb-off capture is bit-exactly zero "
             "(need %.2f %%) — no silent reference to measure the wet tail against"
             % (100.0 * zf, 100.0 * DRY_ZERO_FRAC_FLOOR))
        return
    sub = array.array("h", (w[i] for i in zi))
    wr, wp = rms(sub), peak(sub)
    diff = sum(1 for i in range(0, n, 7) if w[i] != d[i]) / len(range(0, n, 7))
    note("reverb: %d/%d samples (%.2f %%) are bit-exactly zero with reverb off; the "
         "reverb-on run carries rms %.1f, peak %d at those same instants"
         % (len(zi), n, 100.0 * zf, wr, wp))
    note("reverb: %.1f %% of samples differ between the two runs (1-in-7 sample)"
         % (100.0 * diff))
    if wr < WET_AT_DRY_ZERO_RMS_FLOOR:
        fail("reverb: the wet path contributes nothing where the dry path is silent "
             "(rms %.2f < %.1f) — the delay lines are not reaching the main bus"
             % (wr, WET_AT_DRY_ZERO_RMS_FLOOR))
    if diff < WET_DRY_DIFF_FRAC_FLOOR:
        fail("reverb: only %.1f %% of samples differ between reverb-on and reverb-off "
             "(need %.0f %%) — MDKR_AUDIO_REVERB is not changing the mix, so this A/B "
             "proves nothing" % (100.0 * diff, 100.0 * WET_DRY_DIFF_FRAC_FLOOR))


# ==========================================================================
# self-validation: corrupt the real capture, require the analysis to notice
# ==========================================================================
def mutate(a, kind, rate):
    out = array.array("h", a)
    n = len(out)
    if kind == "silence":
        for i in range(n):
            out[i] = 0
    elif kind == "mono":
        out[1::2] = out[0::2]
    elif kind == "clip":
        for i in range(n):
            v = out[i] * 6
            out[i] = 32767 if v > 32767 else (-32768 if v < -32768 else v)
    elif kind == "static":
        blk = int(0.5 * rate) * 2
        src = out[n // 2:n // 2 + blk]
        for off in range(0, n, blk):
            end = min(off + blk, n)
            out[off:end] = src[:end - off]
    elif kind == "detune":
        f = 1.12
        frames = n // 2
        for i in range(frames):
            j = int(i * f)
            if j >= frames:
                j = frames - 1
            out[2 * i] = a[2 * j]
            out[2 * i + 1] = a[2 * j + 1]
    else:
        raise ValueError(kind)
    return out


CONTROLS = (
    ("silence", 2, "all samples zeroed"),
    ("mono",    4, "R := L"),
    ("clip",    3, "x6 gain into the rail"),
    ("static",  5, "one 0.5 s block repeated"),
    ("detune",  6, "resampled by 1.12x"),
)


def run_control(kind, target, a, info, rate, wins):
    """Re-run the targeted assertion over a corrupted copy. Returns list of failures."""
    bad = mutate(a, kind, rate)
    fails = []

    def fail(msg):
        fails.append(msg)

    def note(_msg):
        pass

    if target == 2:
        assert_level(bad, info, wins, fail, note)
    elif target == 3:
        assert_saturation(bad, {"clip": info["clip"]}, fail, note)
    elif target == 4:
        assert_channels(bad, fail, note)
    elif target == 5:
        assert_music_changes(bad, wins, rate, fail, note)
    elif target == 6:
        assert_tempo(tempo_report(bad, wins, rate), fail, note)
    return fails


# ==========================================================================
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default="build")
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("--script", default=SCRIPT)
    ap.add_argument("--frames", type=int, default=FRAMES)
    ap.add_argument("--keep-audio", default=None,
                    help="write the captures here and keep them. ROM-DERIVED — never "
                         "commit a .wav (default: temp dir, deleted)")
    ap.add_argument("--no-controls", action="store_true",
                    help="skip the self-validation arms (they are what prove this "
                         "check can fail)")
    ap.add_argument("--control", choices=("dry-vs-dry", "boot-only"), default=None,
                    help="ENGINE-LEVEL positive control: deliberately break the run "
                         "and confirm this check reports FAIL (exit 1). 'dry-vs-dry' "
                         "runs both arms with MDKR_AUDIO_REVERB=0, which must trip "
                         "assertion 7; 'boot-only' captures 200 frames, of which the "
                         "engine renders the first 176 as real silence, which must trip "
                         "assertion 2.")
    args = ap.parse_args()
    args.build = resolve_binary(args.build)

    reverb_arms = (True, False)
    if args.control == "dry-vs-dry":
        reverb_arms = (False, False)
        print("check_audio_output: CONTROL dry-vs-dry — both arms MDKR_AUDIO_REVERB=0; "
              "assertion 7 must FAIL")
    elif args.control == "boot-only":
        args.frames = 200
        print("check_audio_output: CONTROL boot-only — 200 frames, 176 of them real "
              "engine silence; assertion 2 must FAIL")

    for path in (args.build, args.rom, args.script):
        if not os.path.exists(path):
            sys.exit("missing: %s" % path)

    tmp = args.keep_audio or tempfile.mkdtemp(prefix="mdkr64-audio-")
    os.makedirs(tmp, exist_ok=True)
    failures = []
    notes = []

    def fail(msg):
        failures.append(msg)

    def note(msg):
        notes.append(msg)
        print("   %s" % msg)

    try:
        wet_path = os.path.join(tmp, "reverb_on.wav")
        dry_path = os.path.join(tmp, "reverb_off.wav")
        print("check_audio_output: synthesising %d headless frames of %s (reverb %s, "
              "then reverb %s)" % (args.frames, os.path.basename(args.script),
                                   "on" if reverb_arms[0] else "OFF",
                                   "on" if reverb_arms[1] else "off"))
        arm_out = []
        for path, reverb, label in ((wet_path, reverb_arms[0], "arm A"),
                                    (dry_path, reverb_arms[1], "arm B")):
            rc, out = run_capture(args.build, args.rom, args.script, args.frames,
                                  path, reverb=reverb)
            if rc != 0:
                print("check_audio_output: FAIL — runner exited %d (%s)" % (rc, label))
                return 1
            for bad in ("[CRASH]", "[FATAL]", "[FX BUG]"):
                if bad in out:
                    print("check_audio_output: FAIL — %s in the %s run" % (bad, label))
                    return 1
            arm_out.append(out)
        wet_out, dry_out = arm_out

        wet_info, dry_info = parse_engine(wet_out), parse_engine(dry_out)
        rate, ch, width, wet = load_wav(wet_path)
        _, _, _, dry = load_wav(dry_path)
        nf = frames_of(wet)
        print("   captured %.2f s (%d sample-frames) per arm" % (nf / rate, nf))

        wins = music_windows(wet_info, nf)
        if not wins:
            print("check_audio_output: FAIL — no [AUDIO] music trace lines; the "
                  "sequence player never played, or the build predates the trace")
            return 1

        assert_format(rate, ch, width, wet, wet_info, fail, note)
        assert_level(wet, wet_info, wins, fail, note)
        assert_saturation(wet, wet_info, fail, note)
        assert_channels(wet, fail, note)
        assert_music_changes(wet, wins, rate, fail, note)
        rows = tempo_report(wet, wins, rate)
        assert_tempo(rows, fail, note)
        assert_reverb(wet, dry, wet_info, dry_info, fail, note)

        if failures:
            for f in failures:
                print("check_audio_output: FAIL — %s" % f)
            return 1

        if not args.no_controls:
            print("   self-validation: each corrupted copy must fail its assertion")
            silent_controls = []
            for kind, target, desc in CONTROLS:
                got = run_control(kind, target, wet, wet_info, rate, wins)
                if got:
                    print("     control %-8s (%-28s) -> assertion %d FAILS as required"
                          % (kind, desc, target))
                    print("       reason: %s" % got[0].split(" — ")[-1])
                else:
                    silent_controls.append((kind, target, desc))
            if silent_controls:
                for kind, target, desc in silent_controls:
                    print("check_audio_output: FAIL — control %s (%s) did NOT trip "
                          "assertion %d. The check cannot detect the defect it claims "
                          "to; it would license the claim instead of testing it."
                          % (kind, desc, target))
                return 1

        print("check_audio_output: PASS — %d Hz/%d ch PCM, rms %.0f over %.1f s; "
              "channels decorrelated (rms(L-R) %.0f); %d music sequences with the "
              "beat grid at the programmed tempo; reverb tail measured against a "
              "bit-exact dry reference; fx-guard 0"
              % (rate, ch, rms(wet), nf / rate, diff_rms(wet[0::2], wet[1::2]),
                 len([r for r in rows if r["best"] >= TEMPO_DETECT_MIN])))
        return 0
    finally:
        if not args.keep_audio:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
