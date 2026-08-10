# S7 — Documentation site and community surface

> **Executing this plan:** work the tasks in order, one `- [ ]` step at a time.
> Tick a step only once its command has been run and its output read. The
> run-it-and-watch-it-fail steps are load-bearing; skipping ahead to the
> implementation forfeits the positive control that makes the gate mean anything.

**Goal:** Give the project a front door — a published site that explains what it
is, how to play it, and how to make things for it — generated from the
documentation already in this repository so it cannot drift.

**Architecture:** No new documentation system and no hand-maintained second copy
of anything. A static generator renders the existing `docs/` tree plus a small
number of new landing pages to the same GitHub Pages deployment the browser build
already uses, so the site and the playable build ship together and from one
source. The link checker that already guards `docs/` guards the site by
construction.

**Tech stack:** A static site generator over the existing Markdown (MkDocs
Material or equivalent), GitHub Actions, the existing `web-demo.yml` Pages
deployment, `tools/check_markdown_links.py`.

**Depends on:** [S1](S1-content-pipeline.md) — the modding guides describe the
pack format S1 defines. The site scaffolding (Tasks 1–3) can land first; the
modding section (Task 4) cannot.

## Global constraints

- **One source.** Nothing on the site may be authored outside `docs/` and the
  landing pages this sprint creates. A prose duplicate of `README.md` on a
  marketing page is exactly how the two start disagreeing.
- **No game content, no ROM links, no piracy signposting.** Every page inherits
  the position in `DISCLAIMER.md`. The site must never link to a ROM source, and
  must state that players supply their own.
- Player-facing pages carry no process, gate, or validation vocabulary.
- The site build runs `python3 tools/check_markdown_links.py` and fails on a
  broken link.
- Run `python3 tools/check_public_surface.py --staged` before every commit —
  including on site source, which is the likeliest place a personal path leaks.

---

## User stories

**US-1 — Understand it in thirty seconds.** As someone sent a link, I land on a
page that says what this is, shows it running, and tells me what I need, so that
I can decide whether to try it.

**US-2 — Get it running.** As a new player, I follow one page per platform and
end up playing, so that I do not have to read a repository README.

**US-3 — Make a texture pack.** As an artist with no C experience, I follow a
tutorial from a running game to a finished pack, so that I can contribute art.

**US-4 — Find the answer.** As anyone, I search the documentation and find the
page, so that I do not open a duplicate issue.

**US-5 — Report something useful.** As a player hitting a bug, the issue form
asks me for exactly what is needed, so that my report is actionable first time.

**US-6 — Find where to talk.** As an interested person, I can see where
discussion happens, so that I am not shouting into an issue tracker.

---

## Milestones and acceptance criteria

### M1 — The site builds from `docs/`

**Done when:** a generator renders every existing page under `docs/` with working
navigation and search, the build fails on a broken internal link, and the output
is byte-reproducible for an unchanged input.

### M2 — Landing and platform pages

**Done when:** a landing page and one page per platform exist; every claim on
them is traceable to `README.md` or `docs/`; the download table is generated from
the release metadata rather than typed; and the no-content position is stated on
the landing page, not buried.

### M3 — Published

**Done when:** the site deploys to the existing Pages target alongside the
browser build, both survive the same deployment, and the browser build's URL is
unchanged. A deployment that moves the playable build's URL is a regression.

### M4 — Modding guides

**Done when:** three tutorials exist and have each been executed end-to-end by
someone following only the written steps — a texture pack, a music replacement,
and reading the diagnostics window for a bug report — with the exact commands and
expected output at every step.

### M5 — Contribution funnel

**Done when:** issue forms collect the diagnostics block, ROM revision, renderer,
platform and reproduction; the PR template names the CONTRIBUTING rules that most
often go missing; and a public discussion venue is linked from the site,
`README.md`, and the issue forms.

---

## File structure

**Create:**

