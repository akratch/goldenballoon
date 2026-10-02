#!/usr/bin/env python3
"""Choose the ROM-free test executables for the Windows test bundle.

Reads the CTest registrations of a MinGW BUILD_TESTING=ON build (the one
tools/mingw_cross_check.sh --intake-tests configures) and sorts every test into
"included" or "excluded, because ...". For the included ones it copies each
executable and each fixture it reads into the bundle and writes the manifest
RUN-TESTS.cmd and tools/run_windows_tests_wine.sh both run from.

Manifest (tests/tests.txt): one test per line, tab-separated fields

    name  executable  timeout-seconds  expect  environment  arguments  unicode
    folders

`expect` is a regular expression the output must match, case-sensitively ("-"
for none; CTest's PASS_REGULAR_EXPRESSION). Unlike CTest, the runners also
require exit 0 when there is one. `environment` and `arguments` are "|"-separated
lists ("-" for none). The runner starts each test in its own scratch folder,
so a relative path lands there, and rewrites two spellings:
    {WORK}   that scratch folder, as an absolute path
    data\\    the bundle's tests\\data, copied into the scratch folder first
`folders` ("|"-separated, "-" for none) are created in the scratch folder
before the test starts: the folders CMake makes at configure time for a test's
environment (file(MAKE_DIRECTORY)), which the test expects to find.
Every test runs from a scratch folder whose path is plain ASCII. A test marked
"unicode" runs a second time from one whose path has spaces, Latin-1 and CJK
letters, with TEMP and TMP inside it; those are the tests whose own fixtures
reach the file system through UTF-16 or the UTF-8 helpers, so a failure there
is the code under test, not the fixture.

Tests that CTest registers through a Python, Node or CMake driver cannot run
on a stock Windows PC and are listed as excluded, as are the game binary
itself, anything that opens network sockets, and GPU tests.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

# A floor, not a measurement: a test that finishes in a second on the build
# machine can take far longer on a slow or virus-scanned PC, and a timeout
# exists only to end a hang.
TIMEOUT_FLOOR_SECONDS = 300

# Tests that read a scratch argument through the UTF-16 command line rather
# than the ANSI argv, so they are handed an absolute scratch path, which is
# non-ASCII in their second run.
WIDE_ARGUMENT_TESTS = {"mod_source_zip"}

# Tests that also run from the non-ASCII folder (see the manifest above).
# Many fixtures in this tree use the ANSI APIs (GetTempPathA, tmpfile, mkdir,
# the narrow argv): from a folder the code page cannot spell they fail for the
# fixture's sake, which says nothing about the product, so they are not here.
# The intake tests are written for it. The others touch the file system and
# pass from that folder under Wine, whose ANSI code page (1252) cannot spell
# it either.
UNICODE_PATH_TESTS = {
    "fs_utf8", "fs_set_aside", "mod_source_zip", "pack_installer",
    "pack_install_job",
    "pack_installer_zip_argument",
    "portable_paths_marker", "app_config_persistence", "rom_browser",
    "file_browser_ui", "save_v170_fixture", "save_state_container",
    "video_config", "controller_mapping", "character_draft_store",
    "character_draft_transfer", "character_preview_cache",
    "modern_character_registry",
}

# Runs that CTest has no registration for. pack_installer takes an optional
# real pack ZIP, read from the UTF-16 command line on Windows; handing it a
# synthetic one through a non-ASCII path is the only run of that argument.
# The fixture is written below: a pack.ini and nothing else.
EXTRA_TESTS = [
    {"name": "pack_installer_zip_argument", "after": "pack_installer",
     "exe": "mdkr_pack_installer_test.exe",
     "args": ["{WORK}\\data\\windows-bundle-pack.zip"]},
]
PACK_FIXTURE = "windows-bundle-pack.zip"

# Explicit exclusions the generic rules below cannot see: fixtures that cannot
# work on any Windows PC, found by running the bundle under Wine
# (tools/run_windows_tests_wine.sh), where they fail from every folder for
# their own reason, not the code under test.
EXPLICIT_EXCLUSIONS: dict[str, str] = {
    # The test is ROM-free (synthetic glyphs), but tools/check_no_rom.sh
    # refuses any shipped file named like a font atlas, and that guard is not
    # this bundle's to loosen.
    "font_sdf": "its file name matches tools/check_no_rom.sh's font-atlas "
                "rule (*font_sdf*)",
    "online_lobby_core": "opens its fixture through the build machine's "
                         "source path (MDKR_SOURCE_DIR)",
    "lan_party_launch": "makes its fixture folder with std::system(\"mkdir -p\"), "
                        "a POSIX shell command",
}


def run_ctest_json(build_dir: Path) -> list[dict]:
    result = subprocess.run(
        ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"],
        check=True, capture_output=True, text=True)
    return json.loads(result.stdout)["tests"]


def registered_programs(build_dir: Path) -> dict[str, str]:
    """The program each test names, built or not (CTest's JSON leaves the
    command out when the executable does not exist)."""
    programs = {}
    pattern = re.compile(r'^add_test\((\S+) "([^"]+)"', re.MULTILINE)
    text = (build_dir / "CTestTestfile.cmake").read_text(encoding="utf-8")
    for match in pattern.finditer(text):
        programs[match.group(1)] = Path(match.group(2)).name
    return programs


def properties(test: dict) -> dict:
    return {item["name"]: item["value"] for item in test.get("properties", [])}


def imports_of(objdump: str, exe: Path) -> list[str]:
    dump = subprocess.run([objdump, "-p", str(exe)], check=True,
                          capture_output=True, text=True).stdout
    return [line.split()[-1] for line in dump.splitlines() if "DLL Name" in line]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--tests-dir", required=True, type=Path,
                        help="the bundle's tests folder to fill")
    parser.add_argument("--objdump", default="x86_64-w64-mingw32-objdump")
    options = parser.parse_args()

    build_dir = options.build_dir.resolve()
    repo_root = options.repo_root.resolve()
    tests_dir = options.tests_dir
    data_dir = tests_dir / "data"
    tests_dir.mkdir(parents=True, exist_ok=True)

    manifest: list[str] = []
    included: list[tuple[str, str]] = []
    excluded: list[tuple[str, str]] = []
    executables: set[str] = set()
    fixtures: dict[str, Path] = {}

    programs = registered_programs(build_dir)
    socket_excluded = 0
    for test in run_ctest_json(build_dir):
        name = test["name"]
        command = test.get("command") or []
        props = properties(test)
        labels = props.get("LABELS", [])
        if name in EXPLICIT_EXCLUSIONS:
            excluded.append((name, EXPLICIT_EXCLUSIONS[name]))
            continue
        # Both spellings: the CTest file (built or not) and the JSON command,
        # including any argument naming the game, so a test name the regex
        # misreads can still never ship a game-launching test.
        if programs.get(name) == "mdkr64.exe" or any(
                Path(part).name.lower() == "mdkr64.exe" for part in command):
            excluded.append((name, "runs the game itself, which the acceptance "
                                   "session covers"))
            continue
        if not command:
            excluded.append((name, "its executable did not build for Windows "
                                   "(see the build log)"))
            continue
        program = Path(command[0])
        if program.suffix.lower() != ".exe" or program.parent != build_dir:
            excluded.append((name, f"runs through {program.name}, which a stock "
                                   "Windows PC does not have"))
            continue
        if "gpu" in labels:
            excluded.append((name, "needs a GPU and a display"))
            continue
        dlls = {dll.lower() for dll in imports_of(options.objdump, program)}
        if "ws2_32.dll" in dlls:
            excluded.append((name, "opens network sockets, so Windows "
                                   "Firewall would ask about it (see the note "
                                   "below)"))
            socket_excluded += 1
            continue
        for unsupported in ("WILL_FAIL", "FAIL_REGULAR_EXPRESSION",
                            "SKIP_REGULAR_EXPRESSION", "SKIP_RETURN_CODE"):
            if unsupported in props:
                raise SystemExit(f"{name}: {unsupported} is not supported by "
                                 "the Windows runner; teach both runners it "
                                 "or exclude the test")

        arguments: list[str] = []
        for argument in command[1:]:
            if "|" in argument or "\t" in argument:
                raise SystemExit(f"{name}: argument {argument!r} cannot be "
                                 "written to the manifest")
            path = Path(argument)
            if path.is_absolute() and path.exists() and \
                    repo_root in path.parents and build_dir not in path.parents:
                # A committed fixture: ship it and pass the copy.
                key = path.name
                if fixtures.get(key, path) != path:
                    raise SystemExit(f"two fixtures are both named {key}")
                fixtures[key] = path
                arguments.append(f"data\\{key}")
            elif path.is_absolute() and (path == build_dir or build_dir in path.parents):
                # A scratch folder CTest puts in the build tree.
                if name in WIDE_ARGUMENT_TESTS:
                    arguments.append("{WORK}\\" + path.name)
                else:
                    arguments.append(path.name)
            elif path.is_absolute():
                raise SystemExit(f"{name}: no Windows spelling for {argument}")
            else:
                arguments.append(argument)

        environment: list[str] = []
        folders: list[str] = []
        for item in props.get("ENVIRONMENT", []):
            key, _, value = item.partition("=")
            if str(build_dir) in value:
                # A scratch folder in the build tree: the same name, relative
                # to the test's own folder, made first if CMake made it.
                if Path(value).is_dir():
                    folders.append(Path(value).name)
                value = Path(value).name
            if "|" in item or "\t" in item or value.startswith("/"):
                raise SystemExit(f"{name}: no Windows spelling for {item}")
            environment.append(f"{key}={value}")

        expect = props.get("PASS_REGULAR_EXPRESSION", [])
        expect_field = "|".join(f"(?:{pattern})" for pattern in expect) or "-"
        if "\t" in expect_field:
            raise SystemExit(f"{name}: a tab in PASS_REGULAR_EXPRESSION")
        timeout = max(int(float(props.get("TIMEOUT", 0))), TIMEOUT_FLOOR_SECONDS)

        manifest.append("\t".join([
            name, program.name, str(timeout), expect_field,
            "|".join(environment) or "-", "|".join(arguments) or "-",
            "unicode" if name in UNICODE_PATH_TESTS else "-",
            "|".join(folders) or "-"]))
        executables.add(program.name)
        included.append((name, program.name))
        for extra in EXTRA_TESTS:
            if extra["after"] != name:
                continue
            manifest.append("\t".join([
                extra["name"], extra["exe"], str(timeout), "-", "-",
                "|".join(extra["args"]),
                "unicode" if extra["name"] in UNICODE_PATH_TESTS else "-", "-"]))
            executables.add(extra["exe"])
            included.append((extra["name"], extra["exe"] + " " + " ".join(extra["args"])))

    unplaced = {extra["after"] for extra in EXTRA_TESTS} - {name for name, _ in included}
    if unplaced:
        raise SystemExit(f"extra runs follow tests that were not included: {unplaced}")
    unknown = UNICODE_PATH_TESTS - {name for name, _ in included}
    if unknown:
        raise SystemExit(f"UNICODE_PATH_TESTS names tests that were not included: {unknown}")

    for exe in sorted(executables):
        shutil.copy2(build_dir / exe, tests_dir / exe)
    for key, source in sorted(fixtures.items()):
        destination = data_dir / key
        if source.is_dir():
            shutil.copytree(source, destination)
        else:
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
    if PACK_FIXTURE in fixtures:
        raise SystemExit(f"a committed fixture is already named {PACK_FIXTURE}")
    data_dir.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(data_dir / PACK_FIXTURE, "w", zipfile.ZIP_DEFLATED) as pack:
        entry = zipfile.ZipInfo("pack.ini", date_time=(2026, 1, 1, 0, 0, 0))
        pack.writestr(entry, "[pack]\nname=Windows bundle\n")

    header = ("# name\texecutable\ttimeout-seconds\texpect\tenvironment\targuments\tunicode\tfolders\n"
              "# Written by tools/windows_test_bundle/select_tests.py; "
              "read by RUN-TESTS.cmd.\n")
    (tests_dir / "tests.txt").write_text(header + "\n".join(manifest) + "\n",
                                         encoding="utf-8", newline="\r\n")

    width = max(len(name) for name, _ in included + excluded)
    lines = [f"Included ({len(included)} tests, {len(executables)} executables):"]
    lines += [f"  {name:<{width}}  {exe}" for name, exe in included]
    lines += ["", f"Excluded ({len(excluded)} tests):"]
    lines += [f"  {name:<{width}}  {reason}" for name, reason in excluded]
    lines += ["", "Fixtures shipped in tests\\data:"]
    lines += [f"  {key}  (from {source.relative_to(repo_root)})"
              for key, source in sorted(fixtures.items())]
    lines += [f"  {PACK_FIXTURE}  (written by select_tests.py: a pack.ini only)"]
    lines += ["", "Also run from a non-ASCII folder: " + ", ".join(sorted(UNICODE_PATH_TESTS))]
    if socket_excluded:
        lines += ["", f"Note: with the {socket_excluded} socket tests excluded, this bundle runs no "
                      "Winsock code at all; Windows networking (LAN party, online, phone "
                      "party transport) is not tested by it."]
    (tests_dir / "selection.txt").write_text("\n".join(lines) + "\n",
                                             encoding="utf-8", newline="\r\n")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
