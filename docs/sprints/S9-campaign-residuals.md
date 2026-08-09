# S9 — Campaign residuals

> **Executing this plan:** work the tasks in order, one `- [ ]` step at a time.
> Tick a step only once its command has been run and its output read. The
> run-it-and-watch-it-fail steps are load-bearing; skipping ahead to the
> implementation forfeits the positive control that makes the gate mean anything.

**Goal:** Close the three named residuals behind `check_campaign_progression`, so
that the start-to-credits claim rests on driven routes rather than on retargets
and premises — and reconcile `ROADMAP.md` with what the closure ledger already
records.

**Architecture:** Nothing here is a suspected defect. Each residual is a
**headless-driving obstacle** with a measured mechanism already recorded in
`tests/fixtures/README.md` §"Residual manual acceptance". The work is to build
the driving capability each one needs, then chain the existing seams through it.
Two of the three need a route-planning improvement; one needs the cutscene stack
to be advanceable headlessly.

**Tech stack:** The existing Python check harness, `tests/input_scripts/`, the
`MDKR_LOAD_TRACK` retarget hook, `tools/oracle_routes/`, and the drive hook in
the race harness.

**Size:** Small relative to the other sprints. It is second in the recommended
order because it is cheap and it retires the last "is the game actually
finishable" question.

## Global constraints

- Every game or test invocation passes `--headless-frames N` and sets
  `MDKR_AUDIO=0`.
- **Do not weaken an assertion to make a route pass.** The existing gate says in
  its own docstring that none of these residuals "is weakened into a vacuous
  assertion in this file". A green suite can mean the test was lowered to fit the
  problem; diff the test alongside the fix in review.
- No ROM-derived data in any commit. Every EEPROM image is built in a private
  temporary directory and deleted, exactly as the existing check does.
- Game-code changes go behind `#ifdef NATIVE_PORT` with a `_Static_assert`
  wherever a layout or offset assumption is involved. Prefer harness changes:
  two of these three should need no `game/` edit at all.
- Run `python3 tools/check_public_surface.py --staged` before every commit.

---

## User stories

**US-1 — Finish the game.** As a player, the whole campaign from first race to
credits is proven to work, so that I am not the one who discovers the last door
does not open.

**US-2 — Reach the ending I earned.** As a player who beats Wizpig 2, I see the
true ending, so that the campaign has a conclusion rather than a state bit.

**US-3 — Unlock Future Fun Land.** As a player who wins every trophy, the
lighthouse opens, so that the final world is reachable by playing.

**US-4 — Read an accurate roadmap.** As a contributor choosing work, the roadmap
matches the closure ledger, so that I do not start on something finished eight
days ago.

---

## Milestones and acceptance criteria

### M1 — Roadmap reconciliation

**Done when:** `ROADMAP.md`'s "Campaign completeness" and "Mode-coverage
stragglers" entries agree with `docs/DEFINITION_OF_DONE.md` and with
`tests/fixtures/README.md` §"Residual manual acceptance" — the same three
residuals, described the same way, with no stale claim that the campaign is
ungated or that ghost coverage is one pair of 47.

### M2 — The lobby rematch door, driven

**Done when:** the boss-rematch entry in seam B is reached by driving through the
lobby door from the post-race spawn, not by `MDKR_LOAD_TRACK` retarget; the
retarget path stays in the file as the fast arm; and the driven arm asserts the
same seam-B state the retarget arm does.

### M3 — Trophy and T.T.-amulet chaining, and Future Fun Land

**Done when:** `check_trophy_series`'s four championships and the four T.T.
challenge levels are chained into `check_campaign_progression` as produced state
rather than stated premises, and the Future Fun Land unlock
(`trophies & 0xFF == 0xFF` plus Wizpig 1) is witnessed on a save the chain
produced.

### M4 — The credits screen, reached

**Done when:** a won Wizpig 2 advances through the cutscene stack to
`MENU_CREDITS` headlessly, the screen is asserted to be the true-ending variant
("TO BE CONTINUED …" plus `SEQUENCE_CRESCENT_ISLAND`), and the WHODIDTHIS cheat
contrast arm still reaches credits without `bosses & 0x20` so the two endings
remain distinguished.

### M5 — One continuous claim

**Done when:** the gate's docstring's "What this deliberately does NOT prove"
section is reduced to whatever genuinely remains, and
`docs/DEFINITION_OF_DONE.md`'s campaign row names the residual count accurately —
zero if all three close.

---

## File structure

**Create:**