| Path | Responsibility |
|---|---|
| `site/mkdocs.yml` | Generator configuration and navigation. |
| `site/index.md` | Landing page. |
| `site/play/windows.md`, `play/macos.md`, `play/linux.md`, `play/browser.md` | Per-platform getting started. |
| `site/make/texture-pack.md` | Tutorial: first texture pack. |
| `site/make/music.md` | Tutorial: replacing a track. |
| `site/make/report-a-bug.md` | Tutorial: producing a useful report. |
| `site/overrides/` | Theme overrides and brand assets. |
| `.github/workflows/site.yml` | Build and deploy. |
| `tests/check_site_build.py` | Build, link, and content-absence gate. |

**Modify:** `.github/workflows/web-demo.yml` (shared deployment),
`.github/ISSUE_TEMPLATE/`, `.github/PULL_REQUEST_TEMPLATE.md`, `README.md`,
`docs/README.md`, `tools/run_checks.py`, `brand/`.

---

## Task 1: Generator scaffolding

**Files:**
- Create: `site/mkdocs.yml`, `tests/check_site_build.py`
- Modify: `tools/run_checks.py`, `.gitignore`

- [ ] **Step 1: Write the failing gate.** `tests/check_site_build.py` asserts:
  - the site builds with a zero exit;
  - the build fails when a deliberately broken internal link is introduced —
    a positive control, run in a scratch copy so the tree is untouched;
  - the built output contains no `.z64`, `.v64`, `.n64`, no file over 8 MiB that
    is not a known brand asset, and no string matching the ROM-content patterns
    `tools/check_no_rom.sh` looks for;
  - the built output contains no absolute path beginning with `/Users/`,
    `/home/`, or a Windows drive letter;
  - two consecutive builds of the same input produce identical file lists and
    hashes.

- [ ] **Step 2: Run it, verify it fails.**

- [ ] **Step 3: Add the generator configuration.** Point `docs_dir` at the
  existing `docs/` tree plus the new `site/` pages. Do not copy or move any
  existing document.

- [ ] **Step 4: Run the gate. Verify it passes.**

- [ ] **Step 5: Register in `CHECKS` and commit.**

---

## Task 2: Landing and platform pages

**Files:**
- Create: `site/index.md`, `site/play/*.md`
- Modify: `README.md`, `brand/`

- [ ] **Step 1: Write the landing page.** What it is, one screenshot or short
  clip, the one-line requirement that players supply their own game, the four
  platforms, and a link to the browser build. Player-facing language throughout —
  read `RELEASE_NOTES.md`'s 1.1.0 text for the register this project uses and
  match it.

- [ ] **Step 2: State the position plainly on the landing page** — no ROM,
  textures, audio, music, models or level data are distributed, and the project
  does not condone piracy. Link `DISCLAIMER.md` and `NOTICE.md`. This is not
  small print; put it where it is read.

- [ ] **Step 3: Generate the download table** from the latest release's assets at
  build time rather than typing filenames that will be wrong within a release.
  Where a platform's artifact is absent because its publish gate did not pass,
  the table must say so rather than omitting the row silently.

- [ ] **Step 4: Write one page per platform** covering download, first-open
  (including the macOS unsigned-build note if it still applies after
  [S6](S6-release-engineering.md)), ROM intake, and controls.

- [ ] **Step 5: Trace every claim.** For each factual statement on these pages,
  confirm the corresponding line in `README.md` or `docs/`. Fix the repository
  document if they disagree; do not let the site become the newer truth.

- [ ] **Step 6: Run the gate and the link checker.**

```bash
python3 tests/check_site_build.py
python3 tools/check_markdown_links.py
```

- [ ] **Step 7: Commit.**

---

## Task 3: Deployment

**Files:**
- Create: `.github/workflows/site.yml`
- Modify: `.github/workflows/web-demo.yml`

- [ ] **Step 1: Establish the URL contract first.** Record, in
  `.github/workflows/site.yml`'s header comment, the exact path the playable
  browser build occupies today and the path the site will occupy. The playable
  build's URL must not move.

- [ ] **Step 2: Deploy both from one job or one artifact.** Two workflows racing
  to publish to the same Pages target will intermittently delete each other's
  output. Either merge the deploy step into `web-demo.yml` or make `site.yml`
  build into a subdirectory of the same artifact.

- [ ] **Step 3: Verify after deployment** that the browser build still loads at
  its original URL and the site loads at its new one. Record both.

- [ ] **Step 4: Commit.**

---

## Task 4: Modding tutorials

