#!/usr/bin/env python3
"""ABSOLUTE output level, against a frozen baseline and (optionally) the real ROM.

Why this exists
---------------
Every other audio gate in this tree is scale-blind by construction.
`check_audio_output.py` asserts a *floor* on RMS and a *ceiling* on saturation, so
anything between "clearly alive" and "clearly destroyed" passes; its tempo, stereo
and spectral-change assertions are all ratios or correlations, which a flat gain
leaves exactly unchanged. `check_raw16_audio.py` compares two arms of the *same*
build against each other. `[EVTQ]` telemetry counts events. The resource-plateau
voicePeak law counts voices.

So a systematic loudness bias — one wrong shift in a gain stage, an extra `/2`, a
master trim someone added "to stop the clipping" — would pass the entire suite.
`docs/open-items/audio.md` recorded exactly that gap: the clean-room engine was
measured within 0.5 dB of the pre-swap baseline (a port-vs-port comparison), but
full-scale level against real hardware had never been measured at all.

This check closes the *regression* half of that gap unconditionally and the
*fidelity* half whenever a reference capture is available.

Nothing here opens an audio device. `--headless-frames` returns before SDL audio is
touched and `MDKR_AUDIO=0` is set on top of it; the capture is a file. The capture
is ROM-DERIVED and is written to a temp dir that is deleted unless --keep-audio is
given; it must never be committed (`.gitignore` covers `*.wav`).

Runtime: ~42 s — one 4300-frame headless run plus pure-Python analysis of 12.7 MB
of PCM, repeated over four deliberately mis-scaled copies of it. No third-party
modules.

The fixture
-----------
`tests/input_scripts/race_drive_time_trial.txt` at 4300 frames: boot -> attract ->
title -> character select -> track select -> race. The same route
`check_audio_output.py` uses, so the two gates measure the same program material
and a level move shows up in both bodies of evidence. It yields 3 164 064
sample-frames = 143.49 s of audio at 22050 Hz stereo.

(That is ~2x the wall-clock game time: the pump emits two VI fields of audio per
rendered frame while the headless pacer injects one. Everything asserted here
lives inside the audio timeline, so the 2x does not affect it — see
`check_audio_output.py`'s note on the same point.)

What is asserted, and why each number is not redundant
-----------------------------------------------------
 1. FORMAT AND LENGTH. 22050 Hz / 2 ch / 16-bit and the exact sample-frame count.
    Anti-vacuity: every level number below is an average, and an average over the
    wrong amount of material is not the measurement it claims to be.

 2. WHOLE-CAPTURE RMS, in dBFS, within +-1.0 dB of the frozen baseline
    -12.857 dBFS (7457.9 of 32768). This is the primary detector. A flat +-3 dB
    bias misses it by 3x the tolerance.

 3. PER-CHANNEL RMS, L and R separately, within +-1.2 dB. A gain error confined to
    one side of the pan law, or to one bus, moves one of these and not the other.

 4. SAMPLE PEAK AT FULL SCALE, within 0.01 dB of 0.0 dBFS, reported alongside the
    crest factor (sample peak minus RMS, baseline 12.857 dB). Bounding the crest
    itself would restate assertion 2 — crest is peak minus RMS and both carried a
    +-1.0 dB tolerance against the same baseline — so the peak is asserted
    directly. That is the fact the crest reasoning rests on: the program already
    touches full scale, so a build that got louder cannot raise its peak, it can
    only raise its RMS, which closes the crest. Under the +3 dB engine control the
    crest falls to 10.058 dB even though the peak is bit-identical at 32768, and
    assertion 2 is what fails on it.

 5. SATURATION. Railed-sample fraction 0.07248 % against a ceiling, and the
    engine's own `[AUDIO] mainbus clip` accounting (4578/6329600 = 0.07233 %, worst
    pre-clamp magnitude 35951 = +0.81 dBFS) against ceilings of its own. The two
    are different measurements — the mixer's count is of the master-bus accumulate
    wanting to exceed full scale, the WAV's is of samples that actually ended at
    the rail — and a gain applied after the mixer moves the second without moving
    the first. That asymmetry is itself diagnostic, and this check prints both.

    NOTE: the peak IS full scale (32768). This check does NOT assert "peak below
    full scale"; that assertion would FAIL on correct audio. DKR's mix legitimately
    reaches the rail and the RSP saturates on hardware too.

 6. TRUE PEAK, 4x oversampled with a windowed-sinc interpolator around every
    near-peak region. Measured L +1.002 dBFS, R +1.478 dBFS: the emitted s16 rails
    at 0.0 dBFS but the underlying waveform overshoots between samples, and that
    overshoot is what a real reconstruction filter (and every downstream resampler)
    actually produces. Sample peak cannot see it, because sample peak is pinned.

 7. PER-BAND RMS, absolute, in eight bands from a 192-window Hann/FFT estimate.
    Whole-capture RMS is one number and can be held constant by two errors that
    cancel; a band table cannot. It also localises a fault — a filter-stage or
    resampler gain error moves the top bands and leaves the bottom alone.

 8. PER-SLICE RMS, fifteen fixed 10 s slices. The whole-capture average is
    dominated by the loudest passages; a bias confined to menu SFX, or to one
    music sequence, is visible here and nowhere else.

The frozen baseline is a PORT-SIDE baseline. It freezes what this engine does; it
does not by itself prove that what it does is right. That is what section 9 is for.

 9. CONSOLE REFERENCE (opt-in: --reference PATH). The real ROM's own synthesiser
    output, captured from the audio-interface DMA stream inside the instrumented
    ares of `docs/ORACLE.md` (`MDKR64_ARES_AUDIO_DUMP`, tapped in `AI::sample()`
    before ares converts to float and before any host driver). When it is given,
    the port arm is re-run on the SAME oracle route the console capture used —
    same cadence, same field count, same taps — then envelope-aligned and
    compared as an RMS ratio, whole and per band.

    Refreshed after the retail vehicle-audio path was restored: console
    -11.924 dBFS, port -11.908 dBFS, **port/console +0.016 dB** over a 153.16 s
    aligned overlap (lag -5.65 s, envelope correlation +0.7816). Per-band
    deltas remain within 3.425 dB; see `docs/open-items/audio.md`.

    Tolerances here are deliberately loose (+-1.5 dB broadband, +-6 dB per band):
    the two runners do NOT hold frame-precise alignment through a multi-tap menu
    route (`docs/ORACLE.md` limitation 3), so the overlap is the same *program*
    but not the same *instants*. Engine-level controls independently prove the
    frozen port-side baseline can fail; the ROM-derived reference remains an
    opt-in comparison rather than a synthetic control fixture.

    The reference is ROM-derived and CANNOT be committed, so this section is
    skipped — loudly — when no path is given, and the registered gate runs the
    frozen-baseline half only.

Self-validation — this check is proven to be able to fail
---------------------------------------------------------
SIGNAL-LEVEL (run by default; --no-controls to skip). The same analysis is re-run
over deliberately re-scaled copies of the real capture. Each control MUST trip at
least one assertion; this check FAILS if any control passes:

    +3.0 dB, -3.0 dB   the magnitude the plan names
    +1.5 dB, -1.5 dB   half of it — proves the band is tighter than 1.5 dB and is
                       not a rubber ruler that only catches gross errors

ENGINE-LEVEL (`--control`, each must exit 1). `MDKR_AUDIO_TEST_GAIN_DB` scales the
synthesised PCM inside `dkr_audio_service_tick()`, before the engine's own RMS accounting,
before the dump, and before the sink — so it perturbs the real signal path rather
than the analysis. The seam REFUSES to act whenever a host output device is open,
so it can never make sound; and with the variable unset the capture is bit-identical
to a build compiled without it (verified).

    --control gain+3   -> whole RMS -10.058 dBFS (+2.799 dB), crest 10.058 dB,
                          rails 1.21170 % — assertions 2, 3, 5, 7 and 8 fail
                          (the peak stays pinned at the rail, so 4 cannot see it —
                          that pinning is exactly what assertion 4 exists to hold)
    --control gain-3   -> whole RMS -15.857 dBFS (-3.000 dB), peak 23198 —
                          assertions 2, 3, 4, 6, 7 and 8 fail

What this does NOT cover
------------------------
 * Anything about level in the browser/AudioWorklet sink or the SDL queue path. No
   device is opened here, so the host output stage is outside what this can see.
 * Whether the *console* reference itself is right. ares is an emulator; its RSP
   audio microcode is an implementation, not the silicon. It is a far better
   reference than none, and it is the same reference `docs/ORACLE.md` already uses
   for video and racer state, but it is not a hardware line-out capture.
 * Perceived loudness. Everything here is RMS/peak in the sample domain; no
   K-weighting, no LUFS. That is deliberate — the question is whether the port
   reproduces the ROM's numbers, not whether the ROM's numbers are pleasant.

Usage:
    tests/check_audio_level_reference.py
    tests/check_audio_level_reference.py --keep-audio /tmp/a  # ROM-DERIVED capture
    tests/check_audio_level_reference.py --no-controls
    tests/check_audio_level_reference.py --control gain+3     # must exit 1
    tests/check_audio_level_reference.py --control gain-3     # must exit 1
    tests/check_audio_level_reference.py --reference build/ares-oracle/console.raw

Always runs muted + headless (MDKR_AUDIO=0 and --headless-frames), per
tests/README.md.
"""
import argparse
import array
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import wave

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

