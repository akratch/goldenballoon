#!/usr/bin/env python3
"""Fail closed unless every release lane makes an explicit Phone Party choice.

`MDKR_PARTY_ORIGIN` is a CMake cache variable compiled INTO the launcher
(CMakeLists.txt). It is a property of the artifact, not of the machine that
runs it, and a build without it shows no Phone Party surface at all
(`PhoneParty_availableInBuild`). For three releases no workflow passed it, so
every packaged Windows/macOS/Linux artifact shipped with the feature silently
absent -- and nothing could see it: the compile succeeded, every unit test
passed, the binary linked the party host, and the only symptom was a feature
that was not there.

That is the defect class this gate exists for: *a release lane silently
omitting a required build parameter*. No compiler, linker, unit test or
runtime assertion can catch it, because the omission is well-formed at every
layer. The only place it is visible is the workflow file itself.

So: every workflow that produces a distributable artifact must

  1. thread `-DMDKR_PARTY_ORIGIN` (or `build_app_bundle.sh --party-origin`)
     into every CMake configure it runs, from a variable rather than a
     literal, bound to the release guard's resolved output; and
  2. carry the guard itself -- refuse to build unless a valid
     `vars.MDKR_PARTY_ORIGIN` is set or `vars.MDKR_ALLOW_PARTYLESS_RELEASE`
     is exactly `1`.

A workflow that packages and uploads but is not registered below is a failure
too: a new release lane must make this decision, not inherit silence.

The workflows are parsed structurally (job blocks, step blocks, run bodies)
rather than with PyYAML: the suite has no third-party Python dependency and
this gate must not be the first, matching tests/check_ci_contract.py's
regex-over-source house style.

Run with --self-test to also prove each assertion can still reject the
regression it names.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORKFLOWS = ROOT / ".github" / "workflows"

# A workflow "produces a distributable" when it both packages an artifact and
# uploads one. windows-validate.yml packages but deliberately uploads nothing,
# so it is not a release lane; if that ever changes, this gate demands it be
# registered below rather than quietly inheriting the old exemption.
PACKAGING = re.compile(
    r"package_linux_appimage\.sh|package_windows_zip\.sh|create_dmg\.sh"
    r"|build_app_bundle\.sh")
UPLOADS = re.compile(r"uses:\s*actions/upload-artifact@")

# CMake CONFIGURE invocations only. `cmake --build <dir>` re-uses an already
# configured tree and cannot carry the cache variable.
CONFIGURE = re.compile(r"cmake\s+(?:-S\s+\S+\s+)?-B\s|build_app_bundle\.sh")
THREADED = re.compile(
    r'-DMDKR_PARTY_ORIGIN="\$\{?PARTY_ORIGIN\}?"'
    r'|--party-origin\s+"\$\{?PARTY_ORIGIN\}?"')
# The guard's resolved value, consumed either across jobs or within one job.
ORIGIN_BINDING = re.compile(
    r"PARTY_ORIGIN:\s*\$\{\{\s*"
    r"(?:needs\.(?P<job>[A-Za-z0-9_-]+)|steps\.[A-Za-z0-9_-]+)"
    r"\.outputs\.party_origin\s*\}\}")
JOB_OUTPUT = re.compile(
    r"party_origin:\s*\$\{\{\s*steps\.[A-Za-z0-9_-]+\.outputs\.party_origin"
    r"\s*\}\}")

# Registered release lanes: workflow -> the job that must carry the guard.
LANES = {
    "release.yml": "validate",
    "macos-release.yml": "package",
}
# Lanes that launch the real AppHost and must therefore keep the direct
# packaged-build party witness, not just the compile-time threading.
WITNESS_LANES = {
    # Literal, counted: armed once on the built tree and once on the extracted
    # package, each asserting the trace line and a linked transport.
    "release.yml": (
        ("MDKR_APP_PARTY_TRACE=1", 2),
        ("[app] party: ", 2),
        ("transport=available", 2),
    ),
}


def split_blocks(text: str, indent: int) -> list[tuple[str, str]]:
    """Split a YAML region into (header, body) blocks at one list/key indent.

    Jobs are `^  name:` keys; steps are `^      - ` list items. Both are found
    by the same scan, which is all the structure this gate needs.
    """
    if indent == 2:
        start = re.compile(r"^ {2}([A-Za-z0-9_-]+):\s*$")
    else:
        start = re.compile(r"^ {%d}- " % indent)
    blocks: list[tuple[str, str]] = []
    name = ""
    current: list[str] = []
    for line in text.splitlines():
        match = start.match(line)
        if match:
            if current:
                blocks.append((name, "\n".join(current)))
            name = match.group(1) if indent == 2 else line.strip()
            current = [line]
        elif current:
            current.append(line)
    if current:
        blocks.append((name, "\n".join(current)))
    return blocks


def jobs_of(text: str) -> dict[str, str]:
    """Top-level job blocks, keyed by job id."""
    after_jobs = text.split("\njobs:\n", 1)
    if len(after_jobs) != 2:
        return {}
    return dict(split_blocks(after_jobs[1], 2))


def check_guard(name: str, job_name: str, job: str) -> list[str]:
    """The gate that makes a silent partyless release impossible."""
    required = {
        "reads vars.MDKR_PARTY_ORIGIN":
            "vars.MDKR_PARTY_ORIGIN" in job,
        "reads vars.MDKR_ALLOW_PARTYLESS_RELEASE":
            "vars.MDKR_ALLOW_PARTYLESS_RELEASE" in job,
        "requires an https:// origin":
            "^https://" in job,
        "rejects a trailing slash/path/query/fragment/credentials":
            re.search(r"\^https://\[A-Za-z0-9\]", job) is not None,
        "accepts the escape hatch only for exactly 1":
            re.search(r'"\$allow"\s*==\s*"1"', job) is not None,
        "says out loud that the artifact ships without Phone Party":
            "PARTYLESS RELEASE" in job,
        "fails the run when neither is set":
            job.count("exit 1") >= 2,
        "publishes the resolved origin as an output":
            'party_origin=$origin' in job and "$GITHUB_OUTPUT" in job,
    }
    return [f"{name}: job '{job_name}' guard {label}: missing"
            for label, present in required.items() if not present]


def check_workflow(name: str, text: str) -> list[str]:
    failures: list[str] = []
    jobs = jobs_of(text)
    guard_job = LANES[name]
    if guard_job not in jobs:
        return [f"{name}: expected guard job '{guard_job}' not found"]
    failures += check_guard(name, guard_job, jobs[guard_job])
    # A guard in its own job must publish the resolved origin as a job output;
    # a guard that is a step of the build job hands it to later steps through
    # steps.<id>.outputs and needs no job-level declaration.
    cross_job = f"needs.{guard_job}.outputs.party_origin" in text
    if cross_job and not JOB_OUTPUT.search(jobs[guard_job]):
        failures.append(
            f"{name}: job '{guard_job}' does not expose party_origin as an "
            "output for the build jobs to consume")

    configured = 0
    for job_name, job in jobs.items():
        for header, step in split_blocks(job, 6):
            if not CONFIGURE.search(step):
                continue
            configured += 1
            where = f"{name}: job '{job_name}' step {header!r}"
            if not THREADED.search(step):
                failures.append(
                    f"{where} configures a build without threading "
                    "MDKR_PARTY_ORIGIN -- this artifact would ship with Phone "
                    "Party absent")
                continue
            binding = ORIGIN_BINDING.search(step)
            if binding is None:
                failures.append(
                    f"{where} passes MDKR_PARTY_ORIGIN but does not bind "
                    "PARTY_ORIGIN to the guard's resolved party_origin output")
                continue
            producer = binding.group("job")
            if producer is None:
                continue
            if producer != guard_job:
                failures.append(
                    f"{where} consumes party_origin from '{producer}', not "
                    f"from the guard job '{guard_job}'")
            elif not re.search(
                    r"needs:.*\b%s\b" % re.escape(producer), job, re.S):
                failures.append(
                    f"{where} consumes {producer}.outputs.party_origin "
                    f"without declaring needs: {producer}")
    if configured == 0:
        failures.append(
            f"{name}: no CMake configure step found -- this gate would pass "
            "vacuously; the lane or this check has moved")

    for literal, minimum in WITNESS_LANES.get(name, ()):
        if text.count(literal) < minimum:
            failures.append(
                f"{name}: packaged-build party witness weakened: expected at "
                f"least {minimum} occurrences of {literal!r}")
    return failures


def check(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for name, text in sorted(sources.items()):
        binary_surface_calls = text.count(
            "python3 tests/check_party_binary_surface.py")
        if binary_surface_calls:
            origin_expectations = text.count('--expect-origin "$PARTY_ORIGIN"')
            origin_conditions = text.count(
                'if [[ -n "${PARTY_ORIGIN:-}" ]]; then')
            if origin_expectations != binary_surface_calls:
                failures.append(
                    f"{name}: {binary_surface_calls} packaged Phone Party "
                    "surface assertions exist but not all require the exact "
                    "resolved origin")
            if origin_conditions < binary_surface_calls:
                failures.append(
                    f"{name}: packaged Phone Party surface assertions are not "
                    "all conditional on a nonempty origin; this contradicts "
                    "the deliberate partyless release path")
        # Comment lines are prose about a lane, not the lane: a workflow that
        # explains why it does NOT upload must not read as one that does.
        live = "\n".join(line for line in text.splitlines()
                         if not line.lstrip().startswith("#"))
        distributable = PACKAGING.search(live) and UPLOADS.search(live)
        if name in LANES:
            if not distributable:
                failures.append(
                    f"{name}: registered as a release lane but no longer "
                    "packages and uploads an artifact; update this gate")
            failures += check_workflow(name, text)
        elif distributable:
            failures.append(
                f"{name}: packages and uploads an artifact but is not a "
                "registered release lane, so nothing checks that it compiles "
                "in a Phone Party origin")
    for name in LANES:
        if name not in sources:
            failures.append(f"{name}: registered release lane is missing")
    return failures


def load() -> dict[str, str]:
    return {path.name: path.read_text(encoding="utf-8")
            for path in sorted(WORKFLOWS.glob("*.yml"))}


MUTATIONS = (
    ("release.yml Linux configure loses the origin",
     "release.yml",
     '-DMDKR_VERSION="$RELEASE_VERSION" \\\n            '
     '-DMDKR_PARTY_ORIGIN="$PARTY_ORIGIN"',
     '-DMDKR_VERSION="$RELEASE_VERSION"'),
    ("release.yml guard loses its escape-hatch comparison",
     "release.yml", '"$allow" == "1"', '"$allow" != ""'),
    ("release.yml guard stops failing closed",
     "release.yml", "exit 1", "true"),
    ("release.yml guard stops publishing the origin",
     "release.yml", "party_origin=$origin", "resolved=$origin"),
    ("release.yml drops the packaged-build party witness",
     "release.yml", "MDKR_APP_PARTY_TRACE=1", "MDKR_APP_UI_TRACE=1"),
    ("release.yml build job hardcodes an origin instead of the guard output",
     "release.yml", '-DMDKR_PARTY_ORIGIN="$PARTY_ORIGIN"',
     '-DMDKR_PARTY_ORIGIN="https://party.example.invalid"'),
    ("macos-release.yml build loses --party-origin",
     "macos-release.yml", '--party-origin "$PARTY_ORIGIN" \\\n', ""),
    ("macos-release.yml loses the guard entirely",
     "macos-release.yml", "vars.MDKR_ALLOW_PARTYLESS_RELEASE", "vars.UNUSED"),
    ("release.yml binary surface assertion stops honoring partyless builds",
     "release.yml", 'if [[ -n "${PARTY_ORIGIN:-}" ]]; then',
     'if [[ -z "${PARTY_ORIGIN:-}" ]]; then'),
    ("a validation-only lane starts uploading its package",
     "windows-validate.yml",
     "      - name: Import-table guard (self-contained on stock Windows)\n",
     "      - uses: actions/upload-artifact@34e114876b0b11c390a56381ad16ebd13914f8d5\n"
     "        with:\n"
     "          name: windows-portable\n"
     "          path: dist/\n"
     "      - name: Import-table guard (self-contained on stock Windows)\n"),
)


def self_test(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for label, workflow, needle, replacement in MUTATIONS:
        mutated = dict(sources)
        text = mutated[workflow]
        if needle not in text:
            failures.append(
                f"self-test '{label}': control text not found in {workflow}; "
                "the lane changed shape and this gate is measuring nothing")
            continue
        mutated[workflow] = text.replace(needle, replacement)
        if not check(mutated):
            failures.append(
                f"self-test '{label}': the gate accepted a lane that would "
                "ship without Phone Party")
    return failures


def main() -> int:
    sources = load()
    failures = check(sources)
    if "--self-test" in sys.argv[1:]:
        failures += self_test(sources)
    if failures:
        print("FAIL: release lanes do not make a consistent Phone Party "
              "origin decision")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("PASS: every release lane gates and consistently threads its "
          "Phone Party origin decision (%s)" % ", ".join(sorted(LANES)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
