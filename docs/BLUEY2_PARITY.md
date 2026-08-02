# Bluey 2 parity closeout

Issue [#2](https://github.com/akratch/goldenballoon/issues/2) reported that
Bluey's rematch boost and pace made the race effectively impossible, with an
apparent music interruption near the start. The report does not reproduce in
Original cadence. Golden Balloon 1.0.3 and the US 1.1 game agree closely enough
to close the issue; Enhanced cadence remains a useful positive control because
it reproduces the faster-boss symptom.

This note contains only measurements and hashes. The ROM, EEPROM images, PCM,
screenshots, and instrumented-emulator output remain local and uncommitted.

## Release and reference provenance

- Port: tag `v1.0.3`, commit `6bd7eccbb6cd40dc6a49d92f7f99c5150d6e335d`.
- Published macOS DMG SHA-256:
  `2e275380de3398c15f42cfd3be08a937f82cb14a1168a180cc5d5bc646feaa79`.
- Executable extracted from that DMG SHA-256:
  `a45527741ef5ac3abf8c345365e263a77eb95f3cea7830c070eb99620c3e8c2c`.
- Reference game: verified US 1.1 ROM, SHA-256
  `7de1a8fb2a9558cfc3d9ad4497df698c1e89cf7095ac1531557df2af40ba8bcf`.
- Reference runner: ares commit
  `91b112279ab2ce89c5fc9bff5dbb81e29af51a68`, with the repository's
  input/state/audio instrumentation only. The game code is unmodified.

The native and ares runs used the same scripted controller route, held A from
the same logical point, and compared racer index 1: vehicle 6, Bluey. Boss
clocks are derived in the common 60 Hz logic-tick domain because boss wrappers
do not accumulate ordinary lap-time clocks.

## State result

| Measurement | 1.0.3 Original | US 1.1 / ares | Enhanced positive control |
|---|---:|---:|---:|
| Bluey finish clock | 3,458 | 3,459 | 3,022 |
| Mean object speed | 12.529208 | 12.521118 | 14.377544 |
| Native/reference speed ratio | 1.000646 | — | 1.139822 |
| Checkpoint/lap agreement | 97.266881% | reference | 8.671922% |
| Position error p95 | 109.403882 | reference | 5,186.824 |
| Maximum checkpoint / lap | 24 / 2 | 24 / 2 | 24 / 2 |
| Oracle verdict | **PASS** | reference | **FAIL, as intended** |

Original differs from the reference finish by one 60 Hz field (16.7 ms), and
its mean speed differs by 0.065%. Enhanced finishes 437 fields (7.28 seconds)
before the reference and is about 14% faster over the comparison window. That
is the reported failure shape, and it is not the default.

Velocity means use only clocks where both compared racers are unfinished. The
Enhanced run's earlier finish shortens that common window, so its 1.139822 ratio
uses a 12.613852 reference mean; the 12.521118 value in the table is the longer
Original/reference window. The finish-clock result does not depend on that
windowing detail.

A second, progression-valid regression starts from a checksum-valid Adventure
checkpoint with Bluey 1 cleared, all four Snowflake Mountain silver-coin races
cleared, and Bluey 2 already visited. It traverses the production hub/lobby
route before loading course 52; there is no first-visit boss redirect. On the
current source it measures Original at 3,518 ticks and the Enhanced control at
3,019, with a 1.1696x mean-speed ratio. The small difference from the direct
oracle's Original clock is expected: the progression arm has a different prior
input/RNG history and an autopiloted human racer. Both independently classify
the cadence behavior the same way.

The one-off breadth matrix also produced an identical Bluey state stream and
finish for all eight normally available characters and for authored, 60, 70,
80, and 90 degree vertical FOV settings. This follows the code boundary:
[`update_bluey`](../game/src/vehicle_bluey.c) takes racer state, input, and the
logic update width; it does not read character selection, save progression, or
display/FOV configuration.

## Why Bluey jumps ahead

The behavior is original game logic, not a port-side speed multiplier. While
Bluey is AI-controlled, `update_bluey` temporarily subtracts 15 fields from the
race start timer supplied to the boss movement routine. When that private timer
reaches zero, it presses A for Bluey and assigns a three-tick boost, then restores
the shared timer. That is why Bluey starts before the player.

Original supplies complete two-field updates, matching the reference game's
normal partitioning. Enhanced supplies one field per update. The same authored
head start and boost are therefore integrated through twice as many smaller
physics calls. On this boss, that partition changes the trajectory and produces
the clearly faster result measured above. The in-game Video screen labels
Enhanced as gameplay-changing; fresh 1.0.3 configuration resolves to Original.

## Audio result

The apparent dip is also original behavior, but it is not loss of audio. The
intro music sequence stops before GO, the race sequence starts 3.204 seconds
later and 0.968 seconds after GO, and voice/engine/effects continue through the
transition. In the progression-valid 1.0.3 capture:

- the transition interval has RMS 8,133.5 and only 0.014% exact-zero samples;
- all 24 consecutive 250 ms windows over the first six seconds after GO are
  active, with a minimum RMS of 6,346.5;
- the longest exact-zero run is one sample, and the reverb bounds guard never
  trips.

The exact 1.0.0 and 1.0.3 native captures are byte-identical, both SHA-256
`8c4cc188c4fe1a325ee7e0adf44df5dec06e3ad9a0acd57c0c102d8af2a68aba`.
Against the game/ares reference, the full 153.26-second aligned envelope
correlation is 0.690 with spectral similarity 1.000 and a +0.01 dB RMS delta.
The 27.45-second start-focused window improves to 0.886 envelope correlation,
1.000 spectral similarity, and a -0.07 dB RMS delta. Those measurements include
the intro-stop/race-start transition.

## Why the visible timers do not match exactly

The HUD is not a frame-accurate synchronization instrument. The game converts
an integer 60 Hz field count to minutes, seconds, and hundredths in
[`racer.c`](../game/src/racer.c), then [`game_ui.c`](../game/src/game_ui.c)
replaces the ones digit of the hundredths display with a per-render counter.
That last digit therefore describes render phase, not authoritative race time.

There is a second sampling boundary: native screenshots are captured before
the post-present state trace, while the ares trace samples RDRAM around a
presented VI framebuffer. The CPU can begin the next update while the previous
framebuffer remains visible. A picture and a nearby memory sample can therefore
straddle one update even when each runner is internally correct.

In the questioned `09:42` versus `09:37` pair, the underlying clocks were 568
and 565 fields: a three-field, 50 ms difference, not five tenths of a second.
After alignment at the first mutually visible GO, the corrected sequence ends
one field apart even though its raw HUD labels read `09:42` and `09:48`.
Gameplay parity is established by the full state trajectory and finish clock,
not by that synthetic final digit.

The corrected 1920x1080 evidence master is
`bluey2-side-by-side-v2-youtube.mp4`, SHA-256
`761e81d0567d34e4a479a3f3520ac8321ca9b52819739f80a94497b37cd35f7d`.
It is intentionally not stored in this repository because it contains
copyrighted game imagery. A maintainer can upload that exact file to YouTube
and link it from the issue without weakening the numeric, reproducible proof.

## Standing regression

Run:

```bash
python3 tests/check_bluey2_rematch.py --build build --rom /path/to/owned-us-v11.z64
```

[`check_bluey2_rematch.py`](../tests/check_bluey2_rematch.py) requires the
progression-valid Original race to finish in the reference band, requires the
Enhanced arm to reproduce a materially earlier/faster finish, validates the
natural boss verdict and EEPROM checksum, and asserts the exact audio cue
transition without allowing the mix to drop out. It also substitutes Original
for the Enhanced result and requires its own sensitivity control to reject the
pair.

The independent local-only runner remains:

```bash
tools/run_oracle.sh bluey2_state_oracle \
  --native-bin build-rel/mdkr64 --native-arm original
```

The Ares trace, native trace, PCM, saves, screenshots, and generated JSON report
remain under `build/ares-oracle/` and must not be committed.
