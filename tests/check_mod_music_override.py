#!/usr/bin/env python3
"""A content pack's `music/<id>.wav` replaces the sequence — and only its sound.

Why this exists
---------------
`platform/mod_music.c` lets a player drop `music/35.wav` into a pack and hear it
instead of sequence 35. The dangerous way to build that is to stop running the
sequence player: the game reads `music_current_sequence()` to gate cutscene
fades and dynamic channel sets, `music_animation_fraction()` paces the title
animation from the sequencer's own tempo, and the event queue is what drives
both. A replacement that skipped the player would look and sound fine on the
attract screen and quietly change gameplay everywhere else.

So the replacement is specified as presentation-only: the sequence still starts,
still advances, still posts its events; its OUTPUT GAIN goes to zero and the
decoded WAV is added to the same bus. This check is what makes that specification
a fact rather than an intention — assertion 5 is the one that matters most, and
the other four exist so it cannot pass vacuously.

Nothing here opens an audio device. `--headless-frames` returns before SDL audio
is touched and `MDKR_AUDIO=0` is set on top of it, in every arm. (`MDKR_AUDIO=off`
would be a silent no-op — only the digit `0` disables.) Synthesis still runs, so
the whole path is exercised, and `MDKR_AUDIO_DUMP` taps the PCM.

No ROM audio is involved in what this check asserts on. The WAV the pack ships is
synthesised here, in `synth_wav_bytes()`, from a sine table: it is the check's own
signal, so "the pack's music is what came out" can be measured against something
this file authored. Every pack is written into a throwaway directory that is also
the run's working directory, so `mods/` never appears inside the repository.

The route
---------
Boot with no input script. The attract-mode intro starts sequence 35 at output
sample 66,240 (three seconds in) and holds it for the rest of the capture, which
is the earliest music the game reaches and therefore the cheapest to gate on.
320 frames is 235,520 sample-frames, of which ~170,000 are music.

Seven arms, ~0.6 s each:

    A  no pack            Music 100    [SIMHASH] v3
    B  directory pack     Music 100    [SIMHASH] v3
    C  the same pack, zipped
                          Music 100    [SIMHASH] v3
    D  directory pack     Music 0
    E  no pack            Music 0
    F  directory pack     Music 50
    G  directory pack     Music 100    Content.PacksEnabled=0    [SIMHASH] v3

Effects volume is 0 in every arm. That is not cosmetic: with the sound-effect bus
silent and the sequence player muted, the music window contains the replacement
and nothing else, which is what lets assertion 2 measure an exact waveform match
instead of a spectral resemblance.

What is asserted (measured values are from the reference run, US v80)
---------------------------------------------------------------------
 1. REACHABILITY. Every arm exits 0, produces a 22050 Hz / 2 ch / 16-bit capture,
    and reaches sequence 35. The pack arms log the replacement by name.

 2. THE PACK'S OWN WAV IS WHAT PLAYS. Over a two-second window inside the music,
    a least-squares fit of the synthesised tone's frequency accounts for
    essentially all of arm B: amplitude 9448.6 with a residual RMS of 0.6, i.e.
    0.006 % of the signal. The same fit against arm A, the ROM sequence, finds
    amplitude 21.7 in a window whose RMS is 1788.8 — the baseline is not this
    waveform, and the pack arm is almost nothing else.

 3. NOT THE BASELINE. Arm B's capture is not arm A's, and not marginally: the
    two differ over the whole music window.

 4. THE MUSIC SLIDER GOVERNS IT, exactly as it governs the sequence. At Music 50
    the fitted amplitude is half what it is at Music 100 (measured 0.500x). At
    Music 0 the replacement is silent, and arm D's capture is byte-identical to
    arm E's — a pack installed at Music 0 produces the same bytes as no pack at
    all. The 50 % arm is why the 0 % arm is not vacuous: it proves the gain is a
    real path the slider drives, not just a track that never started.

 5. PRESENTATION-ONLY. The `[SIMHASH]` v3 stream is byte-identical across arms A,
    B and C — 320 rows, not one bit different with a music pack installed. The
    engine's own `[AUDIO] music seq=... uspt=...` trace is identical too, so the
    sequence player is demonstrably still running, still advancing, and still
    reporting the same live tempo at the same sample offsets.

 6. A ZIPPED PACK IS A PACK. Arm C installs the same manifest and the same WAV as
    a single `.zip`, with no directory anywhere in `mods/`, and produces a capture
    byte-identical to arm B's. Discovery, manifest parsing and content reads all
    go through `platform/mod_source.c`, and this is the assertion that keeps the
    two source kinds honest about it.

 7. "CUSTOM CONTENT" GOVERNS MUSIC TOO. Arm G installs the same pack with
    Content.PacksEnabled=0 at launcher rank, which is where a player's saved
    setting sits. The pack is still found and still reported active, and the
    replacement is never claimed: the capture is byte-identical to arm A's, so
    a player who switches custom content off hears the game's own sequence and
    not the pack's WAV. The arm exists because the setting used to reach the
    texture layer alone -- a checkbox called "Custom content" that silently
    left the music replaced was the same class of lie as the one the settings
    checkbox told before it was published live at all.

    What arm G does NOT claim is that the switch cuts a replacement off
    mid-track. It does not, deliberately: mod_music.h reads it at
    mdkr_mod_music_begin(), because muting the sequence player is a one-way
    redirection and stopping a replacement partway would leave silence where
    the game's own music should be. Live coverage of the setting's texture half
    is tests/check_mod_texture_override.py.

Self-validation — this check is proven to be able to fail
---------------------------------------------------------
POSITIVE CONTROL (run by hand, not by this file): stub `mdkr_mod_music_begin()`
in `platform/mod_music.c` to `return 0;` at its first statement, rebuild, and
re-run. The check exits 1 at assertion 1:

    check_mod_music_override: FAIL — pack: the engine never reported the
    replacement

because the store no longer claims the track and the `[MODS] music/35.wav
replaces sequence 35` line is gone. Assertions 2, 3 and 4 fail on the same
build if that reachability line is bypassed — measured on the stubbed binary,
the pack arm's fit is amplitude 21.7 with residual RMS 1788.8, i.e. exactly the
baseline's, so the captures are byte-identical and the 50 % arm's ratio is 1.0
rather than 0.5. Assertion 6 is the one that keeps passing, and honestly so: it
compares two unmodded arms, which are equal for the same reason both are wrong.
Restore the file and rebuild afterwards.

Exit 0 = pass.
"""