# ---- fixture ---------------------------------------------------------------
SCRIPT = "tests/input_scripts/race_drive_time_trial.txt"
FRAMES = 4300
FULL_SCALE = 32768.0

# ---- assertion 1: format and length ---------------------------------------
WANT_RATE = 22050
WANT_CHANNELS = 2
WANT_WIDTH = 2
WANT_FRAMES = 3164800
FRAMES_TOL = 8192           # ~0.37 s; the pump's last block may be short

# ---- assertions 2-4: level ------------------------------------------------
# These baselines were frozen before the cave SFX reverb bus was corrected,
# so the true Ancient Lake level is now ~+0.084 dB (whole -12.773, L -12.769,
# R -12.776, crest 12.773): a few Ancient Lake SFX carry authored fxmix and now
# use the big-room reverb the original routes them to. The shift is 10x inside
# RMS_TOL_DB, so the gate stays green; the routing itself is guarded directly by
# check_cave_reverb_bus.py. A full baseline refresh to the corrected values is a
# deliberate follow-up (no --update mode exists yet), not done piecemeal here.
BASE_RMS_DBFS = -12.857     # 7457.9 / 32768
BASE_RMS_L_DBFS = -12.857   # 7457.3
BASE_RMS_R_DBFS = -12.856   # 7458.6
RMS_TOL_DB = 1.0
RMS_CHANNEL_TOL_DB = 1.2

BASE_CREST_DB = 12.857      # sample peak (32768, 0.0 dBFS) - whole RMS
# The capture rails, so its sample peak is 32768 == 0.0 dBFS exactly. One
# quantisation step at s16 is 0.00027 dB; this only tolerates measurement noise.
PEAK_FULL_SCALE_TOL_DB = 0.01