| Path | Responsibility |
|---|---|
| `tests/route_plan.py` | Shared waypoint-following drive helper. |
| `tests/input_scripts/lobby_rematch_door.txt` | The driven lobby approach. |
| `tests/check_future_fun_land.py` | The trophy → lighthouse unlock gate. |

**Modify:** `tests/check_campaign_progression.py`,
`tests/check_trophy_series.py`, `tests/fixtures/README.md`,
`tests/input_scripts/wizpig_to_credits.txt`, `tools/run_checks.py`,
`ROADMAP.md`, `docs/DEFINITION_OF_DONE.md`, `docs/open-items/gameplay.md`.

---

## Task 1: Reconcile the roadmap with the closure ledger

**Files:** `ROADMAP.md`, `docs/open-items/gameplay.md`

Do this first. It costs an hour and it stops anyone else starting on finished
work.

- [ ] **Step 1: Read all three sources and list the disagreements.**

```bash
grep -n "Campaign completeness" -A 12 ROADMAP.md
grep -n "Mode-coverage stragglers" -A 10 ROADMAP.md
grep -n "campaign completes" -A 4 docs/DEFINITION_OF_DONE.md
grep -n "Ghosts round-trip" -A 4 docs/DEFINITION_OF_DONE.md
```

Expected disagreements, to be confirmed rather than assumed:
- `ROADMAP.md` calls campaign completeness "the largest single piece of deferred
  work"; the ledger records `check_campaign_progression` gating silver coins, all
  four rematches, both Wizpig races and the true-ending bit.
- `ROADMAP.md` says ghost save and load is "gated for one (track, vehicle) pair
  rather than across the set"; the ledger records `check_ghost_matrix` at 46 of
  47 with the 47th an asserted non-producer.

- [ ] **Step 2: Rewrite the two `ROADMAP.md` entries** to describe the three
  actual residuals, each with its measured obstacle, linking
  `tests/fixtures/README.md` §"Residual manual acceptance". Keep the roadmap's
  established voice: what is not done, why it was deferred, and what would have
  to be true to take it up.

- [ ] **Step 3: Update `docs/open-items/gameplay.md`'s ghost-coverage entry** to
  match the ledger.

- [ ] **Step 4: Run the link checker.**

```bash
python3 tools/check_markdown_links.py
```

- [ ] **Step 5: Commit.**

```bash
git add ROADMAP.md docs/open-items/gameplay.md
python3 tools/check_public_surface.py --staged
git commit -m "docs: reconcile the roadmap with the 1.1.0 closure ledger"
```

---

## Task 2: A waypoint-following drive helper

**Files:**
- Create: `tests/route_plan.py`

**The measured obstacle, stated so the fix targets it:** in the Dino Domain lobby
with all door bits open, the kart stalls at `(-1295, 685)`, 1,240 units short of
the boss exit at `(-777, 1812)`, and the drive hook's reverse-and-retry
oscillates there indefinitely. The exit *is* reachable from the spawn the player
returns to after a race. So the problem is not the gate and not the geometry —
it is that the drive hook has no route memory and re-attempts the same blocked
heading forever.

- [ ] **Step 1: Reproduce the stall and record it.** Drive the existing lobby
  route headlessly and capture the position trace. Paste the oscillation. Do not
  proceed on the recorded description alone — confirm it still reproduces at
  this commit.

- [ ] **Step 2: Write the helper.** `tests/route_plan.py` takes an ordered list
  of waypoints and drives to each in turn, with:
  - a per-waypoint arrival radius;
  - a per-waypoint budget in frames, after which the route **fails loudly**
    naming the waypoint and the distance remaining — never silently continues;
  - an oscillation detector: if the position revisits a small neighbourhood N
    times, fail with the neighbourhood and the count rather than looping.

  The loud failures matter more than the driving. The current hook's defect is
  that it oscillates indefinitely instead of reporting.

- [ ] **Step 3: Prove the helper fails correctly** by giving it an unreachable
  waypoint and asserting it reports within its budget.

- [ ] **Step 4: Commit.**

---

## Task 3: Drive the lobby rematch door

**Files:**
- Create: `tests/input_scripts/lobby_rematch_door.txt`
- Modify: `tests/check_campaign_progression.py`, `tests/fixtures/README.md`

- [ ] **Step 1: Add the driven arm as a failing test first.** Add seam B's
  `driven` variant to `check_campaign_progression.py`, asserting the identical
  post-seam state the retarget arm asserts. Run it; it must fail at the stall.

- [ ] **Step 2: Start from the post-race spawn.** The recorded note says the exit
  is reachable from the spawn the player returns to after a race, which is why
  `check_first_boss_progression`'s route reaches the first boss as `12:E7:E38`.
  Compose from that state rather than from the lobby entry spawn.

