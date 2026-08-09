# S6 — Release engineering: green CI, nightlies, signing, updates

> **Executing this plan:** work the tasks in order, one `- [ ]` step at a time.
> Tick a step only once its command has been run and its output read. The
> run-it-and-watch-it-fail steps are load-bearing; skipping ahead to the
> implementation forfeits the positive control that makes the gate mean anything.

**Goal:** Get the first green hosted CI run, turn branch protection on behind it,
publish per-platform nightly builds, sign the Windows artifact, and tell players
in-app when a new version exists.

**Architecture:** The workflow matrix already exists and its checks are real —
`correctness` (policy, native matrix, sanitizers, web), `release`
(validate, linux, windows), `windows-validate`, `macos-release`, `web-demo`. What
is missing is a run that has actually passed, and the scheduled and signed paths
that follow from it. Nothing here should rewrite the matrix; the work is to make
it pass, then extend it.

**Tech stack:** GitHub Actions, the existing `tests/check_ci_contract.py` and
`tests/ci_contract_manifest.py`, `tools/check_github_branch_protection.py`,
`tools/release/`, `signtool`/Azure Trusted Signing for Windows, the existing
macOS notarization path.

## Global constraints

- **This sprint is the one that must not lie.** Every claim it makes is about
  whether something passed; a claim recorded without the run behind it defeats
  the entire purpose. Paste the run URL, not a summary.
- Nightly artifacts must be labelled as untested playtest builds wherever they
  appear, and must never be linked from `README.md`'s download table.
- Every release file keeps its `.provenance.json` naming the exact source commit
  and SHA-256.
- Push the tested tree: publish the exact commit the suite ran on. A re-resolved
  conflict after the suite is a new tree and needs a new run.
- The ROM-absence guard runs on every artifact, in every workflow, including
  nightlies. No exceptions for speed.
- Run `python3 tools/check_public_surface.py --staged` before every commit.

---

## User stories

**US-1 — Trust the badge.** As a contributor, I open a pull request and hosted CI
tells me whether it is correct, so that review is about design rather than
whether it builds.

**US-2 — Protect the branch.** As the maintainer, `main` cannot take a commit
that has not passed the checks, so that a broken tree cannot ship.

**US-3 — Try tonight's build.** As a player following development, I download a
nightly for my platform and try a fix before it is released, so that feedback
arrives before the release rather than after.

**US-4 — Install without a warning.** As a Windows player, the download does not
present an unknown-publisher warning, so that I am not asked to override my own
operating system.

**US-5 — Know there is an update.** As a player, the app tells me when a newer
release exists and links to it, so that I am not running a version with a fixed
bug in it.

**US-6 — Understand a nightly.** As a player, a nightly is clearly marked as
untested, so that I know what I am choosing.

---

## Milestones and acceptance criteria

### M1 — First green hosted run

**Done when:** the `correctness` workflow has completed green on `main`, every
job included, with the run URL recorded in `docs/DEFINITION_OF_DONE.md`. Not "the
workflow is fixed" — completed, green, linked.

### M2 — Branch protection

**Done when:** `main` requires the `correctness` jobs to pass before merge, and
`tools/check_github_branch_protection.py` passes against the live repository
configuration rather than an intended one.

### M3 — Nightlies

**Done when:**
- A scheduled workflow builds Linux, Windows, macOS and web artifacts daily from
  `main`, but only when `main` moved since the last nightly.
- Each artifact carries its `.provenance.json` and passes the ROM-absence guard.
- Nightlies are published as a rolling pre-release, retained for 14 days, and
  labelled untested in the release body, the filename, and the in-app version
  string.
- A failed nightly is visible — it does not silently leave yesterday's build in
  place as if it were current.

### M4 — Windows code signing

**Done when:** the Windows artifact is signed, `signtool verify /pa` passes in
the workflow, and the signing credential lives in a protected environment that
pull requests from forks cannot reach.

### M5 — Update notification

**Done when:** the app checks for a newer release at most once per day, off a
static endpoint, with an explicit opt-out; shows an unobtrusive notice with the
version and a link; never downloads or installs anything; and fails silently
offline.

