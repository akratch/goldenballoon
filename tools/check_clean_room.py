#!/usr/bin/env python3
"""Fail-closed Git hygiene: byte-safe paths and bounded, checked blob reads."""
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile

ROM = re.compile(rb"\.(z64|n64|v64)$", re.I)
DUMP = re.compile(rb"(^|/)[0-9a-f]{32}\.(png|txt|texels)$")
EMULATOR = re.compile(rb"(^|/)(ares|mupen|parallel-rdp|angrylion|libretro)/", re.I)
MAGIC = {bytes.fromhex(value) for value in ("80371240", "37804012", "40123780")}
IGNORE = ("*.z64", "*.n64", "*.v64", "*.zip", "*.7z", "*.rar", "save/",
          "*.ppm", "mods/", "mod-texture-dump/")
# The Git command line. tests/test_clean_room_guard.py runs the guard with a
# fault-injecting wrapper here, in a child interpreter, so the injection works
# where an extensionless "git" script on PATH would not (Windows resolves
# git.exe, not a script).
GIT = ("git",)


def git(*args, data=None):
    return subprocess.run([*GIT, *args], input=data, stdout=subprocess.PIPE,
                          check=True).stdout


def nul_fields(data):
    if data and not data.endswith(b"\0"):
        raise ValueError("unterminated Git path inventory")
    return data[:-1].split(b"\0") if data else []


def added_paths():
    # -m inspects merge-parent diffs too: a file introduced only by a merge and
    # later deleted must not disappear from the historical filename audit.
    fields = nul_fields(git("log", "--all", "--root", "-m", "--no-renames",
                            "--diff-filter=A", "--raw", "-z", "--format="))
    if len(fields) % 2:
        raise ValueError("malformed Git history inventory")
    result = []
    for index in range(0, len(fields), 2):
        record = fields[index].lstrip(b"\n")
        if not record.startswith(b":") or not record.endswith(b" A"):
            raise ValueError("unexpected Git history record")
        result.append(fields[index + 1])
    return result


def blob_inventory():
    ids = git("rev-list", "--objects", "--all", "--no-object-names").splitlines()
    if any(not re.fullmatch(rb"[0-9a-f]{40}|[0-9a-f]{64}", oid) for oid in ids):
        raise ValueError("invalid Git object ID")
    ids = sorted(set(ids))
    records = git("cat-file", "--batch-check=%(objectname) %(objecttype) %(objectsize)",
                  data=b"".join(oid + b"\n" for oid in ids)).splitlines()
    if len(records) != len(ids):
        raise ValueError("incomplete Git object inventory")
    blobs = []
    for oid, record in zip(ids, records):
        fields = record.split()
        if (len(fields) != 3 or fields[0] != oid or not fields[2].isdigit()
                or fields[1] not in (b"blob", b"tree", b"commit", b"tag")):
            raise ValueError("invalid/missing Git object metadata")
        if fields[1] == b"blob":
            blobs.append((oid, int(fields[2])))
    return blobs


def scan_headers(blobs):
    hits = []
    # File-backed stderr avoids a full stderr pipe deadlocking a content read.
    with tempfile.TemporaryFile() as errors:
        process = subprocess.Popen([*GIT, "cat-file", "--batch"],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=errors)
        try:
            for oid, size in blobs:
                process.stdin.write(oid + b"\n")
                process.stdin.flush()
                expected = oid + b" blob " + str(size).encode() + b"\n"
                if process.stdout.readline(256) != expected:
                    raise ValueError("invalid/truncated Git blob header")
                prefix = process.stdout.read(min(4, size))
                if len(prefix) != min(4, size):
                    raise ValueError("truncated Git blob prefix")
                remaining = size - len(prefix)
                while remaining:
                    chunk = process.stdout.read(min(remaining, 65536))
                    if not chunk:
                        raise ValueError("truncated Git blob contents")
                    remaining -= len(chunk)
                if process.stdout.read(1) != b"\n":
                    raise ValueError("invalid Git blob terminator")
                if prefix in MAGIC:
                    hits.append(oid)
            process.stdin.close()
            if process.stdout.read(1):
                raise ValueError("unexpected trailing Git blob output")
            if process.wait() != 0:
                raise ValueError("Git blob reader exited unsuccessfully")
        finally:
            if process.poll() is None:
                process.terminate()
            process.wait()
            process.stdout.close()
            if not process.stdin.closed:
                process.stdin.close()
            errors.seek(0)
            diagnostic = errors.read(8192)
            if diagnostic:
                sys.stderr.write(diagnostic.decode(errors="replace"))
    return hits


def main():
    root = git("rev-parse", "--show-toplevel")
    if not root.endswith(b"\n"):
        raise ValueError("invalid Git root")
    os.chdir(os.fsdecode(root[:-1]))
    tracked = nul_fields(git("ls-files", "-z"))
    history = added_paths()
    failures = []

    def reject(label, paths):
        if paths:
            failures.append(label)
            print("FAIL:", label)
            for path in paths[:20]:
                print(" ", repr(os.fsdecode(path)))
        else:
            print("ok: no", label)

    reject("ROM-extension paths tracked", [p for p in tracked if ROM.search(p)])
    reject("ROM-extension paths added in history", [p for p in history if ROM.search(p)])
    blobs = blob_inventory()
    reject("ROM header in historical blob", scan_headers(blobs))
    print(f"Scanned {len(blobs)} blobs, including blobs smaller than 1 KiB")
    limit = int(os.environ.get("CLEAN_ROOM_MAX_BLOB", "4194304"))
    if limit <= 0:
        raise ValueError("CLEAN_ROOM_MAX_BLOB must be positive")
    for oid, size in blobs:
        if size > limit:
            print(f"WARN: {oid.decode()} ({size} bytes); verify provenance in NOTICE.md")
    reject("emulator source tracked", [p for p in tracked if EMULATOR.search(p)])
    ignored = Path(".gitignore").read_text().splitlines()
    reject("required ignore patterns missing", [p.encode() for p in IGNORE if p not in ignored])
    large = []
    for raw in nul_fields(git("ls-files", "--others", "--exclude-standard", "-z")):
        # lstat: Git records a symbolic link as the link, never the bytes it
        # points at, so a link is not a large file whatever its target is --
        # and a dangling one must not end the scan with an unrelated error.
        info = os.lstat(os.fsdecode(raw))
        if stat.S_ISLNK(info.st_mode):
            target = os.readlink(os.fsdecode(raw))
            state = "" if os.path.exists(os.fsdecode(raw)) else " (missing target)"
            print(f"WARN: untracked symbolic link {os.fsdecode(raw)!r} -> "
                  f"{target!r}{state}; not followed")
        elif stat.S_ISREG(info.st_mode) and info.st_size > 4194304:
            large.append(raw)
    reject("unignored untracked files over 4 MiB", large)
    reject("content-pack payload tracked or in history",
           [p for p in tracked + history if p == b"mods" or
            p.startswith((b"mods/", b"mod-texture-dump/"))])
    reject("dumped texture payload tracked or in history",
           [p for p in tracked + history if DUMP.search(p)])
    if failures:
        return 1
    print("check_clean_room: PASS -- no ROM or bulk ROM-derived assets in the tree or in history.")
    return 0


def run():
    """main() with every scan failure reported, as the command line runs it."""
    try:
        status = main()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"check_clean_room: scan error: {error}", file=sys.stderr)
        status = 1
    if status:
        print("check_clean_room: FAIL -- clean-room claim is unproven. DO NOT PUBLISH.",
              file=sys.stderr)
    return status


if __name__ == "__main__":
    sys.exit(run())