# ---- assertion 5: saturation ----------------------------------------------
BASE_RAIL_FRAC = 0.0007248  # 4588 / 6329600
RAIL_FRAC_CEIL = 0.0030     # 4.1x the baseline; +3 dB gives 0.01212
CLIP_FRAC_CEIL = 0.0030     # engine's own mainbus clip rate (measured 0.00072)
OVERSHOOT_DB_CEIL = 4.0     # worst pre-clamp magnitude, dBFS (measured +0.81)

# ---- assertion 6: true peak ----------------------------------------------
BASE_TRUE_PEAK_L_DBFS = +1.002
BASE_TRUE_PEAK_R_DBFS = +1.478
TRUE_PEAK_TOL_DB = 2.0
TRUE_PEAK_OVERSAMPLE = 4
TRUE_PEAK_HALF_TAPS = 16
TRUE_PEAK_REGION_CAP = 3000
TRUE_PEAK_TRIGGER = 0.85    # fraction of sample peak that opens a region

# ---- assertion 7: per-band RMS -------------------------------------------
BANDS_HZ = (0, 100, 200, 400, 800, 1600, 3200, 6400, 11025)
BASE_BAND_DBFS = (-17.949, -19.357, -21.587, -21.877,
                  -22.934, -24.898, -27.279, -32.869)
BAND_TOL_DB = 2.5
BAND_WINDOWS = 192
BAND_FFT = 2048

# ---- assertion 8: per-slice RMS ------------------------------------------
SLICE_SECONDS = 10
BASE_SLICE_DBFS = (-21.790, -19.440, -13.792, -11.569, -12.179,
                   -15.205, -16.009, -15.532, -15.684, -12.008,
                   -10.362, -10.372, -10.729, -10.829, -10.803)
SLICE_TOL_DB = 2.0
SLICE_MIN_FRAMES = 22050    # a slice shorter than 1 s is not scored

# ---- assertion 9: console reference (opt-in) -----------------------------
# Recorded once the ares audio lane produced a capture; see docs/open-items/audio.md.
# The tolerance is deliberately wide: the two runners do NOT hold frame-precise
# alignment through a multi-tap menu route (docs/ORACLE.md limitation 3), so the
# aligned overlap is the same *program* but not the same *instants*.
REFERENCE_RATIO_TOL_DB = 1.5
REFERENCE_BAND_TOL_DB = 6.0
REFERENCE_ALIGN_MAX_S = 30.0
REFERENCE_ALIGN_HOP_S = 0.05
REFERENCE_MIN_OVERLAP_S = 20.0

CONTROLS_DB = (3.0, -3.0, 1.5, -1.5)


def dbfs(value):
    return 20.0 * math.log10(value / FULL_SCALE) if value > 0 else -200.0


# ==========================================================================
# capture
# ==========================================================================
def run_capture(build, rom, script, frames, wav, gain_db=None, timeout=600,
                extra_env=None):
    """One headless run. NEVER without --headless-frames; MDKR_AUDIO=0 on top."""
    env = dict(os.environ)
    env["MDKR_AUDIO"] = "0"
    env["MDKR_AUDIO_RMS"] = "1"
    env["MDKR_AUDIO_DUMP"] = wav
    # Set in BOTH directions: reverb is on in the baseline, and an inherited
    # MDKR_AUDIO_REVERB=0 would silently move every number below by ~1 dB.
    env["MDKR_AUDIO_REVERB"] = "1"
    if gain_db is None:
        env.pop("MDKR_AUDIO_TEST_GAIN_DB", None)
    else:
        env["MDKR_AUDIO_TEST_GAIN_DB"] = repr(gain_db)
    cmd = [build, "--headless-frames", str(frames), "--input-script", script,
           "--rom", rom]
    with tempfile.TemporaryDirectory(
            prefix="mdkr_audio_level_runtime_") as run_dir:
        env["MDKR_VIDEO_CONFIG_PATH"] = os.path.join(run_dir, "video.ini")
        env["MDKR_SAVE_DIR"] = os.path.join(run_dir, "save")
        if extra_env:
            env.update(extra_env)
        proc = subprocess.run(cmd, env=env, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=timeout)
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


CLIP_RE = re.compile(r"\[AUDIO\] mainbus clip: (\d+)/(\d+) samples .*? "
                     r"magnitude (\d+) \(([-0-9.]+) dBFS\)")
GUARD_RE = re.compile(r"\[AUDIO\] fx-guard trips=(\d+)")
TOTAL_RE = re.compile(r"\[AUDIO\] TOTAL frames=(\d+) samples=(\d+) "
                      r"rms=([0-9.]+) peak=(\d+)")
GAIN_RE = re.compile(r"\[AUDIO\] TEST GAIN INJECTED: ([-+0-9.]+) dB")


def parse_engine(out):
    info = {"clip": None, "guard": None, "total": None, "test_gain": None}
    for line in out.splitlines():
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
            continue
        m = GAIN_RE.search(line)
        if m:
            info["test_gain"] = float(m.group(1))
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
    return a, rate, ch, width