---

## File structure

**Create:**

| Path | Responsibility |
|---|---|
| `.github/workflows/nightly.yml` | Scheduled multi-platform playtest builds. |
| `platform/update_check.h/.c` | Version comparison and the fetch policy. |
| `tests/test_update_check.c` | Version-compare and policy unit test, offline. |
| `tests/check_nightly_contract.py` | Nightly artifact shape and labelling gate. |
| `docs/CI.md` | What each workflow proves, and what it does not. |

**Modify:** `.github/workflows/correctness.yml`, `.github/workflows/release.yml`,
`tools/check_github_branch_protection.py`, `tests/ci_contract_manifest.py`,
`tests/check_ci_contract.py`, `platform/app/ui_launcher.cpp`,
`platform/video_config.h/.c`, `cmake/tests.cmake`, `CMakeLists.txt`,
`tools/run_checks.py`, `docs/DEFINITION_OF_DONE.md`, `ROADMAP.md`,
`docs/RELEASE_CHECKLIST.md`.

---

## Task 1: Make the hosted run green

**Files:** `.github/workflows/correctness.yml` and whatever the failures point at.

This task has no predetermined diff. Its output is a green run and a written
account of what was wrong.

- [ ] **Step 1: Trigger the workflow and capture the real failure.**

```bash
gh workflow run correctness.yml --ref main
gh run list --workflow=correctness.yml --limit 1
gh run watch
```

- [ ] **Step 2: For each failing job, get the log and classify the failure**
  as one of: a genuine defect, a hosted-environment difference (no display, no
  GPU, no ROM), a timeout, or a missing secret. Write the classification down
  before fixing anything — the mix determines whether the fix belongs in the
  workflow or in the code.

```bash
gh run view --log-failed
```

- [ ] **Step 3: Fix environment differences in the workflow, not the checks.**
  A check that needs a display or a ROM must be *excluded by role* in the hosted
  matrix, using the existing role mechanism in `tools/run_checks.py`
  (`rom`, `native`, `ctest`, `wasm`, `browser`, `browser_save`), not disabled.
  `tests/check_ci_contract.py` exists to catch exactly this drift — keep it
  passing.

- [ ] **Step 4: Fix genuine defects as ordinary defects** — root cause first,
  positive control, open-items write-up. Do not paper over one in the workflow.

- [ ] **Step 5: Re-run until green, then record it.** Add the run URL and date to
  `docs/DEFINITION_OF_DONE.md` under a new "Hosted CI" row, and delete the
  "Hosted CI has never run green" bullet from `ROADMAP.md`'s Project
  infrastructure section — in the same commit, so the two cannot disagree.

- [ ] **Step 6: Commit.**

---

## Task 2: Branch protection

**Files:** `tools/check_github_branch_protection.py`, `docs/CI.md`

- [ ] **Step 1: Read the existing checker** to see exactly which properties it
  asserts, so the configuration is set to satisfy the checker rather than the
  checker relaxed to match the configuration.

- [ ] **Step 2: Enable protection on `main`** requiring the `correctness` jobs,
  linear history, and no force pushes.

- [ ] **Step 3: Run the checker against the live configuration.**

```bash
python3 tools/check_github_branch_protection.py
```

Paste the output.

- [ ] **Step 4: Prove it bites.** Attempt a direct push of a trivial commit to
  `main` from a scratch clone and confirm it is rejected. This is the positive
  control; without it, "protection is on" is a setting, not a fact.

- [ ] **Step 5: Commit the recorded evidence.**

---

## Task 3: Nightly workflow

**Files:**
- Create: `.github/workflows/nightly.yml`, `tests/check_nightly_contract.py`
- Modify: `tools/run_checks.py`, `README.md`

- [ ] **Step 1: Write the contract gate first.**
  `tests/check_nightly_contract.py` asserts, against a locally produced artifact
  set:
  - every artifact filename contains `nightly` and the short commit sha;
  - every artifact has a sibling `.provenance.json` whose commit matches the
    filename's sha and whose SHA-256 matches the artifact;
  - `tools/check_no_rom.sh` passes on every artifact;
  - the in-app version string reported by the built binary contains `nightly`,
    so a screenshot from a nightly is identifiable without the filename.