- [ ] **Step 3: Find the waypoints empirically.** Capture a position trace of a
  successful manual approach, or bisect the space between `(-1295, 685)` and
  `(-777, 1812)` with the Task 2 helper, adding intermediate waypoints until the
  route completes. Record the final waypoint list and how it was derived in the
  input script's header comment.

- [ ] **Step 4: Keep both arms.** The retarget arm stays as the fast path; the
  driven arm is the completeness proof. If the driven arm is slow, mark it so in
  the check's frame budget in `tests/README.md`, but do not delete it.

- [ ] **Step 5: Run the gate and the neighbours.**

```bash
MDKR_AUDIO=0 python3 tests/check_campaign_progression.py
MDKR_AUDIO=0 python3 tests/check_first_boss_progression.py
MDKR_AUDIO=0 python3 tests/check_bluey2_rematch.py
MDKR_AUDIO=0 python3 tests/check_door_blocks.py
```

- [ ] **Step 6: Update `tests/fixtures/README.md` §1** — replace the residual
  with what the driven arm now proves, keeping the measured stall description as
  the record of why the route is shaped the way it is.

- [ ] **Step 7: Commit.**

---

## Task 4: Chain trophies and the T.T. amulet

**Files:**
- Create: `tests/check_future_fun_land.py`
- Modify: `tests/check_campaign_progression.py`, `tests/check_trophy_series.py`,
  `tests/fixtures/README.md`, `tools/run_checks.py`

`ttAmulet` is written by the four T.T. challenge levels
(`game/src/objects.c:9256-9268`) and `trophies` by the trophy-race rankings
screen. `check_trophy_series.py` already drives all four championships and their
EEPROM persistence — it is simply not chained in, so fixture E states both as
premises.

- [ ] **Step 1: Extract the trophy drive into a reusable fixture.** Move the
  championship-driving body of `check_trophy_series.py` into a function the
  campaign check can import, leaving `check_trophy_series.py` a thin caller so
  its own gate is unchanged. Run it to confirm it still passes identically.

- [ ] **Step 2: Add the T.T. challenge levels as a new seam.** Drive all four,
  assert `ttAmulet` climbing 1, 2, 3, 4 across EEPROM round trips in separate
  processes, exactly as seam B does for `wizpigAmulet`.

- [ ] **Step 3: Add the negative control.** The same challenge from a save that
  has not earned the prerequisite must **not** award a piece. Without this, the
  seam proves only that the number goes up.

- [ ] **Step 4: Write `tests/check_future_fun_land.py`.** On a save the chain
  produced, assert `trophies & 0xFF == 0xFF` together with the Wizpig 1 bit
  produces the lighthouse unlock at `game/src/thread3_main.c:1895-1899`, and that
  one trophy short does not. Both arms, or the gate proves nothing.

- [ ] **Step 5: Replace fixture E's premises with produced state.** Fixture E
  currently states `trophies` and `ttAmulet` as premises; derive them from the
  new seams, and extend the existing `derive_*` legitimacy derivation to cover
  them the way the other fixtures are covered.

- [ ] **Step 6: Run the chain end to end.**

```bash
MDKR_AUDIO=0 python3 tests/check_trophy_series.py
MDKR_AUDIO=0 python3 tests/check_future_fun_land.py
MDKR_AUDIO=0 python3 tests/check_campaign_progression.py
```

- [ ] **Step 7: Update `tests/fixtures/README.md` §2**, register the new check in
  `CHECKS`, and commit.

---

## Task 5: Reach the credits screen

**Files:** `tests/check_campaign_progression.py`,
`tests/input_scripts/wizpig_to_credits.txt`, `tests/fixtures/README.md`

**The measured obstacle:** after the Wizpig 2 win the game pushes a stack of
cutscene levels and pops them on either an A press with `func_8006C300()`
non-zero or the scene ending itself (`game/src/thread3_main.c:894-919`).
Measured, the run reaches `ASSET_LEVEL_WIZPIG2ANIM` (level 62) and stays there
for 25,000 further frames with A tapped every 200 frames, so the animation does
not run itself out headlessly.

- [ ] **Step 1: Reproduce and instrument.** Run
  `tests/input_scripts/wizpig_to_credits.txt` and log, per frame, the current
  level, the cutscene stack depth, and `func_8006C300()`'s return. The question
  to answer is precise: **is `func_8006C300()` ever non-zero during those 25,000
  frames?** If it is never non-zero, the A presses can never pop, and the tapping
  cadence is irrelevant.

