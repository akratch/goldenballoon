#!/usr/bin/env python3
"""Compare two PCM audio dumps (s16 stereo 22050 Hz).

Usage: compare_audio.py baseline test

Accepts either raw s16le dumps or RIFF/WAVE files (MDKR_AUDIO_DUMP writes WAV;
the .raw form is kept for dumps taken straight off the mixer). Reports RMS
difference, max amplitude, and zero-crossing rate.

Exit code 0 = match, 1 = regression detected.

Ported from mgb64 tools/compare_audio.py; the WAV path and the byte-order fixup
are the mdkr64 adaptation.
"""
import array
import math
import struct
import sys
import wave


def load_pcm(path):
    """Interleaved s16 samples from either a WAV container or a raw s16le dump."""
    with open(path, "rb") as f:
        head = f.read(12)
    if head[:4] == b"RIFF" and head[8:12] == b"WAVE":
        with wave.open(path, "rb") as wf:
            if wf.getsampwidth() != 2:
                sys.exit("%s: expected 16-bit PCM, got %d-bit"
                         % (path, wf.getsampwidth() * 8))
            raw = wf.readframes(wf.getnframes())
        a = array.array("h")
        a.frombytes(raw)
        if sys.byteorder == "big":
            a.byteswap()
        return a
    with open(path, "rb") as f:
        data = f.read()
    return struct.unpack("<%dh" % (len(data) // 2), data)


def rms(samples):
    if not samples:
        return 0.0
    return math.sqrt(sum(s * s for s in samples) / len(samples))


def zero_crossing_rate(samples):
    if len(samples) < 2:
        return 0.0
    crossings = sum(1 for i in range(1, len(samples))
                    if (samples[i] >= 0) != (samples[i - 1] >= 0))
    return crossings / len(samples)


def main():
    if len(sys.argv) != 3:
        print("Usage: %s baseline test" % sys.argv[0])
        sys.exit(2)

    base = load_pcm(sys.argv[1])
    test = load_pcm(sys.argv[2])

    if len(base) == 0 and len(test) == 0:
        print("Samples: both empty")
        print("PASS")
        sys.exit(0)
    if len(base) == 0 or len(test) == 0:
        print("FAIL: one file empty (%d vs %d samples)" % (len(base), len(test)))
        sys.exit(1)

    min_len = min(len(base), len(test))
    if abs(len(base) - len(test)) > max(len(base), 1) * 0.1:
        print("WARNING: size mismatch: %d vs %d samples" % (len(base), len(test)))

    rms_base = rms(base[:min_len])
    rms_test = rms(test[:min_len])

    diff = [test[i] - base[i] for i in range(min_len)]
    rms_diff = rms(diff)

    zcr_base = zero_crossing_rate(base[:min_len])
    zcr_test = zero_crossing_rate(test[:min_len])

    max_base = max(abs(s) for s in base[:min_len])
    max_test = max(abs(s) for s in test[:min_len])

    print("Samples:    %d vs %d (compared %d)" % (len(base), len(test), min_len))
    print("RMS:        baseline=%.1f  test=%.1f  diff=%.1f"
          % (rms_base, rms_test, rms_diff))
    print("Max amp:    baseline=%d  test=%d" % (max_base, max_test))
    print("ZCR:        baseline=%.4f  test=%.4f" % (zcr_base, zcr_test))

    fail = False
    if rms_diff > 500:
        print("FAIL: RMS difference %.1f > 500 (significant audio change)" % rms_diff)
        fail = True
    if zcr_test > 0.8 and zcr_base < 0.5:
        print("FAIL: ZCR jumped from %.4f to %.4f (likely static/noise)"
              % (zcr_base, zcr_test))
        fail = True
    if rms_test < 10 and rms_base > 100:
        print("FAIL: Audio went silent (RMS %.1f -> %.1f)" % (rms_base, rms_test))
        fail = True

    if fail:
        sys.exit(1)
    print("PASS")
    sys.exit(0)


if __name__ == "__main__":
    main()