- [ ] **Step 2: Run it, verify it fails.**

- [ ] **Step 3: Write the workflow.** Schedule daily; guard with a step that
  exits early when `main`'s head equals the previous nightly's recorded sha.
  Reuse the existing `release.yml` build jobs by extracting them into a reusable
  workflow rather than copying them — two divergent copies of the packaging logic
  is how a nightly starts passing while the release path breaks.

- [ ] **Step 4: Publish as a rolling pre-release** with a 14-day retention sweep,
  and a body that states in the first line that these are untested playtest
  builds which will contain bugs.

- [ ] **Step 5: Make failure visible.** On failure, the workflow must delete or
  clearly mark the stale rolling release rather than leaving it presented as
  current.

- [ ] **Step 6: Trigger it manually, watch it, and run the gate against the
  published artifacts.**

- [ ] **Step 7: Add a short "Nightly builds" section to `README.md`** — separate
  from the download table, labelled untested, in player-facing language.

- [ ] **Step 8: Register in `CHECKS` and commit.**

---

## Task 4: Version comparison and update policy (offline, pure)

**Files:**
- Create: `platform/update_check.h`, `platform/update_check.c`,
  `tests/test_update_check.c`
- Modify: `platform/video_config.h/.c`, `cmake/tests.cmake`, `CMakeLists.txt`

Build and test the policy with no network at all; Task 6 adds the fetch.

- [ ] **Step 1: Append the key and its schema rows.**

```c
    MDKR_APP_UPDATE_CHECK,
```

```c
    [MDKR_APP_UPDATE_CHECK] = {
        "App.UpdateCheck", "MDKR_UPDATE_CHECK",
        MDKR_VIDEO_TYPE_INT, MDKR_VIDEO_SCOPE_LIVE, 0.0f, 1.0f,
        "Check for updates",
        "Look once a day for a newer release and show a notice. "
        "Nothing is downloaded or installed.",
        MDKR_VIDEO_CAT_INTERFACE
    },
```

Add the matching default/range/text table entries.

- [ ] **Step 2: Write the failing test.** `tests/test_update_check.c`:
  - `1.1.0` vs `1.1.1` → newer available;
  - `1.1.0` vs `1.1.0` → none;
  - `1.2.0` vs `1.1.9` → none (a running build ahead of the feed is not
    downgraded);
  - `1.1.0` vs `1.1.0-nightly.abc1234` → none, because a nightly is not an
    upgrade from a release;
  - `1.1.0-nightly.abc1234` vs `1.1.0` → **newer**, because a release supersedes
    the nightly it came from;
  - a malformed feed version string → none, and a reason, never a crash;
  - the policy returns "do not check" when the last check was under 24 hours ago,
    and "check" at exactly 24 hours, with the clock injected as a parameter so the
    test needs no real time.

- [ ] **Step 3: Register and run to verify it fails.**

- [ ] **Step 4: Implement.** Pure semver comparison plus the interval policy, no
  I/O in this translation unit.

- [ ] **Step 5: Run the test. Verify it passes.**

- [ ] **Step 6: Commit.**

---

## Task 5: Windows code signing

**Files:** `.github/workflows/release.yml`, `.github/workflows/nightly.yml`,
`docs/RELEASE_CHECKLIST.md`

- [ ] **Step 1: Put the credential in a protected environment**, configured so
  that fork pull requests cannot reach it. Mirror the existing macOS approach in
  `.github/workflows/macos-release.yml`, which already imports a certificate into
  an ephemeral keychain.

- [ ] **Step 2: Sign after the last mutation.** Any step that rewrites the
  executable — packaging, stamping, provenance — invalidates the signature. Sign
  last, then hash, then write `.provenance.json` over the signed bytes.

- [ ] **Step 3: Verify in the workflow.**

```powershell
signtool verify /pa /v Golden-Balloon.exe
```