**Files:** `site/make/texture-pack.md`, `site/make/music.md`,
`site/make/report-a-bug.md`

**Depends on [S1](S1-content-pipeline.md).** Do not write these against a
speculative format.

- [ ] **Step 1: Write the texture-pack tutorial** as a literal transcript: run
  `tools/mod_texture_dump.py`, find a digest, open the PNG, edit it, create
  `pack.ini` with every key shown in full, place it, launch, press `Tab` to
  compare. Every command and its expected output.

- [ ] **Step 2: Execute the tutorial from a clean checkout**, following only the
  written steps, and fix every place the text assumed knowledge. A tutorial that
  has not been executed is a draft.

- [ ] **Step 3: Write the music tutorial** the same way, including the sequence
  id lookup and the WAV format requirements.

- [ ] **Step 4: Write the bug-report tutorial** — open the diagnostics window
  ([S3](S3-in-app-dev-tools.md)), copy the block, attach it to the issue form.
  If S3 has not landed, write it against the current `--headless-frames` +
  `[CRASH]` output route instead and mark the diagnostics route as pending.

- [ ] **Step 5: Link all three from `docs/MODDING.md`** and from the site
  navigation.

- [ ] **Step 6: Run the gate and the link checker. Commit.**

---

## Task 5: Contribution funnel

**Files:** `.github/ISSUE_TEMPLATE/`, `.github/PULL_REQUEST_TEMPLATE.md`,
`README.md`, `site/index.md`

- [ ] **Step 1: Read the existing templates** in `.github/ISSUE_TEMPLATE/` before
  changing them — this repository already has them, and the task is to sharpen,
  not replace.

- [ ] **Step 2: Add required fields to the bug form**: platform, app version, ROM
  revision, renderer (`MDKR_RENDERER`), whether any content packs are installed,
  reproduction steps, and the diagnostics block. Make the diagnostics block a
  required field with a placeholder showing its shape.

- [ ] **Step 3: Add a content-pack issue form** routing pack problems away from
  engine bugs, since S1 makes those a new and distinct category.

- [ ] **Step 4: Sharpen the PR template** around the three rules that go missing
  most often: the positive control, the class sweep, and the pasted command
  output. One checkbox each, each linking the relevant `CONTRIBUTING.md` section.

- [ ] **Step 5: Add a discussion venue** — GitHub Discussions is the lowest-cost
  option and needs no new infrastructure. Link it from the site landing page,
  `README.md`, and the issue forms' "before you file" text.

- [ ] **Step 6: Run the link checker. Commit.**

---

## Task 6: Full verification

- [ ] **Step 1: Run the site gate, the link checker, and the guards.**

```bash
python3 tests/check_site_build.py
python3 tools/check_markdown_links.py
bash tools/check_no_rom.sh
bash tools/check_clean_room.sh
python3 tools/check_public_surface.py
```

- [ ] **Step 2: Run the complete suite, sequentially, and paste the summary.**

```bash
rm -f save/eeprom.bin
MDKR_AUDIO=0 python3 tools/run_checks.py
```

- [ ] **Step 3: Confirm the playable build's URL is unchanged.** Load it and
  paste the URL and result.

---

## Self-review

**Spec coverage.** M1 → Task 1. M2 → Task 2. M3 → Task 3. M4 → Task 4.
M5 → Task 5. US-4's search comes from the generator in Task 1; US-6 from Task 5
Step 5.

**Deliberately out of scope:**

- **A custom domain.** No benefit until the site exists and is used.
- **A mod-hosting service.** Hosting user content is an ongoing moderation and
  legal commitment. Linking an existing community host, if one emerges, costs
  nothing and commits nothing.
- **Doxygen API reference.** The Harbour Masters ports publish one because they
  expose a C mod API. This project does not have one, so an API reference would
  document internals as though they were a contract.
- **Translated documentation.** Worth doing once the English pages are stable.

**Type consistency.** No code interfaces are introduced by this sprint. The two
cross-sprint dependencies are `tools/mod_texture_dump.py` and `pack.ini` from
[S1](S1-content-pipeline.md), and the diagnostics copy block from
[S3](S3-in-app-dev-tools.md); Task 4 Step 4 states the fallback when S3 is absent.