def load_raw_s16(path, rate):
    with open(path, "rb") as f:
        raw = f.read()
    usable = (len(raw) // 4) * 4
    a = array.array("h")
    a.frombytes(raw[:usable])
    if sys.byteorder == "big":
        a.byteswap()
    return a, rate, 2, 2


def load_reference(path):
    """WAV, or raw LE s16 stereo with the rate in a `<path>.rate` sidecar."""
    with open(path, "rb") as f:
        head = f.read(12)
    if head[:4] == b"RIFF" and head[8:12] == b"WAVE":
        return load_wav(path)
    rate = WANT_RATE
    sidecar = path + ".rate"
    if os.path.exists(sidecar):
        with open(sidecar, "r") as f:
            rate = int(f.read().strip().split()[0])
    return load_raw_s16(path, rate)


# ==========================================================================
# measurement
# ==========================================================================
def rms(seq):
    if len(seq) == 0:
        return 0.0
    total = 0
    for v in seq:
        total += v * v
    return math.sqrt(total / len(seq))


def peak_of(seq):
    if len(seq) == 0:
        return 0
    return max(max(seq), -min(seq))


def rail_count(seq):
    return sum(1 for v in seq if v >= 32767 or v <= -32768)


def fft_inplace(re, im):
    n = len(re)
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j ^= bit
        if i < j:
            re[i], re[j] = re[j], re[i]
            im[i], im[j] = im[j], im[i]
    length = 2
    while length <= n:
        angle = -2.0 * math.pi / length
        wr, wi = math.cos(angle), math.sin(angle)
        for i in range(0, n, length):
            cr, ci = 1.0, 0.0
            half = length // 2
            for k in range(i, i + half):
                ur, ui = re[k], im[k]
                vr = re[k + half] * cr - im[k + half] * ci
                vi = re[k + half] * ci + im[k + half] * cr
                re[k], im[k] = ur + vr, ui + vi
                re[k + half], im[k + half] = ur - vr, ui - vi
                cr, ci = cr * wr - ci * wi, cr * wi + ci * wr
        length <<= 1


def band_rms(mono, rate, nwin=BAND_WINDOWS, size=BAND_FFT):
    """Absolute per-band RMS from evenly spaced Hann-windowed FFTs.

    Parseval, undone for the window's power gain, so the bands sum (to within the
    band-edge quantisation) back to the signal's own mean square — which the caller
    prints as a self-check. Evenly spaced windows make the estimate deterministic
    for a given capture.
    """
    n = size
    hann = [0.5 - 0.5 * math.cos(2.0 * math.pi * i / n) for i in range(n)]
    wpow = sum(h * h for h in hann) / n
    avail = len(mono) - n
    if avail <= 0:
        return None, 0
    step = max(1, avail // nwin)
    edges = []
    for b in range(len(BANDS_HZ) - 1):
        k0 = int(math.ceil(BANDS_HZ[b] * n / rate))
        k1 = min(n // 2, int(math.floor(BANDS_HZ[b + 1] * n / rate)))
        edges.append((k0, k1))
    acc = [0.0] * len(edges)
    used = 0
    for start in range(0, avail, step):
        if used >= nwin:
            break
        re = [mono[start + i] * hann[i] for i in range(n)]
        im = [0.0] * n
        fft_inplace(re, im)
        for b, (k0, k1) in enumerate(edges):
            energy = 0.0
            for k in range(k0, k1 + 1):
                p = re[k] * re[k] + im[k] * im[k]
                energy += p if (k == 0 or k == n // 2) else 2.0 * p
            acc[b] += energy / (n * n) / wpow
        used += 1
    if used == 0:
        return None, 0
    return [math.sqrt(v / used) for v in acc], used


def _sinc_taps(oversample, half):
    taps = []
    for phase in range(1, oversample):
        frac = phase / float(oversample)
        row = []
        for k in range(-half, half + 1):
            x = k - frac
            s = 1.0 if abs(x) < 1e-12 else math.sin(math.pi * x) / (math.pi * x)
            w = 0.5 - 0.5 * math.cos(2.0 * math.pi * (k + half) / (2 * half))
            row.append(s * w)
        total = sum(row)
        taps.append([v / total for v in row])
    return taps


def true_peak(channel):
    """4x-oversampled peak, evaluated only around near-peak regions.

    Oversampling 3.1 M samples in pure Python is not affordable and is not
    necessary: an inter-sample overshoot can only occur next to a large sample, so
    interpolating a neighbourhood of every sample within TRUE_PEAK_TRIGGER of the
    sample peak finds it. The region count is reported so a capture whose peaks
    became too numerous to scan cannot silently under-report.
    """
    pk = peak_of(channel)
    if pk == 0:
        return 0.0, 0, 0
    taps = _sinc_taps(TRUE_PEAK_OVERSAMPLE, TRUE_PEAK_HALF_TAPS)
    half = TRUE_PEAK_HALF_TAPS
    thr = pk * TRUE_PEAK_TRIGGER
    n = len(channel)
    best = float(pk)
    regions = 0
    i = 0
    while i < n:
        v = channel[i]
        a = -v if v < 0 else v
        if a >= thr:
            regions += 1
            if regions > TRUE_PEAK_REGION_CAP:
                break
            for row in taps:
                acc = 0.0
                for k in range(-half, half + 1):
                    idx = i + k
                    if 0 <= idx < n:
                        acc += channel[idx] * row[k + half]
                if abs(acc) > best:
                    best = abs(acc)
            i += TRUE_PEAK_OVERSAMPLE
        else:
            i += 1
    return best, regions, pk


def measure(samples, rate):
    frames = len(samples) // WANT_CHANNELS
    left = samples[0::2]
    right = samples[1::2]
    mono = [(left[i] + right[i]) * 0.5 for i in range(frames)]
    r_all = rms(samples)
    pk = peak_of(samples)
    bands, nwin = band_rms(mono, rate)
    tp_l, reg_l, _ = true_peak(left)
    tp_r, reg_r, _ = true_peak(right)
    slices = []
    span = SLICE_SECONDS * rate
    for start in range(0, frames, span):
        seg = samples[start * 2:(start + span) * 2]
        if len(seg) // 2 < SLICE_MIN_FRAMES:
            continue
        slices.append(rms(seg))
    return {
        "frames": frames,
        "rms": r_all,
        "rms_l": rms(left),
        "rms_r": rms(right),
        "peak": pk,
        "crest_db": dbfs(pk) - dbfs(r_all),
        "rails": rail_count(samples),
        "true_peak_l": tp_l,
        "true_peak_r": tp_r,
        "true_peak_regions": (reg_l, reg_r),
        "bands": bands,
        "band_windows": nwin,
        "slices": slices,
        "mono_rms": rms(mono),
    }


def scaled_copy(samples, db):
    g = 10.0 ** (db / 20.0)
    out = array.array("h", bytes(len(samples) * 2))
    for i, v in enumerate(samples):
        x = v * g
        if x > 32767.0:
            x = 32767.0
        elif x < -32768.0:
            x = -32768.0
        out[i] = int(x - 0.5) if x < 0 else int(x + 0.5)
    return out


# ==========================================================================
# assertions
# ==========================================================================
def assert_format(rate, ch, width, m, fail, note):
    if (rate, ch, width) != (WANT_RATE, WANT_CHANNELS, WANT_WIDTH):
        fail("capture is %d Hz / %d ch / %d-bit, want %d/%d/%d"
             % (rate, ch, width * 8, WANT_RATE, WANT_CHANNELS, WANT_WIDTH * 8))
    if abs(m["frames"] - WANT_FRAMES) > FRAMES_TOL:
        fail("capture is %d sample-frames, want %d +-%d — every level number "
             "below is an average, and an average over the wrong amount of "
             "material is not the measurement it claims to be"
             % (m["frames"], WANT_FRAMES, FRAMES_TOL))
    note("format %d Hz / %d ch / %d-bit, %d sample-frames = %.2f s"
         % (rate, ch, width * 8, m["frames"], m["frames"] / float(rate)))


def assert_level(m, fail, note):
    got = dbfs(m["rms"])
    note("whole-capture RMS %.1f = %+.3f dBFS (baseline %+.3f, delta %+.3f dB)"
         % (m["rms"], got, BASE_RMS_DBFS, got - BASE_RMS_DBFS))
    if abs(got - BASE_RMS_DBFS) > RMS_TOL_DB:
        fail("whole-capture RMS %+.3f dBFS is %+.3f dB off the frozen baseline "
             "%+.3f dBFS (tolerance +-%.1f dB)"
             % (got, got - BASE_RMS_DBFS, BASE_RMS_DBFS, RMS_TOL_DB))
    for label, value, base in (("L", m["rms_l"], BASE_RMS_L_DBFS),
                               ("R", m["rms_r"], BASE_RMS_R_DBFS)):
        d = dbfs(value)
        note("  %s RMS %.1f = %+.3f dBFS (baseline %+.3f, delta %+.3f dB)"
             % (label, value, d, base, d - base))
        if abs(d - base) > RMS_CHANNEL_TOL_DB:
            fail("%s-channel RMS %+.3f dBFS is %+.3f dB off baseline %+.3f "
                 "(tolerance +-%.1f dB)"
                 % (label, d, d - base, base, RMS_CHANNEL_TOL_DB))


def assert_crest(m, fail, note):
    # crest = dbfs(peak) - dbfs(rms), and assert_level already pins dbfs(rms)
    # to +-RMS_TOL_DB of the same baseline this crest baseline was derived from.
    # With both tolerances at 1.0 dB the crest inequality restates assert_level
    # and can only fail when the peak moves, so assert the peak directly. That
    # is the independent fact: the program touches full scale, which is why a
    # louder build closes the crest instead of raising the peak.
    note("sample peak %d = %+.3f dBFS, crest %.3f dB (baseline %.3f)"
         % (m["peak"], dbfs(m["peak"]), m["crest_db"], BASE_CREST_DB))
    peak_dbfs = dbfs(m["peak"])
    if peak_dbfs < -PEAK_FULL_SCALE_TOL_DB:
        fail("sample peak %d = %+.3f dBFS is more than %.2f dB below full "
             "scale: the capture no longer reaches the rail the RMS baseline "
             "and every crest figure in this check are referenced against"
             % (m["peak"], peak_dbfs, PEAK_FULL_SCALE_TOL_DB))


def assert_saturation(m, info, fail, note):
    total = m["frames"] * WANT_CHANNELS
    frac = m["rails"] / float(total) if total else 0.0
    note("railed samples %d/%d = %.5f %% (baseline %.5f %%, ceiling %.5f %%)"
         % (m["rails"], total, frac * 100.0, BASE_RAIL_FRAC * 100.0,
            RAIL_FRAC_CEIL * 100.0))
    if frac > RAIL_FRAC_CEIL:
        fail("%.5f %% of samples are at +-full scale, ceiling %.5f %%"
             % (frac * 100.0, RAIL_FRAC_CEIL * 100.0))
    if info["clip"] is None:
        fail("no [AUDIO] mainbus clip line — the engine's own saturation "
             "accounting is what distinguishes a mixer-side gain error from a "
             "post-mixer one, and this check will not run without it")
    else:
        hits, mix_total, worst, over_db = info["clip"]
        cf = hits / float(mix_total) if mix_total else 0.0
        note("engine mainbus clip %d/%d = %.5f %%, worst pre-clamp %d = %+.2f dBFS"
             % (hits, mix_total, cf * 100.0, worst, over_db))
        if cf > CLIP_FRAC_CEIL:
            fail("engine mainbus clip rate %.5f %% exceeds ceiling %.5f %%"
                 % (cf * 100.0, CLIP_FRAC_CEIL * 100.0))
        if over_db > OVERSHOOT_DB_CEIL:
            fail("worst master-bus pre-clamp overshoot %+.2f dBFS exceeds "
                 "ceiling %+.2f dBFS" % (over_db, OVERSHOOT_DB_CEIL))
    if info["guard"] is None:
        fail("no [AUDIO] fx-guard trips line")
    elif info["guard"] != 0:
        fail("fx-guard tripped %d time(s): a delay-line DMA left its allocation "
             "and was clamped, which changes the wet level" % info["guard"])


def assert_true_peak(m, fail, note):
    for label, value, base, regions in (
            ("L", m["true_peak_l"], BASE_TRUE_PEAK_L_DBFS, m["true_peak_regions"][0]),
            ("R", m["true_peak_r"], BASE_TRUE_PEAK_R_DBFS, m["true_peak_regions"][1])):
        d = dbfs(value)
        note("true peak %s %.1f = %+.3f dBFS (baseline %+.3f, delta %+.3f dB, "
             "%d region(s))" % (label, value, d, base, d - base, regions))
        if regions == 0:
            fail("true-peak scan for %s found no near-peak region; the estimate "
                 "would be vacuous" % label)
        if abs(d - base) > TRUE_PEAK_TOL_DB:
            fail("%s true peak %+.3f dBFS is %+.3f dB off baseline %+.3f "
                 "(tolerance +-%.1f dB)"
                 % (label, d, d - base, base, TRUE_PEAK_TOL_DB))


def assert_bands(m, fail, note):
    if not m["bands"]:
        fail("per-band RMS could not be computed")
        return
    note("per-band RMS over %d windows (sum-of-bands %.1f vs mono RMS %.1f):"
         % (m["band_windows"], math.sqrt(sum(v * v for v in m["bands"])),
            m["mono_rms"]))
    for b, value in enumerate(m["bands"]):
        d = dbfs(value)
        base = BASE_BAND_DBFS[b]
        note("  %5d-%5d Hz  %9.2f  %+8.3f dBFS (baseline %+8.3f, delta %+.3f)"
             % (BANDS_HZ[b], BANDS_HZ[b + 1], value, d, base, d - base))
        if abs(d - base) > BAND_TOL_DB:
            fail("band %d-%d Hz RMS %+.3f dBFS is %+.3f dB off baseline %+.3f "
                 "(tolerance +-%.1f dB)"
                 % (BANDS_HZ[b], BANDS_HZ[b + 1], d, d - base, base, BAND_TOL_DB))


def assert_slices(m, fail, note):
    got = m["slices"]
    if len(got) != len(BASE_SLICE_DBFS):
        fail("capture yields %d scored %d s slices, baseline has %d — the route "
             "changed length, so the per-slice comparison would compare different "
             "program material" % (len(got), SLICE_SECONDS, len(BASE_SLICE_DBFS)))
        return
    note("per-slice RMS (%d s slices):" % SLICE_SECONDS)
    for i, value in enumerate(got):
        d = dbfs(value)
        base = BASE_SLICE_DBFS[i]
        note("  t=%6.1f s  %9.2f  %+8.3f dBFS (baseline %+8.3f, delta %+.3f)"
             % (i * SLICE_SECONDS, value, d, base, d - base))
        if abs(d - base) > SLICE_TOL_DB:
            fail("slice %d (t=%d s) RMS %+.3f dBFS is %+.3f dB off baseline "
                 "%+.3f (tolerance +-%.1f dB)"
                 % (i, i * SLICE_SECONDS, d, d - base, base, SLICE_TOL_DB))


# ==========================================================================
# assertion 9: console reference
# ==========================================================================
def envelope(mono, window):
    out = []
    for i in range(0, len(mono) - window + 1, window):
        total = 0.0
        for k in range(i, i + window):
            v = mono[k]
            total += -v if v < 0 else v
        out.append(total / window)
    return out


def normalise(values):
    if not values:
        return []
    mu = sum(values) / len(values)
    var = sum((v - mu) * (v - mu) for v in values) / len(values)
    sd = math.sqrt(var)
    if sd < 1e-12:
        return [0.0] * len(values)
    return [(v - mu) / sd for v in values]


def corr_at(a, b, lag):
    if lag >= 0:
        ai, bi = 0, lag
    else:
        ai, bi = -lag, 0
    n = min(len(a) - ai, len(b) - bi)
    if n <= 2:
        return -2.0
    total = 0.0
    for i in range(n):
        total += a[ai + i] * b[bi + i]
    return total / n


def resample_linear(mono, src_rate, dst_rate):
    if src_rate == dst_rate or not mono:
        return list(mono)
    ratio = src_rate / float(dst_rate)
    out_len = int(len(mono) / ratio)
    out = [0.0] * out_len
    for i in range(out_len):
        pos = i * ratio
        j = int(pos)
        frac = pos - j
        out[i] = (mono[j] * (1.0 - frac) + mono[j + 1] * frac
                  if j + 1 < len(mono) else mono[-1])
    return out


def capture_reference_arm(build, rom, tmp, route, gain_db, fail, note):
    """Re-run the port on the SAME oracle route the console capture used.

    The default fixture route is a native input script; ares drives its own
    frame-numbered injection route derived from the shared JSON, so a
    console-vs-port level ratio is only meaningful when the port arm is the
    route's own native arm — same cadence, same field count, same taps. This
    reproduces `tools/run_oracle.sh`'s native invocation exactly, including the
    isolated save directory.
    """
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    route_json = os.path.join(root, "tools", "oracle_routes", route + ".json")
    if not os.path.exists(route_json):
        fail("no such oracle route: %s" % route_json)
        return None, None
    with open(route_json, "r") as f:
        spec = json.load(f)
    frames = int(spec.get("native", {}).get("frames", 0))
    cadence = spec.get("native_cadence", "original")
    fields = str(spec.get("native_synth_fields", 2))
    if frames <= 0:
        fail("oracle route %s declares no native frame count" % route)
        return None, None

    script = os.path.join(tmp, "oracle_input.txt")
    proc = subprocess.run(
        [sys.executable, os.path.join(root, "tools", "dkr_oracle_route.py"),
         "native-script", route],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        fail("dkr_oracle_route.py native-script %s failed: %s"
             % (route, proc.stderr.decode("utf-8", "replace")[-500:]))
        return None, None
    with open(script, "wb") as f:
        f.write(proc.stdout)

    saves = os.path.join(tmp, "oracle_saves")
    os.makedirs(saves, exist_ok=True)
    wav = os.path.join(tmp, "oracle.wav")
    note("console reference: re-running the port on oracle route '%s' "
         "(%d frames, cadence %s, %s synth field(s))"
         % (route, frames, cadence, fields))
    rc, out = run_capture(build, rom, script, frames, wav, gain_db=gain_db,
                          extra_env={"MDKR_SIMULATION_CADENCE": cadence,
                                     "MDKR_SYNTH_FIELDS": fields,
                                     "MDKR_SAVE_DIR": saves})
    if rc != 0 or not os.path.exists(wav):
        fail("oracle-route capture exited %d" % rc)
        return None, None
    return wav, parse_engine(out)


def assert_reference(port_samples, port_rate, ref_path, fail, note):
    ref, ref_rate, ref_ch, ref_width = load_reference(ref_path)
    if (ref_ch, ref_width) != (WANT_CHANNELS, WANT_WIDTH):
        fail("console reference is %d ch / %d-bit, want %d ch / %d-bit"
             % (ref_ch, ref_width * 8, WANT_CHANNELS, WANT_WIDTH * 8))
        return
    if ref_rate <= 0:
        fail("console reference has invalid sample rate %d" % ref_rate)
        return
    ref_frames = len(ref) // ref_ch
    note("console reference %s: %d Hz / %d ch, %d sample-frames = %.2f s"
         % (ref_path, ref_rate, ref_ch, ref_frames, ref_frames / float(ref_rate)))
    if ref_frames == 0:
        fail("console reference is empty")
        return
    ref_mono = [(ref[i * ref_ch] + ref[i * ref_ch + 1]) * 0.5
                for i in range(ref_frames)]
    port_frames = len(port_samples) // WANT_CHANNELS
    port_mono = [(port_samples[i * 2] + port_samples[i * 2 + 1]) * 0.5
                 for i in range(port_frames)]
    if ref_rate != port_rate:
        note("  resampling the reference %d Hz -> %d Hz for comparison"
             % (ref_rate, port_rate))
        ref_mono = resample_linear(ref_mono, ref_rate, port_rate)

    hop = max(1, int(round(REFERENCE_ALIGN_HOP_S * port_rate)))
    a = normalise(envelope(ref_mono, hop))
    b = normalise(envelope(port_mono, hop))
    max_lag = int(round(REFERENCE_ALIGN_MAX_S * port_rate / hop))
    best_lag, best = 0, -2.0
    for lag in range(-max_lag, max_lag + 1):
        c = corr_at(a, b, lag)
        if c > best:
            best, best_lag = c, lag
    lag_samples = best_lag * hop
    note("  envelope alignment: lag %+d sample-frames (%.2f s), correlation %+.4f"
         % (lag_samples, lag_samples / float(port_rate), best))

    if lag_samples >= 0:
        ref_start, port_start = 0, lag_samples
    else:
        ref_start, port_start = -lag_samples, 0
    n = min(len(ref_mono) - ref_start, len(port_mono) - port_start)
    if n < REFERENCE_MIN_OVERLAP_S * port_rate:
        fail("aligned overlap is only %.2f s; at least %.0f s is needed for the "
             "ratio to mean anything"
             % (n / float(port_rate), REFERENCE_MIN_OVERLAP_S))
        return
    ref_cut = ref_mono[ref_start:ref_start + n]
    port_cut = port_mono[port_start:port_start + n]
    note("  aligned overlap %.2f s" % (n / float(port_rate)))

    r_ref = math.sqrt(sum(v * v for v in ref_cut) / n)
    r_port = math.sqrt(sum(v * v for v in port_cut) / n)
    ratio = 20.0 * math.log10((r_port + 1e-12) / (r_ref + 1e-12))
    note("  RMS console %.1f (%+.3f dBFS), port %.1f (%+.3f dBFS), "
         "port/console %+.3f dB"
         % (r_ref, dbfs(r_ref), r_port, dbfs(r_port), ratio))
    if abs(ratio) > REFERENCE_RATIO_TOL_DB:
        fail("port is %+.3f dB against the real ROM's own output (tolerance "
             "+-%.1f dB)" % (ratio, REFERENCE_RATIO_TOL_DB))

    ref_bands, _ = band_rms(ref_cut, port_rate)
    port_bands, _ = band_rms(port_cut, port_rate)
    if ref_bands and port_bands:
        for i in range(len(ref_bands)):
            d = 20.0 * math.log10((port_bands[i] + 1e-12) / (ref_bands[i] + 1e-12))
            note("  %5d-%5d Hz  console %+8.3f dBFS  port %+8.3f dBFS  %+.3f dB"
                 % (BANDS_HZ[i], BANDS_HZ[i + 1], dbfs(ref_bands[i]),
                    dbfs(port_bands[i]), d))
            if abs(d) > REFERENCE_BAND_TOL_DB:
                fail("band %d-%d Hz is %+.3f dB against the real ROM (tolerance "
                     "+-%.1f dB)"
                     % (BANDS_HZ[i], BANDS_HZ[i + 1], d, REFERENCE_BAND_TOL_DB))


# ==========================================================================
# controls
# ==========================================================================
def run_control(db, samples, info, rate):
    """Re-run every scale-sensitive assertion over a re-scaled copy.

    Returns the list of assertions that tripped. An empty list means the control
    PASSED the check, which is a failure of this check.
    """
    tripped = []

    def fail(msg):
        tripped.append(msg)

    def note(_msg):
        pass

    m = measure(scaled_copy(samples, db), rate)
    assert_level(m, fail, note)
    assert_crest(m, fail, note)
    assert_saturation(m, info, fail, note)
    assert_true_peak(m, fail, note)
    assert_bands(m, fail, note)
    assert_slices(m, fail, note)
    return tripped


# ==========================================================================
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default=DEFAULT_BUILD_DIR)
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("--script", default=SCRIPT)
    ap.add_argument("--frames", type=int, default=FRAMES)
    ap.add_argument("--keep-audio", default=None,
                    help="write the capture here and keep it. ROM-DERIVED — never "
                         "commit a .wav (default: temp dir, deleted)")
    ap.add_argument("--no-controls", action="store_true",
                    help="skip the signal-level self-validation arms (they are "
                         "what prove this check can fail)")
    ap.add_argument("--control", choices=("gain+3", "gain-3"), default=None,
                    help="ENGINE-LEVEL positive control: re-run the binary with "
                         "MDKR_AUDIO_TEST_GAIN_DB and confirm this check reports "
                         "FAIL (exit 1)")
    ap.add_argument("--reference", default=None,
                    help="console reference capture (ares MDKR64_ARES_AUDIO_DUMP "
                         "raw s16, or a WAV). ROM-DERIVED — never commit it.")
    ap.add_argument("--reference-route", default="race_state_oracle",
                    help="oracle route the console capture was taken on; the port "
                         "arm is re-run on the same route for the comparison")
    args = ap.parse_args()
    args.build = resolve_binary(args.build)

    for path in (args.build, args.rom, args.script):
        if not os.path.exists(path):
            sys.exit("missing: %s" % path)
    if args.reference and not os.path.exists(args.reference):
        sys.exit("missing: %s" % args.reference)

    gain_db = None
    if args.control == "gain+3":
        gain_db = 3.0
    elif args.control == "gain-3":
        gain_db = -3.0
    if gain_db is not None:
        print("check_audio_level_reference: CONTROL %s — MDKR_AUDIO_TEST_GAIN_DB="
              "%+.1f injected into dkr_audio_service_tick(); this run MUST report FAIL"
              % (args.control, gain_db))

    tmp = args.keep_audio or tempfile.mkdtemp(prefix="mdkr64-level-")
    os.makedirs(tmp, exist_ok=True)
    failures = []
    notes = []

    def fail(msg):
        failures.append(msg)

    def note(msg):
        notes.append(msg)

    try:
        wav = os.path.join(tmp, "level.wav")
        rc, out = run_capture(args.build, args.rom, args.script, args.frames,
                              wav, gain_db=gain_db)
        if rc != 0:
            print(out[-4000:])
            sys.exit("capture run exited %d" % rc)
        info = parse_engine(out)
        if gain_db is None and info["test_gain"] is not None:
            fail("MDKR_AUDIO_TEST_GAIN_DB is active (%+.3f dB) in what is supposed "
                 "to be the reference run — the baseline would be meaningless"
                 % info["test_gain"])
        if gain_db is not None and info["test_gain"] is None:
            sys.exit("control run did not report an injected test gain; the seam "
                     "is not in this binary, so the control proves nothing")
        if not os.path.exists(wav):
            sys.exit("no capture at %s" % wav)

        samples, rate, ch, width = load_wav(wav)
        m = measure(samples, rate)

        assert_format(rate, ch, width, m, fail, note)
        assert_level(m, fail, note)
        assert_crest(m, fail, note)
        assert_saturation(m, info, fail, note)
        assert_true_peak(m, fail, note)
        assert_bands(m, fail, note)
        assert_slices(m, fail, note)

        if args.reference:
            ref_wav, _ = capture_reference_arm(
                args.build, args.rom, tmp, args.reference_route, gain_db,
                fail, note)
            if ref_wav:
                arm, arm_rate, _, _ = load_wav(ref_wav)
                assert_reference(arm, arm_rate, args.reference, fail, note)
        else:
            note("console reference: SKIPPED (no --reference). This run proves "
                 "the port has not DRIFTED, not that its absolute level is "
                 "right. See docs/open-items/audio.md.")

        if not args.no_controls and gain_db is None:
            note("signal-level controls (each must trip at least one assertion):")
            for db in CONTROLS_DB:
                tripped = run_control(db, samples, info, rate)
                if not tripped:
                    fail("CONTROL %+.1f dB PASSED the check — a flat %+.1f dB "
                         "level error is invisible to this gate, so it cannot do "
                         "the job it claims to do" % (db, db))
                else:
                    note("  %+.1f dB -> %d assertion(s) tripped: %s"
                         % (db, len(tripped), tripped[0].split(" (")[0]))
    finally:
        if not args.keep_audio:
            shutil.rmtree(tmp, ignore_errors=True)

    for line in notes:
        print(line)
    print("")
    if failures:
        for line in failures:
            print("FAIL: %s" % line)
        print("\ncheck_audio_level_reference: FAIL (%d)" % len(failures))
        return 1
    print("check_audio_level_reference: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
