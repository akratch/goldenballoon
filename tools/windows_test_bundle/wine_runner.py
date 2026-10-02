#!/usr/bin/env python3
"""Run a Windows test bundle under Wine, the way RUN-TESTS.cmd runs it.

SUPPLEMENTARY EVIDENCE ONLY: Wine is not Windows. A pass here does not stand in
for the run on a Windows PC, and a failure may be Wine's own. This runs inside
the container tools/run_windows_tests_wine.sh starts and mirrors the runner in
RUN-TESTS.cmd: the bundle is unzipped into a folder with spaces and non-ASCII
letters; each test runs from a copy of its executable in its own scratch folder
with TEMP/TMP inside it, from an ASCII folder and, for the tests marked
"unicode", again from a folder with spaces, Latin-1 and CJK letters. Unlike
RUN-TESTS.cmd it runs several tests at once (--jobs), because every start of a
Windows program costs seconds under Wine in an emulated container.

Usage: wine_runner.py BUNDLE.zip [--jobs N] [--only REGEX]
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import zipfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

LABEL = "SUPPLEMENTARY (Wine, not Windows)"
PREFIX = Path(os.environ.get("WINEPREFIX", str(Path.home() / ".wine")))
DRIVE_C = PREFIX / "drive_c"


def windows_path(path: Path) -> str:
    return "C:\\" + str(path.relative_to(DRIVE_C)).replace("/", "\\")


def read_manifest(tests_dir: Path) -> list[dict]:
    tests = []
    for line in (tests_dir / "tests.txt").read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 8:
            raise SystemExit(f"bad manifest line: {line!r}")
        name, exe, timeout, expect, env, args, unicode, folders = fields
        tests.append({
            "name": name, "exe": exe, "timeout": int(timeout),
            "expect": "" if expect == "-" else expect,
            "env": [] if env == "-" else env.split("|"),
            "args": [] if args == "-" else args.split("|"),
            "unicode": unicode == "unicode",
            "folders": [] if folders == "-" else folders.split("|"),
        })
    return tests


def quote(argument: str) -> str:
    """The Windows command-line rule, as RUN-TESTS.cmd applies it. {WORK}
    becomes a folder with spaces in it, so it is always quoted."""
    if argument and not re.search(r'[\s"]', argument) and "{WORK}" not in argument:
        return argument
    out, slashes = '"', 0
    for character in argument:
        if character == "\\":
            slashes += 1
            continue
        out += "\\" * (2 * slashes + 1 if character == '"' else slashes) + character
        slashes = 0
    return out + "\\" * (2 * slashes) + '"'


def run_test(test: dict, tests_dir: Path, root: Path, index: int) -> dict:
    """Wine replaces TEMP and TMP from its registry whatever the Unix
    environment says, so each test starts through a small batch file in its
    folder that sets them, as RUN-TESTS.cmd does through the process
    environment. The batch file names the folder only as %CD%, so it stays
    ASCII whatever the folder is called."""
    work = root / f"{index:03d}"
    (work / "temp").mkdir(parents=True)
    for folder in test["folders"]:
        (work / folder).mkdir(parents=True, exist_ok=True)
    shutil.copy2(tests_dir / test["exe"], work / test["exe"])
    lines = ["@echo off", 'set "TEMP=%CD%\\temp"', 'set "TMP=%CD%\\temp"']
    for item in test["env"]:
        lines.append(f'set "{item.replace("{WORK}", "%CD%")}"')
    command = f'"%CD%\\{test["exe"]}"'
    for argument in test["args"]:
        if "data\\" in argument and not (work / "data").exists():
            shutil.copytree(tests_dir / "data", work / "data")
        if re.search(r'[%^&|<>!]', argument):
            raise SystemExit(f"{test['name']}: cannot pass {argument!r} through cmd")
        command += " " + quote(argument).replace("{WORK}", "%CD%")
    lines += [command, "exit /b %ERRORLEVEL%"]
    (work / "run.cmd").write_text("\r\n".join(lines) + "\r\n", encoding="ascii")
    started = time.monotonic()
    # Its own session, so a hung test and every Windows process it started
    # (each one a Unix process under Wine) can be ended without touching the
    # tests running beside it.
    process = subprocess.Popen(["wine", "cmd", "/d", "/c", "run.cmd"], cwd=work,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               start_new_session=True)
    try:
        stdout, stderr = process.communicate(timeout=test["timeout"])
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        stdout, stderr = process.communicate()
        output = (stdout + stderr).decode("utf-8", "replace")
        return {"passed": False, "summary": f"timed out after {test['timeout']} s",
                "seconds": time.monotonic() - started, "folder": windows_path(work),
                "output": output}
    seconds = time.monotonic() - started
    output = stdout.decode("utf-8", "replace")
    error = stderr.decode("utf-8", "replace")
    if error:
        output += "\n--- standard error ---\n" + error
    summary = ""
    if process.returncode != 0:
        summary = f"exit {process.returncode}"
    elif test["expect"] and not re.search(test["expect"], output):
        summary = "exit 0 but the output lacks: " + test["expect"]
    return {"passed": not summary, "summary": summary, "seconds": seconds,
            "folder": windows_path(work), "output": output}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("bundle_zip", type=Path)
    # The host script passes its options through the environment, so the
    # container's command line never has to quote them.
    parser.add_argument("--jobs", type=int, default=int(os.environ.get("WINE_LANE_JOBS", "6")))
    parser.add_argument("--only", default=os.environ.get("WINE_LANE_ONLY", ""))
    options = parser.parse_args()
    subprocess.run(["wineboot", "-i"], check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # One wineserver for the whole run; otherwise each test pays its start-up.
    subprocess.run(["wineserver", "-p"], check=False)
    place = DRIVE_C / "users" / "Public" / "Golden Balloon b\u00fcndel \u540d\u524d"
    place.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(options.bundle_zip) as archive:
        archive.extractall(place)
    bundles = [child for child in place.iterdir() if (child / "tests" / "tests.txt").exists()]
    if len(bundles) != 1:
        raise SystemExit("the zip does not hold exactly one bundle folder")
    bundle = bundles[0]
    tests_dir = bundle / "tests"
    tests = read_manifest(tests_dir)
    if options.only:
        tests = [test for test in tests if re.search(options.only, test["name"])]
    stamp = time.strftime("%H%M%S")
    temp_root = DRIVE_C / "users" / os.environ.get("USER", "root") / "AppData" / "Local" / "Temp"
    unicode_root = temp_root / f"Golden Balloon \u00dcn\u00ef \u540d\u524d {stamp}"
    ascii_root = temp_root / f"GoldenBalloonTests {stamp}"
    version = subprocess.run(["wine", "cmd", "/d", "/c", "ver"], capture_output=True,
                             check=False).stdout.decode("utf-8", "replace").strip()
    build = (tests_dir / "BUILD.txt").read_text(encoding="utf-8").strip()
    print(f"{LABEL}: {build}")
    print(f"{LABEL}: {version}; bundle folder {windows_path(bundle)}")
    print(f"{LABEL}: ASCII folder {windows_path(ascii_root)}")
    print(f"{LABEL}: non-ASCII folder {windows_path(unicode_root)}")
    print(f"{LABEL}: {len(tests)} tests, {options.jobs} at a time", flush=True)

    failures = []
    passed = 0
    with ThreadPoolExecutor(max_workers=max(1, options.jobs)) as pool:
        scheduled = []
        for index, test in enumerate(tests, 1):
            runs = [("ASCII folder", pool.submit(run_test, test, tests_dir, ascii_root, index))]
            if test["unicode"]:
                runs.append(("non-ASCII folder",
                             pool.submit(run_test, test, tests_dir, unicode_root, index)))
            scheduled.append((test, runs))
        for test, runs in scheduled:
            results = [(label, future.result()) for label, future in runs]
            parts = []
            for label, run in results:
                prefix = f"{label}: " if len(results) > 1 else ""
                parts.append(f"{prefix}{run['seconds']:.1f} s"
                             + ("" if run["passed"] else f", {run['summary']}"))
                if not run["passed"]:
                    failures.append((test["name"], label, run))
            ok = all(run["passed"] for _, run in results)
            passed += ok
            print(f"{LABEL} {'PASS' if ok else 'FAIL'}  {test['name']}  ({'; '.join(parts)})",
                  flush=True)
            for skip in results[0][1]["output"].splitlines():
                if re.search(r"\bSKIP\b", skip):
                    print(f"      {skip.strip()}")
    print()
    for name, label, run in failures:
        print(f"===== {name}, {label} ({run['summary']}) in {run['folder']}")
        print(run["output"].rstrip())
        print()
    shutil.rmtree(unicode_root, ignore_errors=True)
    shutil.rmtree(ascii_root, ignore_errors=True)
    subprocess.run(["wineserver", "-k"], check=False)
    verdict = "PASS" if passed == len(tests) else "FAIL"
    print(f"{LABEL} OVERALL: {verdict} ({passed} of {len(tests)} tests passed under Wine)")
    return 0 if passed == len(tests) else 1


if __name__ == "__main__":
    sys.exit(main())
