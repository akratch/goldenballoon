#!/usr/bin/env python3
"""List the stable "screen segments" in a captured frame directory.

Calibrating an oracle route means answering one question repeatedly: *at which
frame does each runner actually arrive on each screen?* Doing that by eye across
dozens of PPMs is slow and error-prone, and getting it wrong is expensive -- a
mis-timed press sends the two runners down different menu paths and the
comparison then reports a large "fidelity gap" that is really a navigation
divergence (our port on CAUTION while the real ROM was still on PLAYER SELECT).

This turns it into a measurement. A menu transition in DKR fades to black, so a
capture is a sequence of stable segments separated by dark gaps. For each frame
it reports mean luma and the structural distance from the previous frame, then
groups frames into segments and prints where each one starts.

Use the printed segment starts to set `ares_extra` on a route's events/marks
(see tools/dkr_oracle_route.py emit_ares_script for why that field exists).

Usage:
    tools/oracle_screens.py build/ares-oracle/<route>/ares/frames
    tools/oracle_screens.py <dir> --dark 12 --change 18 --verbose
"""
import argparse
import os
import struct
import sys


def read_ppm(path):
    with open(path, "rb") as f:
        magic = f.readline().strip()
        if magic != b"P6":
            raise ValueError("not a P6 PPM: %s" % path)
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        w, h = map(int, line.split())
        f.readline()  # maxval
        return w, h, f.read(w * h * 3)


def thumb_luma(w, h, buf, gw=16, gh=12):
    """Coarse grid of mean luma -- structural fingerprint, cheap and resolution
    independent so native (1280x960) and ares (640x480) are comparable."""
    cells = []
    for gy in range(gh):
        for gx in range(gw):
            x0, x1 = gx * w // gw, max(gx * w // gw + 1, (gx + 1) * w // gw)
            y0, y1 = gy * h // gh, max(gy * h // gh + 1, (gy + 1) * h // gh)
            tot = n = 0
            for y in range(y0, y1, max(1, (y1 - y0) // 4)):
                row = y * w * 3
                for x in range(x0, x1, max(1, (x1 - x0) // 4)):
                    i = row + x * 3
                    tot += (buf[i] * 30 + buf[i + 1] * 59 + buf[i + 2] * 11) // 100
                    n += 1
            cells.append(tot / max(1, n))
    return cells


def frame_index(name):
    digits = "".join(c for c in name if c.isdigit())
    return int(digits) if digits else -1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir")
    ap.add_argument("--dark", type=float, default=12.0,
                    help="mean luma at or below this counts as a transition/blank frame")
    ap.add_argument("--change", type=float, default=18.0,
                    help="mean per-cell luma change that starts a new segment")
    ap.add_argument("-v", "--verbose", action="store_true", help="per-frame detail")
    args = ap.parse_args()

    names = sorted((n for n in os.listdir(args.dir) if n.endswith(".ppm")),
                   key=frame_index)
    if not names:
        sys.exit("no .ppm frames in %s" % args.dir)

    rows = []
    prev = None
    for n in names:
        w, h, buf = read_ppm(os.path.join(args.dir, n))
        cells = thumb_luma(w, h, buf)
        mean = sum(cells) / len(cells)
        dist = 0.0 if prev is None else sum(abs(a - b) for a, b in zip(cells, prev)) / len(cells)
        rows.append((frame_index(n), mean, dist, cells))
        prev = cells

    if args.verbose:
        print("frame    mean   change")
        for idx, mean, dist, _ in rows:
            tag = " DARK" if mean <= args.dark else ""
            print("  %6d %6.1f %7.1f%s" % (idx, mean, dist, tag))
        print()

    # Group into segments: a run of non-dark frames with no large structural jump.
    segments = []
    cur = None
    for idx, mean, dist, _ in rows:
        dark = mean <= args.dark
        if dark:
            if cur:
                segments.append(cur); cur = None
            continue
        if cur and dist > args.change:
            segments.append(cur); cur = None
        if cur is None:
            cur = {"start": idx, "end": idx, "n": 1, "mean": mean}
        else:
            cur["end"] = idx; cur["n"] += 1
            cur["mean"] = (cur["mean"] * (cur["n"] - 1) + mean) / cur["n"]
    if cur:
        segments.append(cur)

    print("%s: %d frames, %d stable segment(s)" % (args.dir, len(rows), len(segments)))
    print("  (a segment = a screen; its START is when that screen became visible)")
    for i, s in enumerate(segments):
        print("  [%d] frames %6d..%-6d  n=%-3d  mean_luma=%5.1f   <-- screen appears at %d"
              % (i, s["start"], s["end"], s["n"], s["mean"], s["start"]))
    if not segments:
        print("  none -- every frame was at or below the --dark threshold")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
