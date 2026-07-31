#!/usr/bin/env python3
"""Align ares (real-ROM reference) vs native-port frames and score similarity.

The two runners dump at different resolutions (ares ~640x240 N64 VI output;
native 1280x960 HiDPI) and along different boot clocks. This tool takes the
route's mark-pairs (name native_frame ares_frame), loads the nearest available
dumped frame from each runner, downscales both to a common 4:3 luma raster, and
scores similarity with two deliberately coarse metrics so that minor filtering /
sub-pixel differences do not dominate:

  * hist  -- 32-bin global luma histogram intersection
  * block -- normalized correlation of a 16x12 grid of block-mean luma
             (structural: "is the same stuff in the same place")

It writes a per-route report (JSON + text) and a montage PNG (one row per mark:
native | ares | abs-diff) for eyeballing.

Frame files: frame_<N>.ppm (P6). Native uses %04d, ares %05d -- the frame index
is parsed as an integer, not by fixed width.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

CMP_W, CMP_H = 256, 192          # common 4:3 comparison raster
GRID_X, GRID_Y = 16, 12          # block-mean grid
HIST_BINS = 32
THUMB_W, THUMB_H = 240, 180      # montage thumbnail size
LABEL_H = 18

FRAME_RE = re.compile(r"frame_(\d+)\.ppm$")


def index_frames(dir_path: str) -> dict[int, str]:
    out: dict[int, str] = {}
    for f in glob.glob(os.path.join(dir_path, "frame_*.ppm")):
        m = FRAME_RE.search(os.path.basename(f))
        if m:
            out[int(m.group(1))] = f
    return out


def nearest(frames: dict[int, str], target: int) -> tuple[int, str] | None:
    if not frames:
        return None
    best = min(frames, key=lambda k: abs(k - target))
    return best, frames[best]


def load_luma(path: str) -> np.ndarray:
    im = Image.open(path).convert("L").resize((CMP_W, CMP_H), Image.BILINEAR)
    return np.asarray(im, dtype=np.float64)


def load_rgb_thumb(path: str) -> Image.Image:
    return Image.open(path).convert("RGB").resize((THUMB_W, THUMB_H), Image.BILINEAR)


def hist_similarity(a: np.ndarray, b: np.ndarray) -> float:
    ha, _ = np.histogram(a, bins=HIST_BINS, range=(0, 255))
    hb, _ = np.histogram(b, bins=HIST_BINS, range=(0, 255))
    ha = ha / max(ha.sum(), 1)
    hb = hb / max(hb.sum(), 1)
    return float(np.minimum(ha, hb).sum())  # histogram intersection in [0,1]


def block_similarity(a: np.ndarray, b: np.ndarray) -> float:
    def block_means(x: np.ndarray) -> np.ndarray:
        ys = np.array_split(x, GRID_Y, axis=0)
        cells = []
        for row in ys:
            for col in np.array_split(row, GRID_X, axis=1):
                cells.append(col.mean())
        return np.array(cells, dtype=np.float64)

    va, vb = block_means(a), block_means(b)
    va -= va.mean()
    vb -= vb.mean()
    na, nb = np.linalg.norm(va), np.linalg.norm(vb)
    if na < 1e-6 or nb < 1e-6:
        # One image is flat (e.g. both black): fall back to "equal if both flat".
        return 1.0 if (na < 1e-6 and nb < 1e-6) else 0.0
    corr = float(np.dot(va, vb) / (na * nb))       # [-1,1]
    return max(0.0, (corr + 1.0) / 2.0)            # map to [0,1]


def make_diff_thumb(a: np.ndarray, b: np.ndarray) -> Image.Image:
    diff = np.abs(a - b).astype(np.uint8)
    im = Image.fromarray(diff, mode="L").resize((THUMB_W, THUMB_H), Image.NEAREST)
    return im.convert("RGB")


def parse_mark_pairs(path: str) -> list[tuple[str, int, int]]:
    pairs = []
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 3:
                continue
            pairs.append((parts[0], int(parts[1]), int(parts[2])))
    return pairs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--route", required=True)
    ap.add_argument("--native-dir", required=True)
    ap.add_argument("--ares-dir", required=True)
    ap.add_argument("--mark-pairs", required=True, help="file: 'name native_frame ares_frame' lines")
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--max-frame-drift", type=int, default=40,
                    help="warn if the nearest dumped frame is farther than this from the target")
    ap.add_argument("--align-window", type=int, default=120,
                    help="search +/- this many ares frames for the best-matching frame at each "
                         "mark (0 disables). Reports BOTH the exact-frame score and the aligned "
                         "score plus the offset used -- see the note in the module docstring.")
    args = ap.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    native_frames = index_frames(args.native_dir)
    ares_frames = index_frames(args.ares_dir)
    marks = parse_mark_pairs(args.mark_pairs)

    if not native_frames:
        print(f"FAIL: no native frames in {args.native_dir}", file=sys.stderr)
        return 1
    if not ares_frames:
        print(f"FAIL: no ares frames in {args.ares_dir}", file=sys.stderr)
        return 1

    results = []
    rows = []
    for name, nf, af in marks:
        n_hit = nearest(native_frames, nf)
        a_hit = nearest(ares_frames, af)
        if not n_hit or not a_hit:
            continue
        n_idx, n_path = n_hit
        a_idx, a_path = a_hit
        n_luma = load_luma(n_path)

        def score(path):
            l = load_luma(path)
            h = hist_similarity(n_luma, l)
            b = block_similarity(n_luma, l)
            return 0.5 * h + 0.5 * b, h, b, l

        combined, hist, block, a_luma = score(a_path)

        # Bounded re-alignment. The two runners' transition lengths differ
        # STRUCTURALLY, not because either is wrong: on real hardware a level
        # load is a genuine multi-frame stall (DMA + decompression) that ares
        # presents frames through, while this port loads from host memory almost
        # instantly. Measured on title_to_options: our black transition spans
        # ~20 frames, the real ROM's ~40. So a single global sync delta cannot
        # align a title screen AND a screen two loads later -- the derived ares
        # frame lands mid-fade and scores ~0 hist against a fully-drawn native
        # frame, which looks exactly like a fidelity failure but is not.
        #
        # So: also search a bounded window for the best-matching ares frame and
        # report BOTH scores plus the offset used. A large offset is itself the
        # finding (transition-length drift); a low ALIGNED score is the real
        # fidelity gap. Never read the aligned score alone -- it is the more
        # flattering of the two by construction.
        best = {"offset": 0, "frame": a_idx, "sim": combined, "hist": hist, "block": block}
        if args.align_window > 0:
            for cand_idx, cand_path in sorted(ares_frames.items()):
                if abs(cand_idx - a_idx) > args.align_window:
                    continue
                c_sim, c_h, c_b, _ = score(cand_path)
                if c_sim > best["sim"]:
                    best = {"offset": cand_idx - a_idx, "frame": cand_idx,
                            "sim": c_sim, "hist": c_h, "block": c_b}
            if best["frame"] != a_idx:
                a_luma = load_luma(ares_frames[best["frame"]])
                a_path = ares_frames[best["frame"]]

        # Degenerate-frame guard. A blank (near-constant) frame has no structure,
        # so block correlation is undefined and hist intersection is trivially
        # ~100% against any other blank frame -- two black frames score 50%,
        # which reads like a half-decent match and is actually "both nothing".
        # Flag it instead of pretending it is a measurement.
        n_flat = float(n_luma.std()) < 3.0
        a_flat = float(a_luma.std()) < 3.0
        rec = {
            "mark": name,
            "degenerate": bool(n_flat or a_flat),
            "native_blank": n_flat, "ares_blank": a_flat,
            "native_target": nf, "native_used": n_idx, "native_drift": n_idx - nf,
            "ares_target": af, "ares_used": a_idx, "ares_drift": a_idx - af,
            "hist_sim": round(hist, 4),
            "block_sim": round(block, 4),
            "similarity": round(combined, 4),
            "aligned_frame": best["frame"],
            "aligned_offset": best["offset"],
            "aligned_hist_sim": round(best["hist"], 4),
            "aligned_block_sim": round(best["block"], 4),
            "aligned_similarity": round(best["sim"], 4),
        }
        results.append(rec)
        rows.append((name, rec, load_rgb_thumb(n_path), load_rgb_thumb(a_path),
                     make_diff_thumb(n_luma, a_luma)))

    # ---- montage ----------------------------------------------------------
    montage_path = out_dir / f"montage_{args.route}.png"
    if rows:
        col_labels = ["native (our port)", "ares (real ROM)", "abs luma diff"]
        row_h = THUMB_H + LABEL_H
        W = THUMB_W * 3 + 8
        H = row_h * len(rows) + LABEL_H
        sheet = Image.new("RGB", (W, H), (18, 18, 18))
        d = ImageDraw.Draw(sheet)
        for c, lbl in enumerate(col_labels):
            d.text((c * (THUMB_W + 4) + 4, 4), lbl, fill=(180, 200, 255))
        for r, (name, rec, n_thumb, a_thumb, diff) in enumerate(rows):
            y = LABEL_H + r * row_h
            for c, thumb in enumerate((n_thumb, a_thumb, diff)):
                sheet.paste(thumb, (c * (THUMB_W + 4), y + LABEL_H))
            label = (f"{name}  aligned={rec['aligned_similarity']*100:.1f}% "
                     f"(hist={rec['aligned_hist_sim']*100:.0f}% block={rec['aligned_block_sim']*100:.0f}%)  "
                     f"exact={rec['similarity']*100:.1f}%  "
                     f"nat#{rec['native_used']} ares#{rec['aligned_frame']} "
                     f"(off{rec['aligned_offset']:+d})")
            d.text((4, y + 3), label, fill=(255, 255, 120))
        sheet.save(montage_path)

    # Overall scores exclude degenerate marks -- averaging in a "both frames are
    # blank" 50% would move the headline number without measuring anything.
    scored = [r for r in results if not r["degenerate"]]
    overall = round(float(np.mean([r["similarity"] for r in scored])), 4) if scored else 0.0
    overall_aligned = (round(float(np.mean([r["aligned_similarity"] for r in scored])), 4)
                       if scored else 0.0)
    report = {
        "route": args.route,
        "native_dir": args.native_dir,
        "ares_dir": args.ares_dir,
        "native_frames_available": len(native_frames),
        "ares_frames_available": len(ares_frames),
        "marks_compared": len(results),
        "align_window": args.align_window,
        "overall_similarity": overall,
        "overall_aligned_similarity": overall_aligned,
        "montage": str(montage_path) if rows else None,
        "marks": results,
    }
    report_path = out_dir / f"report_{args.route}.json"
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")

    # ---- text summary -----------------------------------------------------
    print(f"=== oracle comparison: {args.route} ===")
    print(f"native frames: {len(native_frames)}   ares frames: {len(ares_frames)}")
    for r in results:
        drift = ""
        if abs(r["native_drift"]) > args.max_frame_drift or abs(r["ares_drift"]) > args.max_frame_drift:
            drift = f"  [drift nat{r['native_drift']:+d} ares{r['ares_drift']:+d}]"
        if r["degenerate"]:
            who = "native+ares" if (r["native_blank"] and r["ares_blank"]) else (
                  "native" if r["native_blank"] else "ares")
            print(f"  {r['mark']:<14} SKIPPED (blank frame: {who}) -- no structure to score; "
                  f"move this mark off the transition")
            continue
        print(f"  {r['mark']:<14} aligned={r['aligned_similarity']*100:5.1f}% "
              f"(hist={r['aligned_hist_sim']*100:5.1f}% block={r['aligned_block_sim']*100:5.1f}%)  "
              f"exact={r['similarity']*100:5.1f}%  "
              f"nat#{r['native_used']} ares#{r['aligned_frame']} "
              f"(off{r['aligned_offset']:+d}){drift}")
    print(f"overall: aligned={overall_aligned*100:.1f}%  exact={overall*100:.1f}%")
    print("  aligned = best ares frame within +/-%d (offset shown); exact = the derived frame.\n"
          "  A large offset means transition-length drift, not a render gap; judge fidelity on\n"
          "  the ALIGNED score, and treat a big offset as its own finding." % args.align_window)
    print(f"montage: {montage_path if rows else '(none)'}")
    print(f"report:  {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