from __future__ import annotations

import argparse
import array
import hashlib
import io
import math
import os
import subprocess
import sys
import tempfile
import wave
import zipfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

ROOT = Path(__file__).resolve().parent.parent

# ---- fixture ---------------------------------------------------------------
FRAMES = 320                # boot -> attract; sequence 35 from sample 66,240
RATE = 22050                # audi_port_dkr.c DKR_OUTPUT_RATE
CHANNELS = 2
WIDTH = 2

# The sequence the attract intro plays, and therefore the file name the pack has
# to use. Pinned rather than parsed so a route change that stops reaching music
# fails assertion 1 instead of silently gating on nothing.
SEQUENCE_ID = 35

# The synthesised track. One second holds exactly 440 cycles at 22050 Hz, so the
# loop seam is continuous in both value and slope and the whole capture is one
# unbroken tone — which is what lets a single global fit measure it.
TONE_HZ = 440.0
TONE_SECONDS = 1.0
TONE_AMPLITUDE = 12000

# ---- the measurement window ------------------------------------------------
# Inside the music (which starts at 66,240) and clear of its first pump block.
FIT_START_FRAME = 70000
FIT_FRAMES = 44100          # two seconds

# ---- assertion 2 -----------------------------------------------------------
PACK_AMPLITUDE_FLOOR = 5000.0     # measured 9448.6
PACK_RESIDUAL_CEIL = 20.0         # measured 0.6
BASELINE_AMPLITUDE_CEIL = 500.0   # measured 21.7
BASELINE_RMS_FLOOR = 500.0        # measured 1788.8 — anti-vacuity: music is on

# ---- assertion 3 -----------------------------------------------------------
DIFF_RMS_FLOOR = 1000.0           # measured 6945.0
DIFF_FRACTION_FLOOR = 0.90        # measured 0.9999

# ---- assertion 4 -----------------------------------------------------------
HALF_VOLUME_TOL = 0.02            # measured 0.500x
SILENT_AMPLITUDE_CEIL = 1.0       # measured 0.0

BAD_RE = ("[FATAL]", "[CRASH]", "AddressSanitizer",
          "UndefinedBehaviorSanitizer", "runtime error:")

