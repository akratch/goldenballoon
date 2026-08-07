# Definition of Done — the 1.1.0 closure ledger

*A point-in-time record, written 2026-08-07 at the close of the "definitionally
done" campaign. Each row states a claim a skeptical player or reviewer might
test, and the machine-checked evidence behind it. Where a residual exists it is
named here, in the open-items ledger, or in the platform-acceptance list at the
end — nothing is omitted.*

## Gameplay correctness

| Claim | Evidence |
|---|---|
| The campaign completes | `check_campaign_progression`: silver-coin collection and persistence, all four boss rematches and amulet pieces on production-persisted saves, Wizpig 1 unlock (both directions) and victory, Wizpig 2 victory setting the true-ending bit. Residuals (three, named in the gate's docs): the lobby rematch door driven-to rather than through, trophy/T.T.-amulet chaining (gated separately by `check_trophy_series`), and the credits animation tail, whose screen is proven renderable by the cheat fixture. |
| Every reported player issue is fixed | Issues #9–#20 closed across 1.0.5/1.0.6 with per-issue regression gates (aspect leak, Taj portraits, tearing, PAL audio, overlay input handoff, and the rest). |
| No opponent can wedge | `check_ai_unstick_opponents`: 32 seeded races, 23 stuck episodes, 23 recoveries, 0 wedges; the recovery mechanism (out-of-bounds respawn) identified and witnessed. |
| Boss verdicts are honest | `check_boss_win_verdict`: win → cutscene 4 and never 5; lose → 5; non-vacuity proven by the losing arm. |
| Ghosts round-trip everywhere | `check_ghost_matrix`: 46 of 47 legal pairs write, persist, and replay a ghost in a fresh process; the 47th is an asserted autopilot non-producer, not a skip. Save format untouched. |
| Saves, events, input, and sound are deterministic | `check_state_hash` v3 streams byte-identical across reruns, window sizes, and backends; every presentation and camera setting proven presentation-only against those streams. |

## Presentation and play feel

| Claim | Evidence |
|---|---|
| No tearing in any default or numeric mode | Present-mode policy: caps at or under refresh ride FIFO with the deadline grid; above/uncapped ride Mailbox where offered; Immediate only behind the explicit Allow Tearing choice. Gated per arm. |
| Frame delivery is even, and measured | M2 histograms + M3 grid-quantized interpolation phase: phase variance improved 2.6–140×, every displayed frame on one grid point through p99; regression-gated in `check_pacing_quality`. |
| Scrolling surfaces move smoothly under smoothing | M4 retained UV interpolation, all driver classes, wrap-aware, zero added arena bytes, pixel-gated with a proven-non-vacuous midpoint arm. Motion smoothing is no longer labelled a preview. |
| Settings apply while you play | Frame limit, motion smoothing, and allow tearing are live; camera keys apply at the next track. The stale-walk hazard is closed with a measured negative control (36 stale replays without the invalidation, 0 with). |
| The camera can be kept out of walls, one setting away | Available opt-in; **default-on rejected by device acceptance 2026-08-07** ("too sensitive/causes issues" in play), so the shipped default is the authored camera. The correction itself is unchanged and fully gated: per-family treatment table, chatter 0, shoulder flips 0, retract latency 0 ticks, emergency framing 46 frames across the 24-arm display matrix (was 1,585), 4P finalizer p99 130 µs against an 833 µs budget. Every one of those was green when the default was pulled — see `docs/architecture/camera-obstruction.md` §10.1, and MOTION-01's thresholds need recalibration before another flip. Reduced-motion option ships alongside. |
| Ultrawide players get the same camera quality | Exact fan admission: emergency rates at 32:9/140° now within 2× of the recorded 4:3 baseline. |
| VRR and handheld owners have honest choices | `display-margin` (refresh − 3) and a 40 cap, both on the always-tear-free grid; one-click Original/Smooth pace over the live keys. |
| PAL is first-class | Beat-aware audio cushion (underruns 19→0 in the modelled beat), `pal-display` gate arm, smooth-display guidance at launch, German menu on every disc with the retail menu one setting away. |

## Code and repository health

| Claim | Evidence |
|---|---|
| The vendored decomp is current and untouched | Synced to the upstream's 100%-matching head; both carried exceptions retired; vendored text byte-identical (alias layer carries the readable names; `nm`-verified raw exports). |
| Raw-symbol residue is named or registered | 84 evidence-based aliases for everything port code touches; the archaeology remainder (154 func_, 292 D_) is a tracked open-items wave with a never-blind-rename rule; deliberate keeps (GLOBAL_ASM markers, UNUSED idiom, symbol references) carry written rationales. |
| No dead code known | GLES ifdefs kept-with-evidence (live sibling convergence), retired replay alias deleted with git archaeology, hash tiers named without moving a byte, orphan env vars deleted or documented, Metal backend removed. |
| The harness cannot silently rot | One authoritative helper per concern (PPM, EEPROM, fatal markers, row parsing); contract pins live as data with 589 entries and 240 self-controls; gates that diverge from shipping env are inventoried, with the shadow family running shipping-config arms. |
| Structure matches its size | Test wiring extracted from the build definition; the audio engine in nine seam-split files with byte-identical PCM; `check_ci_contract` engine at a third of its size. |
| Open items are closed or defended | Every ledger row is fixed-with-gate, closed-with-evidence, or carries a skeptic-proof deferral rationale in place. |

## What still needs hardware this machine does not have

These are the only known claims resting on code review or a single environment
rather than a machine check here — each is recorded in its gate's docstring or
the roadmap:

- Windows: the high-resolution timer path (MinGW cross-compiled, unmeasured),
  adaptive-vsync fallback, real-pad confirmation of the overlay input fix.
- VRR panels and mixed-refresh multi-monitor: the display-change event and
  `display-margin` wall-clock behaviour.
- Browser breadth beyond current Chromium; physical-GPU Linux breadth.
- The audible-DAC matrix on physical output devices.
- The window-server-dependent gate arms (FPS-only overlay, realtime pacing
  baseline) require an active display session to run.