Fail the job on a non-zero exit. A signing step whose result is not verified is
not a signing step.

- [ ] **Step 4: Confirm the artifact still passes the existing gates.**

```bash
bash tools/check_windows_imports.sh
bash tools/check_no_rom.sh
```

- [ ] **Step 5: Add the signing and verification rows to
  `docs/RELEASE_CHECKLIST.md`**, and update `ROADMAP.md`'s "Windows signing
  remains open" line.

- [ ] **Step 6: Commit.**

---

## Task 6: The update notice

**Files:**
- Modify: `platform/update_check.c`, `platform/app/ui_launcher.cpp`,
  `.github/workflows/release.yml`

- [ ] **Step 1: Publish a static feed.** A single small JSON file written by the
  release workflow to the existing GitHub Pages site, containing the latest
  release version and its URL. Static means no service to run and nothing to
  break.

- [ ] **Step 2: Fetch it off the main thread and time-bounded.** A blocked
  network call on the launcher thread turns a bad connection into a hang. Bound
  the request to a few seconds and treat every failure — DNS, timeout, malformed
  body — as "no update information", silently.

- [ ] **Step 3: Show the notice in the launcher only**, never during play, as a
  single line with the new version and a link. Do not download. Do not install.
  Do not modal.

- [ ] **Step 4: Honour the opt-out completely.** With `App.UpdateCheck=0` no
  request is made at all — assert this with a test that runs the launcher with a
  poisoned proxy environment variable and asserts no connection attempt.

- [ ] **Step 5: Run the launcher tests.**

```bash
MDKR_AUDIO=0 python3 tests/check_app_ui_input.py
MDKR_AUDIO=0 python3 tests/check_shell_dropfile.py
```

- [ ] **Step 6: Commit.**

---

## Task 7: `docs/CI.md` and the honesty pass

**Files:**
- Create: `docs/CI.md`
- Modify: `docs/README.md`, `docs/DEFINITION_OF_DONE.md`, `ROADMAP.md`,
  `docs/RELEASE_CHECKLIST.md`

- [ ] **Step 1: Write `docs/CI.md`** — one section per workflow: what it runs,
  what it proves, and **what it does not**. The Windows story in particular
  already has a stated evidence boundary — hosted CI proves the binary, imports,
  package and extracted startup, but does not provide a stable GPU environment
  for rendered gameplay. That boundary must survive this sprint intact and be
  stated here.

- [ ] **Step 2: Reconcile the four documents** so the CI claims agree across
  `docs/CI.md`, `docs/DEFINITION_OF_DONE.md`, `ROADMAP.md` and
  `docs/RELEASE_CHECKLIST.md`.

- [ ] **Step 3: Run the link checker.** `python3 tools/check_markdown_links.py`

- [ ] **Step 4: Run the complete suite, sequentially, and paste the summary.**

```bash
rm -f save/eeprom.bin
MDKR_AUDIO=0 python3 tools/run_checks.py
```

- [ ] **Step 5: Commit.**

---

## Self-review

**Spec coverage.** M1 → Task 1. M2 → Task 2. M3 → Task 3. M4 → Task 5.
M5 → Tasks 4 and 6. US-6 → Task 3 Steps 1, 4 and 7.

**Deliberately out of scope:**

- **Self-updating.** Downloading and replacing a running binary is a large
  security surface for a portable app that is trivial to re-download. The notice
  is the useful 90%.
- **A package-manager presence** (Homebrew, Flatpak, winget). Worth doing, but it
  depends on signed, nightly-tested artifacts existing first — this sprint is its
  precondition, not its home.
- **Hosted GPU runners.** The Windows rendered-gameplay boundary is a hardware
  fact, not a workflow bug; S4's acceptance routes are where it is addressed.

**Type consistency.** `mdkr_update_check_*` is introduced in Task 4 and
consumed in Task 6. `App.UpdateCheck` is appended in Task 4 Step 1 and read in
Task 6 Step 4. The nightly artifact naming contract is defined in Task 3 Step 1
and produced in Task 3 Step 3.