PACK_INI = b"[pack]\nname=Music Test\npriority=100\n"


class CheckError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CheckError(message)


# ==========================================================================
# the pack this check authors for itself
# ==========================================================================
def synth_wav_bytes() -> bytes:
    """A 440 Hz stereo sine as a RIFF/WAVE file. Authored here, start to finish.

    Nothing ROM-derived reaches this function or anything downstream of it: the
    bytes a pack serves in this check are a sine table this file computes, which
    is the only reason 'the capture is the pack's music' is a statement the check
    can make about a specific waveform.
    """
    count = int(RATE * TONE_SECONDS)
    samples = array.array("h")
    for index in range(count):
        value = int(round(TONE_AMPLITUDE *
                          math.sin(2.0 * math.pi * TONE_HZ * index / RATE)))
        for _ in range(CHANNELS):
            samples.append(value)
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as handle:
        handle.setnchannels(CHANNELS)
        handle.setsampwidth(WIDTH)
        handle.setframerate(RATE)
        handle.writeframes(samples.tobytes())
    return buffer.getvalue()


def install_pack(run_dir: Path, kind: str) -> None:
    """Writes the pack into `run_dir/mods`, as a directory or as a zip.

    Both shapes carry byte-identical content, which is what makes arm C's
    comparison against arm B a statement about the source layer and not about
    the two packs happening to be similar.
    """
    if kind == "none":
        return
    mods = run_dir / "mods"
    mods.mkdir(parents=True, exist_ok=True)
    wav = synth_wav_bytes()
    if kind == "dir":
        pack = mods / "TestPack"
        (pack / "music").mkdir(parents=True, exist_ok=True)
        (pack / "pack.ini").write_bytes(PACK_INI)
        (pack / "music" / f"{SEQUENCE_ID}.wav").write_bytes(wav)
        return
    if kind == "zip":
        with zipfile.ZipFile(mods / "TestPack.zip", "w",
                             zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("pack.ini", PACK_INI)
            archive.writestr(f"music/{SEQUENCE_ID}.wav", wav)
        # Proven, not assumed: the zip arm must not be reading a directory that
        # a previous arm left behind.
        require(not any(entry.is_dir() for entry in mods.iterdir()),
                "the zip arm's mods/ contains a directory")
        return
    raise CheckError(f"unknown pack kind {kind!r}")


# ==========================================================================
# capture
# ==========================================================================
class Arm:
    def __init__(self, label, kind, music, hashes, packs=None):
        self.label = label
        self.kind = kind
        self.music = music
        self.hashes = hashes
        # None leaves Content.PacksEnabled at its default; "0" switches custom
        # content off the way a player's saved setting does, at LAUNCHER rank.
        self.packs = packs
        self.samples = array.array("h")
        self.digest = ""
        self.rows: list[str] = []
        self.traces: list[str] = []
        self.mods: list[str] = []


def run_arm(binary: Path, rom: Path, root: Path, arm: Arm, frames: int,
            timeout: int, verbose: bool) -> None:
    """One headless run. NEVER without --headless-frames; MDKR_AUDIO=0 on top."""
    run_dir = root / arm.label
    run_dir.mkdir(parents=True, exist_ok=True)
    install_pack(run_dir, arm.kind)
    capture = run_dir / "capture.wav"

    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        # --headless-frames is the guarantee; this is belt and braces, and the
        # digit is load-bearing (MDKR_AUDIO=off does nothing at all).
        MDKR_AUDIO="0",
        MDKR_AUDIO_RMS="1",
        MDKR_AUDIO_DUMP=str(capture),
        # Pinned in both directions: an inherited MDKR_AUDIO_REVERB=0 would make
        # one arm dry and turn every cross-arm comparison below into a
        # comparison of two different engines.
        MDKR_AUDIO_REVERB="1",
        MDKR_MASTER_VOLUME="100",
        MDKR_MUSIC_VOLUME=str(arm.music),
        # The whole point: with the effects bus off and the sequence player
        # muted, the music window holds the replacement alone.
        MDKR_EFFECTS_VOLUME="0",
        MDKR_VIDEO_CONFIG_PATH=str(run_dir / "video.ini"),
        MDKR_SAVE_DIR=str(run_dir / "save"),
    )
    if arm.hashes:
        env["MDKR_STATE_HASH"] = "3"

    command = [str(binary), "--headless-frames", str(frames), "--rom", str(rom)]
    if arm.packs is not None:
        command += ["--video-launch-set", f"Content.PacksEnabled={arm.packs}"]
    if verbose:
        print(f"$ ({arm.label}) {' '.join(command)}", flush=True)
    process = subprocess.run(
        command, cwd=str(run_dir), env=env, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=timeout, check=False)
    output = process.stdout.decode("utf-8", "replace")
    require(process.returncode == 0,
            f"{arm.label}: engine exited {process.returncode}\n{output[-4000:]}")
    for needle in BAD_RE:
        require(needle not in output,
                f"{arm.label}: runtime diagnostic {needle!r}\n{output[-4000:]}")

    for line in output.splitlines():
        if line.startswith("[SIMHASH]"):
            arm.rows.append(line)
        elif "[AUDIO] music seq=" in line:
            arm.traces.append(line)
        elif "[MODS]" in line:
            arm.mods.append(line)

    require(capture.is_file() and capture.stat().st_size > 44,
            f"{arm.label}: no non-empty capture was produced")
    with wave.open(str(capture), "rb") as handle:
        got = (handle.getframerate(), handle.getnchannels(),
               handle.getsampwidth())
        require(got == (RATE, CHANNELS, WIDTH),
                f"{arm.label}: capture format={got}, "
                f"expected {(RATE, CHANNELS, WIDTH)}")
        arm.samples.frombytes(handle.readframes(handle.getnframes()))
    if sys.byteorder == "big":
        arm.samples.byteswap()
    arm.digest = hashlib.sha256(capture.read_bytes()).hexdigest()


