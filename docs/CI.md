# Hosted CI — what each workflow proves, and what it does not

Five workflows. This page states, per workflow, the claim it supports and the
claim it does **not**, because the second half is the part that gets forgotten
and then quietly assumed.

`tests/check_ci_contract.py` is the gate that keeps the workflows from drifting
away from this description. It asserts the push/PR/manual policy, the three-cell
native matrix, the sanitizer lane, linked wasm and browser save custody, the ROM
guards, GPU-qualified Linux artifacts, non-publishing Windows validation,
fail-closed unsigned WebGPU, optional trusted macOS releases, and 14 immutable
action pins. If you change a workflow, that check tells you whether you also
changed a promise.

## Current state

Hosted CI **has run green on `main`**: run `31248954626`, commit
`080c4c4e6480eef86afe1f964766d27d2e618fda`, 2026-08-08, all six jobs. That
retires the long-standing "every claim in this repository rests on local runs"
caveat.

It does **not** retire the next one: `main` does not yet *require* those jobs to
pass before a commit lands. The run proves the tree passed; it does not prove a
failing tree cannot land. `tools/check_github_branch_protection.py` exists to
assert the configuration and has not been run green against the live repository.
Enabling protection is a repository-settings change rather than a source change,
which is why it is tracked in [`../ROADMAP.md`](../ROADMAP.md) rather than
claimed here.

The run history also shows `correctness` failing on several feature branches in
the same period. The workflow is green on `main`, not uniformly green, and those
failures are worth classifying before protection is turned on — a required check
that is flaky blocks honest work.

---

## `correctness.yml`

**Triggers:** push, pull request, manual.

| Job | Proves |
|---|---|
| **Policy and source contracts** | Clean-room history, the public tree and new-commit boundary, native SDK surface, third-party notices, image metadata, shell and Python syntax, documentation links, ignored-artifact hygiene, and the CI/WebGPU source contracts. |
| **Native** (3-cell matrix) | The tree configures, builds, and passes the ROM-free unit and format contracts on Linux OpenGL-only, Linux WebGPU, and macOS WebGPU. |
| **Linux ROM-free ASan + UBSan** | The ROM-free surface is clean under AddressSanitizer and UndefinedBehaviorSanitizer. |
| **Linked wasm and browser save custody** | The wasm target links, and browser save custody holds. |

**What it does not prove.** Everything that needs a ROM. The overwhelming
majority of this project's evidence — every pixel comparison, every state-hash
stream, every race, the whole `tests/check_*.py` corpus in the `rom` role —
cannot run here, because no ROM may exist on a hosted runner. Hosted CI is a
*structural* gate. The behavioural gate is the local suite, and
[`../CONTRIBUTING.md`](../CONTRIBUTING.md) is where that obligation lives.

It also does not prove anything about rendered output. The native cells build
and run ROM-free units; they do not produce frames.

## `release.yml` — "Portable release (Linux qualified; Windows validation only)"

**Trigger:** manual.

| Job | Proves |
|---|---|
| **Validate release inputs and source policy** | The requested version and the source policy agree. |
| **Linux app (AppImage + tar.gz)** | A Linux artifact builds and passes its publish gates, including the built and extracted WebGPU/GL pixel gates. |
| **Windows portable validation** | The Windows binary builds, its import table is stock-Windows clean, the package shape is right, and the extracted archive starts. |

**What it does not prove — and this boundary is load-bearing.** The Windows job
is named *validation* and publishes nothing, deliberately. Hosted Windows gives
no stable GPU environment, so rendered gameplay, controller behaviour, audio,
saves and relaunch are **not** covered there. Those rest on a manual pass on real
hardware, and the release notes say so rather than letting the green check imply
otherwise.

The Linux publish gate is fail-closed: if the software-GPU pixel gates do not
pass, the named Linux files are absent from the release. An absent artifact means
"not qualified", not "forgot to upload" — do not redistribute an unverified
workflow artifact in its place.

## `windows-validate.yml` — "Windows validate (execution)"

**Trigger:** manual. A focused execution check on `windows-latest`, separate from
the release path so it can be run without cutting a release. Same GPU caveat as
above.

## `macos-release.yml`

**Trigger:** manual. Packages on `macos-14`, publishes from `ubuntu-24.04`.
Imports a Developer ID certificate into an ephemeral keychain, signs inside-out
with Hardened Runtime, notarizes and staples the app, signs and notarizes the
DMG, mounts it, and requires Gatekeeper acceptance.

**What it does not prove.** The trusted path is optional — an unsigned build is
still producible and is what has shipped so far. The first accepted Apple notary
run is still owed, so the signing path is implemented and unproven, not proven.
Whether the bundle runs on Intel is a separate question entirely; there is no
Intel slice yet.

## `web-demo.yml`

**Trigger:** on push to the relevant paths. Builds the wasm target and deploys
the browser build to GitHub Pages.

**What it does not prove.** Browser breadth beyond current Chromium. The
deployment succeeding says the artifact exists, not that it renders correctly on
any given machine.

---

## What is missing

- **Branch protection.** Above.
- **Nightly builds.** No workflow has a `schedule:` trigger. Every artifact
  today comes from a tag or a manual dispatch, so nobody plays a change before
  it is released. This is
  [S6 task 3](sprints/S6-release-engineering.md#task-3-nightly-workflow).
- **Windows code signing.** Open. The macOS path exists as the model.
- **A GPU-bearing Windows runner.** Not a workflow bug — a hardware fact. It is
  addressed by the manual routes in
  [`PLATFORM_ACCEPTANCE.md`](PLATFORM_ACCEPTANCE.md), not by more CI.

## The rule this page exists to protect

A green check is evidence for exactly the claim its job makes, and no more. When
someone reads "CI is green" and concludes "Windows rendering works", the mistake
was not theirs — it was made here, by not writing down what the check covers.
