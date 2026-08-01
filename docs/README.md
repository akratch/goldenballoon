# Documentation

Golden Balloon is a native and browser source port of *Diddy Kong Racing*. This
directory is the reference material; the project overview, build instructions and
supported-ROM policy are in [`../README.md`](../README.md).

## Start here

| If you are… | Read, in this order |
|---|---|
| **Playing it** | [`../README.md`](../README.md) — build and run · [`SAVE_MANAGEMENT.md`](SAVE_MANAGEMENT.md) — keep, move, edit or recover progress · [`ROM_REVISIONS.md`](ROM_REVISIONS.md) — which ROMs work |
| **Fixing a bug** | [`DEVELOPER_HANDBOOK.md`](DEVELOPER_HANDBOOK.md) — especially §3 · [`open-items/`](open-items/README.md) — is it already known? · [`../tests/README.md`](../tests/README.md) — the check that must catch it |
| **Contributing a change** | [`../CONTRIBUTING.md`](../CONTRIBUTING.md) — build, the hard audio-safety rule, and what a fix must ship with |
| **Working on one subsystem** | [`architecture/`](architecture/README.md) — input, WebGPU, audio, race, web |
| **Looking for something to do** | [`../ROADMAP.md`](../ROADMAP.md) — the deferred work, honestly scoped |
| **Cutting a release** | [`RELEASE_CHECKLIST.md`](RELEASE_CHECKLIST.md) — the clean-room and artifact gates |
| **Checking the legal position** | [`../DISCLAIMER.md`](../DISCLAIMER.md), [`../NOTICE.md`](../NOTICE.md), [`../LICENSE`](../LICENSE) |

**If you read only one thing, read
[`DEVELOPER_HANDBOOK.md`](DEVELOPER_HANDBOOK.md) §3** — the recurring bug shapes
behind nearly every hard defect in this port, several of them completely silent.
Recognising them on sight saves days.

## Building and running

- **[`../README.md`](../README.md)** — native and browser quick starts, renderer
  selection, presentation modes, and the full flag reference.
- **[`../CONTRIBUTING.md`](../CONTRIBUTING.md)** — the build matrix, the
  **audio-safety rule** (always pass `--headless-frames N`), the regression
  suite, and what a fix has to ship with.
- **[`architecture/web.md`](architecture/web.md)** — how the wasm/WebGPU build is
  produced, and what the browser shell is responsible for.
- **[`ROM_REVISIONS.md`](ROM_REVISIONS.md)** — the supported ROM revisions
  (US 1.1 and European 1.1), how every other revision is identified and refused
  by name, and what it would take to add one.

## Architecture

- **[`ARCHITECTURE_DECISIONS.md`](ARCHITECTURE_DECISIONS.md)** — the standing
  decisions: cooperative single thread, pointer tokens, the arena model,
  normalise-at-load endianness. The decisions are current; the milestone
  narrative below them is history.
- **[`architecture/`](architecture/README.md)** — one specification per subsystem
  of the port layer: [input](architecture/input.md),
  [WebGPU](architecture/webgpu.md), [audio](architecture/audio.md),
  [race](architecture/race.md), [web](architecture/web.md).
- **[`DEVELOPER_HANDBOOK.md`](DEVELOPER_HANDBOOK.md)** — what exists and what
  demonstrates it, the dominant bug class, the testing rules, decomp sync, and
  the practices this codebase punishes people for skipping.
- **[`COLOUR_LIGHTING_PIPELINE.md`](COLOUR_LIGHTING_PIPELINE.md)** — the
  end-to-end colour-space policy and the directional-lighting contract across
  GL, WebGPU and the browser.
- **[`asset_swap_notes.md`](asset_swap_notes.md)** — per-asset-type endianness
  and LP64 layout coverage: what is normalised, what is deliberately punted, and
  the open questions. Cited directly from `platform/asset_swap.c`.

## Subsystem guides

- **[`SAVE_MANAGEMENT.md`](SAVE_MANAGEMENT.md)** — backup, import, inspection,
  editing and recovery of progress, in the browser and from the native CLI.
- **[`VIRTUAL_CONTROLLER_PAK.md`](VIRTUAL_CONTROLLER_PAK.md)** — the four virtual
  Controller Paks, ghost custody, and how they are checksummed.
- **[`ORACLE.md`](ORACLE.md)** — pixel comparison of the port against the real
  ROM in a patched emulator. **It includes the ways this harness has lied**; read
  that section before trusting any fidelity score.
- **[`DECOMP_SYNC.md`](DECOMP_SYNC.md)** — the three-way merge for pulling
  upstream decompilation changes, and the validation required afterwards.
- **[`../tests/README.md`](../tests/README.md)** — every regression check, its
  frame budget, its expected terminal state, and the input-script format.

## Open items and status

- **[`open-items/`](open-items/README.md)** — every known and fixed defect, by
  subsystem, each with its mechanism, the measurement that found it, the fix, and
  how it was verified. **Nothing is deleted when it is fixed**: read the entry for
  a subsystem before changing it, because most of these defects were silent.
  ([`OPEN_ITEMS.md`](OPEN_ITEMS.md) is the pointer left behind by the split.)
- **[`../ROADMAP.md`](../ROADMAP.md)** — what is deferred and why, with the
  condition under which each item would be taken up.
- **[`STATUS.md`](STATUS.md)** — the long-form per-milestone engineering log.
  History, not a current-state summary; for current state use
  [`../README.md`](../README.md) and `open-items/`.
- **[`../CHANGELOG.md`](../CHANGELOG.md)** — release by release, limited to what
  the regression suite actually demonstrates.

## Project and provenance

- **[`RELEASE_CHECKLIST.md`](RELEASE_CHECKLIST.md)** — the repeatable
  pre-release gate: clean-room verification, build, tests, ROM-absence guard.
- **[`DEMO_REPO.md`](DEMO_REPO.md)** — how the playable web demo is published,
  and why it is a publication target that is never hand-edited.
- **[`MGB64_BACKFLOW.md`](MGB64_BACKFLOW.md)** — findings sent back to mgb64, the
  sibling GoldenEye port that shares parts of `platform/`. Relevant if you touch
  shared renderer code.
- **[`PHASE3_SCOPE.md`](PHASE3_SCOPE.md)** — the Phase 3 work items with
  per-item status, **including the claims that were retracted**.
- **[`ref/`](ref/dkr_asset_spec.md)** — the linker script, the DKR asset
  specification and the asset-layout headers that `platform/asset_swap.c` is
  written against. Derived from the upstream decompilation project's tooling, not
  first-party; see [`../NOTICE.md`](../NOTICE.md).

This repository is the public source of truth and keeps ordinary Git history.
Temporary plans, handoffs, transcripts, personal filesystem paths, and similar
working material belong outside tracked paths. Durable decisions go into the
architecture or open-items documents above, with measured evidence and a
regression check. `python3 tools/check_public_surface.py --staged` enforces this
boundary before publication.

## House style

Every write-up here follows the same shape, and additions should too:

> **mechanism → measured evidence → fix → verification**

State what the code actually does, show the measurement that proves it, describe
the fix at true cause, and name the check that would catch a regression. No claim
without evidence; a retraction is recorded rather than quietly deleted.