# ==========================================================================
# analysis
# ==========================================================================
def mono_window(samples: array.array, start: int, count: int) -> list[float]:
    """Channel-summed frames [start, start+count). Both channels carry the same
    tone, so summing them raises the measurement's signal without changing what
    is being measured."""
    return [(samples[(start + i) * CHANNELS] +
             samples[(start + i) * CHANNELS + 1]) / 2.0
            for i in range(count)]


def fit_tone(window: list[float], start: int) -> tuple[float, float, float]:
    """Least-squares amplitude of TONE_HZ, the residual RMS after removing it,
    and the window's own RMS.

    `start` is the absolute frame index, so the basis functions carry the same
    phase reference the engine's output does and the fit does not have to search
    for one. Returned amplitude is phase-independent (the quadrature magnitude),
    which is what makes the comparison against the baseline meaningful: a signal
    that merely has energy near 440 Hz does not fit a single sinusoid.
    """
    cos_dot = sin_dot = cos_norm = sin_norm = 0.0
    energy = 0.0
    basis = []
    for index, value in enumerate(window):
        theta = 2.0 * math.pi * TONE_HZ * (start + index) / RATE
        cosine, sine = math.cos(theta), math.sin(theta)
        basis.append((cosine, sine))
        cos_dot += value * cosine
        sin_dot += value * sine
        cos_norm += cosine * cosine
        sin_norm += sine * sine
        energy += value * value
    a = cos_dot / cos_norm if cos_norm else 0.0
    b = sin_dot / sin_norm if sin_norm else 0.0
    residual = 0.0
    for (cosine, sine), value in zip(basis, window):
        difference = value - (a * cosine + b * sine)
        residual += difference * difference
    count = len(window)
    return (math.hypot(a, b),
            math.sqrt(residual / count),
            math.sqrt(energy / count))


def difference_stats(left: array.array, right: array.array,
                     start: int, count: int) -> tuple[float, float]:
    first = start * CHANNELS
    last = (start + count) * CHANNELS
    total = 0.0
    differing = 0
    for index in range(first, last):
        delta = left[index] - right[index]
        total += float(delta) * float(delta)
        if delta:
            differing += 1
    span = last - first
    return math.sqrt(total / span), differing / span


