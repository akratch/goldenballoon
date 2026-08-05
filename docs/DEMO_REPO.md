# The two-repo split: source → demo

## Why

The source and the live playable demo live in two different repositories, and
they stay separate even though both are now public.

The demo repo is a deployment target for GitHub Pages: its entire contents are
machine-generated build output, replaced wholesale on every publish. Mixing that
into a source repository means every publish rewrites tracked files nobody
authored, and the Pages site's history becomes indistinguishable from the
project's.

| | source repo (this one) | demo repo |
|---|---|---|
| purpose | the project | a Pages deployment target |
| contains | everything — decomp, engine, tests, oracle, docs | built artifacts only |
| game code | yes (decompiled) | **none** |
| ROM / assets | none (bring-your-own) | **none** |
| edited by | humans | **nobody — generated** |

> **Note on history.** The source repository keeps ordinary public Git history.
> The demo repository remains a separate, generated deployment target; this
> document uses “source repo” only to distinguish authored source from that
> generated site payload.

## The contract

**1. One direction, always.** The demo repo is a publication *target*. Its payload is
replaced wholesale on every publish, so anything edited there is silently lost. Even
its `README.md`, `DISCLAIMER.md` and `LICENSE` are version-controlled *here*, in
`tools/web/demo-repo/`, and re-synced every time — there is exactly one source of
truth, and the public repo cannot drift by someone editing it.

**2. Every publish is provenance-stamped.** `tools/web/build_web.sh` writes
`dist/web/build-info.json`:

```json
{ "source_commit": "…", "source_dirty": false, "built_utc": "…",
  "emcc": "…", "wasm_bytes": 1444256 }
```

With no source in the demo repo, that stamp is the only link from the live site back
to code, so the publisher repeats it in the demo repo's commit message too. "What
code is live?" always has an exact answer.

Staging clears the previous link's output before copying this one in, so clearing and
stamping are one indivisible step: every gate that can refuse the build is decided
before `dist/web` is touched, and an I/O failure inside the staging block removes the
whole staged set rather than leaving artifacts with no provenance to describe them. A
refused build therefore leaves either the previous complete build or the tracked
sources alone — never a half-built `dist/web` that later tools would read as real.

**3. Fail closed.** `tools/web/publish_demo.sh` aborts rather than shipping something
questionable. In order:

- **dirty source tree** → refused (a published build must correspond to a commit, or
  nobody can say what is live). Override is explicit: `--allow-dirty`.
- **build** runs `build_web.sh`, which enforces the 40 MiB wasm budget and runs
  `tools/check_no_rom.sh`. With `--no-build` the ROM guard still runs over the
  staged `dist/web`, and the staged `build-info.json` must name `HEAD` — reusing a
  directory built from another commit would make the footer, the stamp and the
  commit message assert something untrue about what is live.
- **demo dir sanity** → refused if it is the source repo, is *nested* inside it, or
  is not a git repo.
- **source leakage** → refused if the demo repo contains `game/`, `platform/` or a
  ROM. The demo repo is supposed to hold build output and nothing else; source
  appearing in it means the publisher copied the wrong tree, so this stops the
  run instead of warning.
- **stamping** → `tools/web/stamp_publish.sh` rewrites the staged `index.html`,
  adding `?v=<short commit>` to every locally referenced shell asset and
  `data-build-version=<MDKR_VERSION>` to `<html>`, then re-greps for each
  reference and exits non-zero if one is missing. A half-stamped page looks fine
  and a sticky cache pins it forever, so a missed reference stops the publish.
  The same script is the Actions publish path's only stamper, so the two cannot
  drift; `mdkr64-shell.js` recovers the stamp from its own script URL and
  propagates it to the engine, the save-tools module and the service worker, so
  a stamped page cannot serve a mixed wasm/JS pair.

## Keeping them in sync

The failure mode this split invites is that the live site quietly rots: nothing
breaks, it is just old, and nobody notices. The ratchet against that:

```bash
tools/web/check_demo_sync.sh --demo ../golden-balloon-demo
```

It reads the stamp, compares to `HEAD`, and distinguishes *stale* from *behind*:

- exactly `HEAD` → **PASS**
- behind, but no intervening commit touched `platform/`, `game/`, `lib/`, `cmake/`,
  `CMakeLists.txt`, `dist/web/` or `tools/web/` → **PASS**, because a republish
  would produce a byte-identical site
- behind by commits that *do* touch the build → **exit 1**, listing the files

It also fails if the live build was made from a dirty tree, or if the published
commit is not in this repo at all (history rewritten, or published from another
clone).

Run it before saying "the demo is up to date", and after any renderer, engine or
shell change.

## Publishing

```bash
# 1. land your work here first — the publisher refuses a dirty tree
git status

# 2. publish (builds, guards, stamps, commits in the demo repo)
tools/web/publish_demo.sh --demo ../golden-balloon-demo

# 3. review what landed, then push
cd ../golden-balloon-demo && git status && git push
```

Add `--push` to step 2 once you trust it. Pages then deploys from the demo repo's
default branch; `.nojekyll` is written so Pages does not run Jekyll over the
wasm/js payload.

## First-time setup of the demo repo

```bash
mkdir -p ../golden-balloon-demo && cd ../golden-balloon-demo
git init -b main
git remote add origin git@github.com:<you>/golden-balloon.git   # the PUBLIC repo name
# (the local directory is golden-balloon-demo; the remote is what the URL is built from)
```

Then publish once, push, and enable Pages on that repo (branch `main`, folder `/`).
Update the live URL at the top of `tools/web/demo-repo/README.md` — remembering that
it is edited *here*, not there.

## If you would rather not run a second repo

Cloudflare Pages, Netlify and Vercel can all deploy a static directory straight
from a repository on their free tiers, public or private. In that case
`dist/web` is the publish directory and this whole split is unnecessary — keep
`build-info.json` and the ROM guard, drop `publish_demo.sh`.
