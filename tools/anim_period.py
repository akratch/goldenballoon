#!/usr/bin/env python3
"""Measure the dominant animation period, in frames, of a per-frame PPM capture.

Why this exists
---------------
"The dancing on the player-select menu is CRAZY fast" is a real bug report, but
not yet a number. Frame-rate readouts do not settle it either: the engine can
present at a perfectly correct 60 Hz while advancing menu animation two ticks per
present, and it can also present too fast. Those two look identical to a human
and have completely different causes.

What IS decisive is the period of the motion, in frames, measured on our port and
on the real ROM under the same navigation. If ares bobs with a period of N frames
and we bob with N/2, the engine advances animation twice per frame; if both are N,
the animation is right and the problem is presentation rate.

How
---
Each frame is reduced to a coarse luma grid, so captures at different resolutions
(native 1280x960, ares 1536x1152) are comparable. Every grid cell is then
mean-centred across time, which deletes the static parts of the screen -- the
background, the panels, the text -- and leaves only what moves. The dominant
period is the lag of the first clear peak in the normalised autocorrelation of
that residual.

Reported alongside it:
  * motion    -- RMS of the residual; near zero means nothing moved and the period
                 is meaningless (guard against "measuring" a still screen)
  * peak      -- correlation at the reported lag, 1.0 = perfectly periodic
  * harmonics -- the next peaks, so a period of N is not confused with N/2 or 2N

Usage:
    tools/anim_period.py <frames-dir> [<frames-dir> ...]
    tools/anim_period.py a/frames b/frames --min-lag 4 --max-lag 60 --verbose
"""
import argparse
import os
import re
import sys


def read_ppm_luma_grid(path, gw, gh, region=None):
    """Coarse grid of mean luma. Resolution-independent, so two runners at
    different output sizes produce comparable feature vectors.

    `region` is (x0, y0, x1, y1) in NORMALISED coordinates, so the same region
    selects the same part of the picture in a 640x240 ares dump and a 1280x960
    native dump. Use it to exclude scenery that animates on its own (sky, water)
    and measure one specific thing."""
    with open(path, "rb") as f:
        if f.readline().strip() != b"P6":
            raise ValueError("not a P6 PPM: %s" % path)
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        w, h = map(int, line.split())
        f.readline()                       # maxval
        buf = f.read(w * h * 3)
    if len(buf) < w * h * 3:
        raise ValueError("truncated PPM: %s" % path)

    rx0, ry0, rx1, ry1 = region or (0.0, 0.0, 1.0, 1.0)
    x_lo, x_hi = int(rx0 * w), max(int(rx0 * w) + 1, int(rx1 * w))
    y_lo, y_hi = int(ry0 * h), max(int(ry0 * h) + 1, int(ry1 * h))
    rw, rh = x_hi - x_lo, y_hi - y_lo

    # Sample a stride rather than averaging every pixel: at 1280x960 a full
    # average is ~1.2M pixels per frame in pure Python, and the grid means are
    # indistinguishable at stride 4.
    step = 4
    cells = [0.0] * (gw * gh)
    counts = [0] * (gw * gh)
    for y in range(y_lo, y_hi, step):
        gy = (y - y_lo) * gh // rh
        row = y * w * 3
        for x in range(x_lo, x_hi, step):
            gx = (x - x_lo) * gw // rw
            i = row + x * 3
            # Rec.601 luma, integer weights.
            cells[gy * gw + gx] += (buf[i] * 299 + buf[i + 1] * 587 + buf[i + 2] * 114)
            counts[gy * gw + gx] += 1
    return [c / (n * 1000.0) if n else 0.0 for c, n in zip(cells, counts)]


def load_capture(d, gw, gh, start=None, end=None, region=None):
    def num(n):
        return int(re.search(r"(\d+)", n).group(1))
    names = sorted((n for n in os.listdir(d) if n.endswith(".ppm")), key=num)
    if start is not None:
        names = [n for n in names if num(n) >= start]
    if end is not None:
        names = [n for n in names if num(n) <= end]
    if not names:
        raise SystemExit("no PPM frames in %s (after --start/--end filtering)" % d)
    frames = [read_ppm_luma_grid(os.path.join(d, n), gw, gh, region) for n in names]
    first = int(re.search(r"(\d+)", names[0]).group(1))
    last = int(re.search(r"(\d+)", names[-1]).group(1))
    return frames, first, last