# ==========================================================================
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR,
                        help="native build directory or mdkr64 executable")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--frames", type=int, default=FRAMES)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).expanduser().resolve()
    require(binary.is_file(), f"build executable not found: {binary}")
    require(rom.is_file(), f"ROM not found: {rom}")
    require(args.frames >= FRAMES,
            f"--frames must be at least {FRAMES} to reach sequence "
            f"{SEQUENCE_ID}")

    arms = {
        "baseline": Arm("baseline", "none", 100, True),
        "pack": Arm("pack", "dir", 100, True),
        "zipped": Arm("zipped", "zip", 100, True),
        "pack-muted": Arm("pack-muted", "dir", 0, False),
        "baseline-muted": Arm("baseline-muted", "none", 0, False),
        "pack-half": Arm("pack-half", "dir", 50, False),
        "pack-off": Arm("pack-off", "dir", 100, True, packs="0"),
    }

    print("1. seven headless captures, no device, no pack content in the repo")
    with tempfile.TemporaryDirectory(prefix="mdkr_mod_music_") as tmp:
        root = Path(tmp)
        for arm in arms.values():
            run_arm(binary, rom, root, arm, args.frames, args.timeout,
                    args.verbose)

    frames_captured = len(arms["baseline"].samples) // CHANNELS
    require(frames_captured >= FIT_START_FRAME + FIT_FRAMES,
            f"capture is {frames_captured} frames, too short for the "
            f"{FIT_START_FRAME}+{FIT_FRAMES} measurement window")
    for arm in arms.values():
        require(len(arm.samples) // CHANNELS == frames_captured,
                f"{arm.label}: capture length differs from the baseline's")
    reached = [line for line in arms["baseline"].traces
               if f"seq={SEQUENCE_ID} " in line and "playing=1" in line]
    require(reached,
            f"the route never reached sequence {SEQUENCE_ID}; there is no "
            "music to replace and every assertion below would be vacuous")
    for label in ("pack", "zipped", "pack-half"):
        needle = f"[MODS] music/{SEQUENCE_ID}.wav replaces sequence {SEQUENCE_ID}"
        require(any(needle in line for line in arms[label].mods),
                f"{label}: the engine never reported the replacement\n" +
                "\n".join(arms[label].mods))
    require(not arms["baseline"].mods,
            "the baseline arm found content packs; its mods/ is not empty")
    require(any("1 pack(s) active" in line for line in arms["pack-off"].mods),
            "the pack-off arm did not even find the pack, so assertion 7 "
            "would be measuring an empty mods/ directory\n"
            + "\n".join(arms["pack-off"].mods))
    print(f"   ok  {frames_captured} frames each; sequence {SEQUENCE_ID} "
          f"reached; replacement reported in 3 arms")

    print("2. the pack's own synthesised WAV is what the capture holds")
    fits = {}
    for label in ("baseline", "pack", "pack-half", "pack-muted",
                  "baseline-muted"):
        window = mono_window(arms[label].samples, FIT_START_FRAME, FIT_FRAMES)
        fits[label] = fit_tone(window, FIT_START_FRAME)
    pack_amp, pack_residual, pack_rms = fits["pack"]
    base_amp, base_residual, base_rms = fits["baseline"]
    require(pack_amp >= PACK_AMPLITUDE_FLOOR,
            f"the pack arm's {TONE_HZ:.0f} Hz amplitude is {pack_amp:.1f}, "
            f"below the {PACK_AMPLITUDE_FLOOR:.0f} floor — the synthesised "
            "track is not being played")
    require(pack_residual <= PACK_RESIDUAL_CEIL,
            f"the pack arm is not the synthesised tone: residual RMS "
            f"{pack_residual:.1f} above the {PACK_RESIDUAL_CEIL:.1f} ceiling "
            f"(amplitude {pack_amp:.1f})")
    require(base_rms >= BASELINE_RMS_FLOOR,
            f"the baseline window RMS is {base_rms:.1f}: there is no ROM music "
            "in it, so 'not the baseline' below would be vacuous")
    require(base_amp <= BASELINE_AMPLITUDE_CEIL,
            f"the baseline already contains a {TONE_HZ:.0f} Hz tone at "
            f"amplitude {base_amp:.1f}; this measurement cannot tell the "
            "replacement apart from the sequence")
    print(f"   ok  pack amp={pack_amp:.1f} residual={pack_residual:.1f} "
          f"({pack_residual / pack_amp:.5%} of signal); "
          f"baseline amp={base_amp:.1f} in rms={base_rms:.1f}")

    print("3. and it is not the baseline")
    require(arms["pack"].digest != arms["baseline"].digest,
            "the pack arm's capture is byte-identical to the baseline's")
    diff_rms, diff_fraction = difference_stats(
        arms["pack"].samples, arms["baseline"].samples,
        FIT_START_FRAME, FIT_FRAMES)
    require(diff_rms >= DIFF_RMS_FLOOR,
            f"pack-vs-baseline difference RMS {diff_rms:.1f} is below the "
            f"{DIFF_RMS_FLOOR:.0f} floor")
    require(diff_fraction >= DIFF_FRACTION_FLOOR,
            f"only {diff_fraction:.2%} of the music window differs from the "
            f"baseline (need {DIFF_FRACTION_FLOOR:.0%})")
    print(f"   ok  diff rms={diff_rms:.1f} over {diff_fraction:.2%} of the "
          "window")

    print("4. the Music slider governs the replacement, at 50 % and at 0")
    half_amp = fits["pack-half"][0]
    ratio = half_amp / pack_amp
    require(abs(ratio - 0.5) <= HALF_VOLUME_TOL,
            f"Music 50 gives {ratio:.4f}x the amplitude of Music 100, not "
            f"0.5 +- {HALF_VOLUME_TOL}")
    muted_amp = fits["pack-muted"][0]
    require(muted_amp <= SILENT_AMPLITUDE_CEIL,
            f"at Music 0 the replacement still plays at amplitude "
            f"{muted_amp:.2f}")
    require(arms["pack-muted"].digest == arms["baseline-muted"].digest,
            "at Music 0 a pack still changes the output: the capture is not "
            "byte-identical to the same run with no pack installed")
    print(f"   ok  50 % -> {ratio:.4f}x; 0 % -> amp {muted_amp:.2f} and a "
          "capture identical to no pack at all")

    print("5. presentation-only: authoritative state and the sequence trace "
          "do not move")
    baseline_rows = arms["baseline"].rows
    require(len(baseline_rows) == args.frames,
            f"expected {args.frames} [SIMHASH] rows, got "
            f"{len(baseline_rows)} — the v3 stream is not being emitted")
    for label in ("pack", "zipped"):
        rows = arms[label].rows
        require(rows == baseline_rows,
                f"{label}: the [SIMHASH] v3 stream differs from the baseline's "
                "— the replacement is NOT presentation-only")
        require(arms[label].traces == arms["baseline"].traces,
                f"{label}: the sequence-player trace differs from the "
                "baseline's; the muted player is not advancing identically\n"
                f"  baseline: {arms['baseline'].traces}\n"
                f"  {label}: {arms[label].traces}")
    print(f"   ok  {len(baseline_rows)} [SIMHASH] rows and "
          f"{len(arms['baseline'].traces)} sequence-trace lines identical "
          "across all three arms")

    print("6. the same pack, zipped, is discovered and produces the same bytes")
    require(arms["zipped"].digest == arms["pack"].digest,
            "the zipped pack's capture is not byte-identical to the "
            f"directory pack's ({arms['zipped'].digest[:16]} vs "
            f"{arms['pack'].digest[:16]})")
    require(any("1 pack(s) active" in line for line in arms["zipped"].mods),
            "the zipped pack was not discovered as an active pack\n" +
            "\n".join(arms["zipped"].mods))
    print(f"   ok  zip == directory, sha256 {arms['pack'].digest[:16]}")

    print("7. Custom content off means the game's own music, not the pack's")
    needle = f"[MODS] music/{SEQUENCE_ID}.wav replaces sequence {SEQUENCE_ID}"
    require(not any(needle in line for line in arms["pack-off"].mods),
            "with Content.PacksEnabled=0 the engine still claimed the "
            "replacement\n" + "\n".join(arms["pack-off"].mods))
    require(arms["pack-off"].digest == arms["baseline"].digest,
            "with Content.PacksEnabled=0 an installed pack still changes the "
            "output: the capture is not byte-identical to the same run with no "
            f"pack at all ({arms['pack-off'].digest[:16]} vs "
            f"{arms['baseline'].digest[:16]})")
    require(arms["pack-off"].rows == baseline_rows,
            "pack-off: the [SIMHASH] v3 stream differs from the baseline's")
    print(f"   ok  pack installed, switch off -> the baseline's own bytes, "
          f"sha256 {arms['baseline'].digest[:16]}")

    print("check_mod_music_override: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (CheckError, OSError, subprocess.TimeoutExpired, wave.Error) as error:
        print(f"check_mod_music_override: FAIL — {error}", file=sys.stderr)
        raise SystemExit(1)