- [ ] **Step 2: Follow the mechanism, not the symptom.** Depending on Step 1:
  - **If `func_8006C300()` is never non-zero:** find what makes it non-zero and
    whether that condition is reachable headlessly. This is likely a
    presentation-driven condition, which is the actual root cause.
  - **If it is non-zero but the press is not consumed:** the A press is not
    reaching the consumer; compare against the overlay input handoff work, which
    fixed a structurally similar defect.
  - **If it pops but the next level re-pushes:** the stack is not draining;
    log the push sites.

  Write the mechanism down before changing anything.

- [ ] **Step 3: Prefer a harness fix.** If the animation simply needs
  presentation to advance, drive it with the existing headless presentation path
  rather than adding a skip. A `game/` edit that skips the animation would make
  the gate prove less than it appears to.

- [ ] **Step 4: Assert the ending variant, not just the screen.** On reaching
  `MENU_CREDITS`, assert `menu_credits_init` (`game/src/menu.c:15188-15201`)
  selected "TO BE CONTINUED …" and `SEQUENCE_CRESCENT_ISLAND`, and that
  `gViewingCreditsFromCheat` is zero.

- [ ] **Step 5: Keep the contrast arm.** The WHODIDTHIS route reaching
  `MENU_CREDITS` from a never-started save is what makes "reached credits"
  meaningful; it must still pass and still produce the "THE END" variant.

- [ ] **Step 6: If Step 2 finds a real defect, treat it as one.** Root cause,
  positive control, and a write-up in `docs/open-items/gameplay.md` with
  mechanism → measured evidence → fix → verification. Then sweep the class: if
  the cutscene stack does not drain here, ask where else a stack of pushed levels
  is popped by the same predicate, and check every one.

- [ ] **Step 7: Run the gate and the cutscene neighbours.**

```bash
MDKR_AUDIO=0 python3 tests/check_campaign_progression.py
MDKR_AUDIO=0 python3 tests/check_overlay_pause_cutscene.py
MDKR_AUDIO=0 python3 tests/check_boss_win_verdict.py
```

- [ ] **Step 8: Update `tests/fixtures/README.md` §3 and commit.**

---

## Task 6: Close the claim

**Files:** `tests/check_campaign_progression.py`, `tests/fixtures/README.md`,
`docs/DEFINITION_OF_DONE.md`, `ROADMAP.md`

- [ ] **Step 1: Reduce the gate's "What this deliberately does NOT prove"
  section** to whatever genuinely remains. If a residual did not close, it stays,
  with its measured obstacle updated to what was learned. Do not remove a
  residual that was not closed.

- [ ] **Step 2: Update the campaign row in `docs/DEFINITION_OF_DONE.md`** with
  the accurate residual count and what each remaining one is.

- [ ] **Step 3: Update `ROADMAP.md`** again if Tasks 3–5 changed the picture from
  Task 1's reconciliation.

- [ ] **Step 4: Run the complete suite, sequentially.**

```bash
rm -f save/eeprom.bin
MDKR_AUDIO=0 python3 tools/run_checks.py
```

- [ ] **Step 5: Diff the tests as well as the code.** Before declaring this
  sprint done, review the diff of every touched `tests/` file specifically for
  lowered assertions — a threshold widened, an arm removed, a strict comparison
  loosened. A green suite that was made green by the test moving is the failure
  mode this sprint is most exposed to.

- [ ] **Step 6: Paste the suite summary into the final commit body.**

---

## Self-review

**Spec coverage.** M1 → Task 1. M2 → Tasks 2–3. M3 → Task 4. M4 → Task 5.
M5 → Task 6. US-4 is served entirely by Task 1, which is why it is first.

**Deliberately out of scope:**

- **A single continuous start-to-credits run.** The existing check's own
  reasoning holds: it would run for hours and fail as one opaque blob. Composing
  witnessed seams is the better design and this sprint keeps it.
- **The 47th ghost (track, vehicle) pair.** It is an asserted autopilot
  non-producer, not a skip. Nothing to close.
- **Expanding magic-code coverage beyond the three gated codes.** The remaining
  codes share one decrypt, normalize, validate and apply path whose actual
  failure mode was an endianness defect in the shared table decode, which any
  single code exercises. The existing deferral rationale stands.

**Type consistency.** `tests/route_plan.py` is introduced in Task 2 and consumed
in Task 3. The trophy fixture extracted in Task 4 Step 1 is consumed by
`check_campaign_progression.py` in Task 4 Step 5. No C interfaces are introduced.