def autocorrelation(frames, min_lag, max_lag):
    """Normalised autocorrelation of the time-varying residual."""
    n = len(frames)
    ncell = len(frames[0])
    means = [sum(f[c] for f in frames) / n for c in range(ncell)]
    resid = [[f[c] - means[c] for c in range(ncell)] for f in frames]

    energy = sum(v * v for row in resid for v in row)
    motion = (energy / (n * ncell)) ** 0.5

    out = []
    for lag in range(min_lag, min(max_lag, n - 4) + 1):
        num = den_a = den_b = 0.0
        for t in range(n - lag):
            a, b = resid[t], resid[t + lag]
            for c in range(ncell):
                num += a[c] * b[c]
                den_a += a[c] * a[c]
                den_b += b[c] * b[c]
        r = num / ((den_a * den_b) ** 0.5) if den_a > 0 and den_b > 0 else 0.0
        out.append((lag, r))
    return out, motion


def peaks(corr):
    """Local maxima, strongest first."""
    found = []
    for i in range(1, len(corr) - 1):
        if corr[i][1] > corr[i - 1][1] and corr[i][1] >= corr[i + 1][1]:
            found.append(corr[i])
    found.sort(key=lambda p: -p[1])
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dirs", nargs="+")
    ap.add_argument("--grid", default="24x18", help="luma grid, WxH (default 24x18)")
    ap.add_argument("--min-lag", type=int, default=3)
    ap.add_argument("--max-lag", type=int, default=64)
    ap.add_argument("--motion-floor", type=float, default=0.05,
                    help="below this residual RMS the screen is treated as still")
    ap.add_argument("--start", type=int, default=None,
                    help="ignore frames numbered below this (skip a fade-in)")
    ap.add_argument("--end", type=int, default=None)
    ap.add_argument("--region", default=None,
                    help="x0,y0,x1,y1 in 0..1 -- measure only this part of the picture")
    ap.add_argument("--verbose", action="store_true", help="print the whole correlogram")
    args = ap.parse_args()

    gw, gh = (int(v) for v in args.grid.lower().split("x"))
    region = tuple(float(v) for v in args.region.split(",")) if args.region else None
    if region and len(region) != 4:
        raise SystemExit("--region needs four comma-separated numbers")
    results = {}
    for d in args.dirs:
        frames, first, last = load_capture(d, gw, gh, args.start, args.end, region)
        corr, motion = autocorrelation(frames, args.min_lag, args.max_lag)
        pk = peaks(corr)
        label = os.path.basename(os.path.dirname(d.rstrip("/"))) or d
        print("%s" % d)
        print("   frames %d  (capture frame %d..%d)   motion(RMS)=%.3f"
              % (len(frames), first, last, motion))
        if motion < args.motion_floor:
            print("   STILL — residual below the motion floor; no period to report")
            results[label] = None
        elif not pk:
            print("   no periodic peak in lag %d..%d" % (args.min_lag, args.max_lag))
            results[label] = None
        else:
            print("   period = %d frames   (peak r=%.3f)" % (pk[0][0], pk[0][1]))
            print("   next peaks: %s"
                  % ", ".join("%d(r=%.2f)" % (l, r) for l, r in pk[1:5]))
            results[label] = pk[0][0]
        if args.verbose:
            for lag, r in corr:
                print("      lag %3d  r=%+.3f  %s" % (lag, r, "#" * int(max(0, r) * 40)))
        print()

    vals = [v for v in results.values() if v]
    if len(vals) == 2:
        a, b = vals
        print("ratio %.2fx  (%d vs %d frames)" % (max(a, b) / min(a, b), a, b))
    return 0


if __name__ == "__main__":
    sys.exit(main())
