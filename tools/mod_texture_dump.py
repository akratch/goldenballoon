#!/usr/bin/env python3
"""Dump the digest-named PNG corpus a content pack author starts from.

A thin wrapper, on purpose: `platform/mod_texture_store.c` does the decoding
and naming (MDKR_MOD_TEXTURE_DUMP=<dir>), and `platform/mod_texture_key.h`
owns the digest itself. This just runs the real binary headlessly over a
caller-supplied input script with that variable set, then reports how many
files landed.

No pack has to be installed to use this -- that is the whole point. An author
dumping a first corpus has none yet, and the store treats a running dump as
reason enough to resolve every texture's digest even with zero packs found.

Each run writes `<out>/<digest>.png` (the picture, RGBA8, exactly what the
game uploaded for that texture) and `<out>/<digest>.txt` (its width, height,
RDP `fmt`/`siz`, and where in the run it was first requested) for every
distinct texture the script causes to load. Copy a PNG this writes into
`<pack>/textures/<digest>.png`, unmodified filename, and it overrides that
exact texture -- see docs/MODDING.md.

Usage:
    tools/mod_texture_dump.py --input-script tests/input_scripts/race_drive_long.txt \
        --frames 3900 --out /some/scratch/dir

Always runs muted + headless (MDKR_AUDIO=0 and --headless-frames), per
tests/README.md. Exit 0 on a clean run that dumped at least one texture; exit 1
if the binary failed or nothing was dumped (the script likely never bound a
texture, which usually means it exited before reaching gameplay).
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BUILD_DIR = "build"
DEFAULT_FRAMES = 1800


def resolve_binary(value: str) -> Path:
    """`--build build-dir` -> `build-dir/mdkr64`; an executable path passes through."""
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = ROOT / path
    if path.is_dir():
        path /= "mdkr64"
    return path.resolve()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR,
                        help="build directory or path to the mdkr64 binary "
                             f"(default: {DEFAULT_BUILD_DIR})")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--input-script", required=True,
                        help="a `frame TOKEN[+TOKEN]` script; see tests/input_scripts/ "
                             "for examples and tests/README.md for the format")
    parser.add_argument("--frames", type=int, default=DEFAULT_FRAMES,
                        help=f"headless frame budget (default: {DEFAULT_FRAMES}); "
                             "must cover however long the script takes to reach the "
                             "textures you want dumped")
    parser.add_argument("--out", required=True,
                        help="directory to write <digest>.png/<digest>.txt into; "
                             "created if missing. Keep it outside the repository -- "
                             "these are decoded ROM textures, never commit them")
    parser.add_argument("--renderer", choices=("gl", "webgpu"), default="gl")
    parser.add_argument("--mode", choices=("pure", "restored", "remastered"),
                        default=None,
                        help="forwarded as --MODE; default is the binary's own default")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="echo the command line and the binary's own output")
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    binary = resolve_binary(args.build)
    rom = Path(args.rom).expanduser().resolve()
    script = Path(args.input_script).expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM"), (script, "input script")):
        if not path.exists():
            print(f"FAIL: {label} not found: {path}", file=sys.stderr)
            return 1

    out_dir = Path(args.out).expanduser().resolve()
    # Every file this writes is decoded ROM pixels. Inside the working tree they
    # are untracked, un-ignored and innocuously named, so the next `git add -A`
    # could commit ROM-derived data. Refuse rather than warn: there is no reason
    # to dump into the source
    # tree, and the cost of being wrong is a published repository that has to be
    # rewritten. `mod-texture-dump/` is the exception because .gitignore covers
    # it by name.
    allowed_inside = ROOT / "mod-texture-dump"
    if out_dir == ROOT or (
        ROOT in out_dir.parents
        and allowed_inside != out_dir
        and allowed_inside not in out_dir.parents
    ):
        print(
            f"FAIL: --out {out_dir} is inside the source tree ({ROOT}).\n"
            "      This writes ROM-derived pixels; committing them would break "
            "the clean-room guarantee.\n"
            "      Use a path outside the repository, or "
            f"{allowed_inside} (which .gitignore covers).",
            file=sys.stderr,
        )
        return 1
    out_dir.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="mdkr_mod_texture_dump_") as run_dir_s:
        run_dir = Path(run_dir_s)
        save_dir = run_dir / "save"
        save_dir.mkdir(parents=True, exist_ok=True)

        # A private config/save root per run, same discipline the regression
        # checks use (tests/check_texture_lineswap.py): nothing this tool does
        # should touch the caller's real preferences or save data.
        env = {
            key: value for key, value in os.environ.items()
            if not key.startswith(("MDKR", "GE007_"))
        }
        env.update(
            MDKR_AUDIO="0",              # --headless-frames is the guarantee; belt+braces
            MDKR_MOD_TEXTURE_DUMP=str(out_dir),
            MDKR_RENDERER=args.renderer,
            MDKR_SAVE_DIR=str(save_dir),
            MDKR_VIDEO_CONFIG_PATH=str(run_dir / "mdkr64.ini"),
            MDKR64_HIDDEN="1",
            LC_ALL="C",
        )

        cmd = [
            str(binary),
            "--headless-frames", str(args.frames),
            "--input-script", str(script),
            "--rom", str(rom),
        ]
        if args.mode is not None:
            cmd.append(f"--{args.mode}")

        if args.verbose:
            print(f"$ MDKR_MOD_TEXTURE_DUMP={out_dir} " + " ".join(cmd))

        proc = subprocess.run(
            cmd, cwd=run_dir, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        if args.verbose:
            print(proc.stdout)
        if proc.returncode != 0:
            print(f"FAIL: binary exited {proc.returncode}", file=sys.stderr)
            print(proc.stdout[-4000:], file=sys.stderr)
            return 1

    written = sorted(p.stem for p in out_dir.glob("*.png"))
    if not written:
        print(
            "FAIL: no textures were dumped -- the script may have exited "
            "before drawing anything; rerun with -v to see the binary's own "
            "output",
            file=sys.stderr,
        )
        return 1

    print(f"mod_texture_dump: wrote {len(written)} texture(s) to {out_dir}")
    if args.verbose:
        for digest in written:
            print(f"  {digest}.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
