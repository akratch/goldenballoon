# Public issue acceptance

This register separates implementation, verification, and release availability.
An existing feature or a passing test for one symptom is not evidence that an
entire public issue is resolved. The repository inventory on 2026-09-05 found
four open issues in `akratch/goldenballoon`, and none in `akratch/mdkr64` or
`akratch/goldenballoon-staging`. A read-only refresh of `akratch/goldenballoon`
on 2026-09-06 finds five open issues, including new report #63; the other
repositories were not refreshed in that check.

**Qualification refresh (2026-09-08).** The complete suite has now executed
against the integrated candidate at `b6e8ff40`: 267 of 271 tasks passed in
628m49s, and all four failures are resolved and re-validated (three were gate
defects -- two hardcoded engine-arm ceilings and one census budget -- and the
fourth is the opt-in camera item, reproduced byte-identically on shipped
v1.6.0). The statuses below are therefore no longer waiting on requalification:
where a section says a behavioural gate had not executed, it has now, and the
gate names are listed with their verdicts in
[the historical release checklist](../RELEASE_CHECKLIST.md#historical-candidate-status-2026-09-08).
This does not close any issue: release availability and the owner-side device
matrix are separate obligations, and #61's Windows/NVIDIA symptom cannot be
settled by any macOS result. This update reads the local source tree and saved
validation logs; it does not mutate or close issues. The candidate remains
**not release-ready**. The custom-character KTX2/BasisU
alignment finding is fixed in the candidate with a valid-texture regression
and a clean 601-second ASan/UBSan fuzz run (4,004,077 executions). KTX2 remains
enabled; final-artifact qualification is pending. See
[the historical release blockers](../RELEASE_CHECKLIST.md#historical-candidate-status-2026-09-08).
The passes below remain historical evidence until explicitly requalified.
This checkpoint uses non-executing checks under the latest conversation-supplied
instructions; the saved workstation policy is unchanged. Earlier execution-control
refusals are not a current hardware diagnosis or a new validation result.
The old-source diagnostic suite
at `12cab88c` has finished with 36/271 failed tasks in 626m50s and process exit 1.
Its results do not qualify newer source changes or justify issue closure.

Clean-source artifact checkpoint `12cab88c` additionally passes macOS bundle
and mounted-DMG LaunchServices/WebGPU qualification with exact provenance.
Its packaged launcher-skip check passes all nine arms. The clean local-only
browser payload passes rendered cloud-surface absence and save-custody checks.
These narrow artifact results neither establish Windows/NVIDIA equivalence
nor replace physical-controller acceptance or the unrestricted release suite.

The unrestricted diagnostic run also reports Modern-camera motion failures;
the candidate corrects a shoulder-census miscount, but a separate correction
re-engagement remains unresolved. Neither is issue-closure evidence. Linux
Release compilation now passes at the later `ba620a1b` checkpoint, but Linux package/runtime
qualification is incomplete. Its first complete ROM-free rerun had three online
test failures; the transport fix passes focused Linux optimized/sanitizer
checks, and the complete rerun with the adapter-harness correction passes
279/279 tests. Strict packaging at `5fc099fc` now produces both Linux artifacts;
tarball launcher checks pass and the AppImage's shared payload matches it.
A subsequent clean `72770712` qualification includes PNG-writer hardening and
passes 280/280 selected tests, built/packaged launcher checks, strict packaging,
and shared-payload parity. Native AppImage runtime/hardware acceptance and
final-source full qualification remain pending. The original native diagnostic
CTest run's stale lobby-takeover success count is also corrected; its focused
CTest and CI drift controls now pass, not the complete unrestricted suite.
Further texture-pack PNG admission hardening is compiled but awaits behavioral
validation; it is not included in the earlier qualified Linux artifacts.
Fresh clean-source Linux Release compilation at `da4d9924` now includes that
PNG hardening and the zero-time camera correction, with the partyless beta/KTX2
profile and exact binary build stamp. All default targets compile; no Linux
test, launcher or new package has been qualified at that source checkpoint.
Clean Debug compilation at `c522ebae` and focused ASan/UBSan-instrumented
compilation now pass; these are not behavioral acceptance results.
The subsequent complete Windows cross-build now passes after fixes for three
unit-target portability failures and a test-fixture bounds error. No Windows
executable was run, and the Windows/NVIDIA and package gaps remain open.
The complete incremental Windows and web builds at `c701145d` additionally
compile the zero-time camera release-hold correction. Generated
JavaScript/WebAssembly checks pass without executing the module; this is not
browser acceptance or final-package provenance. A Windows portable-mode
read-back predicate is corrected with pending CI controls; that source fix does
not establish Windows settings persistence or package acceptance.
See the release checklist for these separate
qualification gaps; neither changes the Windows/NVIDIA or controller verdicts.
The older full diagnostic run has also failed its realtime pacing re-anchor
budget. That release gate remains unresolved; improved per-attempt reporting
does not fix the pacing result or establish its cause.

## #63 — Future Funland trophy missing

[Report](https://github.com/akratch/goldenballoon/issues/63), opened
2026-09-05: winning Future Funland's championship leaves the trophy cabinet
empty and Tracks does not acknowledge the trophy clear. The reporter lists
version 1.5.2 and RTX 2070 Super; OS, ROM revision, backend and exact build are
not specified. A later read-only refresh found a maintainer comment at
2026-09-06T11:47:07Z confirming the helper defect and targeting the next release;
the issue remains open. That comment is not package-validation evidence.

**Confirmed source defect; candidate fix now behaviourally validated
(2026-09-08).** `check_trophy_series.py` executes all five championship worlds
and passes: Future Funland runs its four authored rounds (tracks 17/32/33/15),
awards `rank=0 points=36 new=0x300`, and persists 0x300 into a checksum-valid
EEPROM. The gate additionally requires the production cabinet to publish the
state each world just earned -- the reported symptom was an empty cabinet, which
the award assertions alone never reached -- and world 4's rank-3 finish serves as
the negative control that a no-trophy result must not display one.
`mdkr_trophy_state()` rejected worlds above 4 even though Future Funland is
world 5 and the EEPROM stores five two-bit trophy fields. The same helper
guards the championship award in `menu.c`, cabinet spawning in
`object_functions.c`, and trophy model selection in `objects.c`. Thus world 5
could neither acquire its trophy bits nor display an already-saved trophy.
Tracks reads the fifth field directly, so the suppressed award also explains
its missing completion marker without implicating the reporter's GPU.

The candidate accepts all five championship worlds using the world enum while
retaining invalid-world/null-output guards. The previous unit assertion that
world 5 was invalid is replaced with exhaustive coverage of all 1,024 saved
trophy combinations across five worlds, plus invalid-boundary cases. The
existing production-series harness now includes Future Funland's four rounds
and gold award/EEPROM check; its world-selection control also accepts world 5.
The four-mainland-trophy rocket unlock condition and save format are unchanged.

**Related-pattern sweep:** Tracks also ORed medal fields across save slots,
allowing bronze plus silver to appear as unearned gold. The candidate merges
the best rank per world instead. The T.T. status counter now visits only the
five saved trophy fields instead of shifting a signed mask across 16 fields;
its four-icon authored layout is unchanged. Optimized and ASan/UBSan game,
runtime-contract and save-codec targets compile. Python 3.10 parser and diff
checks pass; new behavioral tests and old-code controls have not executed.
See [the domain audit](trophy-domain-audit.md) for scope, retained mainland-only
rules and the remaining validation matrix.

**Pre-release priority / remaining acceptance:** execute the unit and old-bound
negative control; win all four Future Funland rounds through production menus,
confirm award and checksum-valid persistence, restart and inspect the cabinet
and Tracks marker. Also verify existing fifth-world trophies display, lower
finishes do not downgrade gold, other worlds stay intact, and Adventure Two
and final packaged builds behave correctly. Source confirmation is not a
claim that the reporter's exact environment has been reproduced. Existing
saves whose award was never written cannot have that missing achievement
inferred safely; no automatic gold grant or save migration is introduced.
Keep #63 open until these checks and release availability are established.

## #62 — Opponent skill

[Report](https://github.com/akratch/goldenballoon/issues/62): a text field does
not explain which values are accepted.

**Fixed in the candidate and gated, not yet released.** `enh_ai_difficulty`
passes in the qualifying run. The shared launcher/in-game
settings row now uses the existing controller-navigable combo with Original,
Hard, and Brutal choices. Labels describe their relative speeds. The stored
values remain `authored`, `hard`, and `brutal`; the default, gameplay calculation,
restart requirement, persistence, and environment locks are unchanged.

**Mechanism and class sweep.** `drawKey()` falls back to `InputText` whenever a
string key has no `optionsFor()` entry. Opponent skill was missing that entry.
Inspection of every string schema declaration found the other closed domains
already routed to choices: simulation cadence, frame limit, smoothing, tearing,
presentation mode, shadows, camera obstruction/comfort, menu languages, window
mode, rumble profile, and controller bindings. Aspect expressions, FOV values,
texture-pack paths, and disabled-pack lists intentionally remain open domains.

**Verification (optimized native build).**

```sh
MDKR_DEDICATED_TEST_DESKTOP=1 MDKR_APP_TESTS_ALLOWED=1 MDKR_AUDIO=0 \
  ctest --test-dir build-rel --output-on-failure -j1 \
  -R '^(app_schema|app_opponent_skill_choices|app_ui_policy|video_config|video_config_runtime|enhancement_registry)$'
```

Result: `100% tests passed, 0 tests failed out of 6`. Removing only the new
option table and its routing, then rebuilding, makes
`app_opponent_skill_choices` fail with `Required regular expression not found`.
Restoring and rebuilding passes all six again. The schema dump enumerates the
actual `optionsFor()` route, including the choices and labels, rather than an
independent hard-coded claim. This proves widget selection, not a new physical
controller acceptance run.

**Remaining acceptance:** physical-controller navigation and selection on the
packaged candidate, followed by release availability.
The renewed sweep also found that mixed-case and unknown legacy values could
display raw text instead of their effective named choice. Gameplay already
accepts case-insensitive names and falls back to Original for unknown values.
The new shared pure resolver preserves that behavior while aligning combo
selection, spoken labels and separate current/Next Play snapshots. Raw settings,
restart staging and environment locks are unchanged. Case-variant/fallback and
source-binding regressions are registered, not executed; neither those additions
nor the earlier widget pass qualifies the final physical-controller journey.
The [acceptance guide, section 2](../RELEASE_CANDIDATE_TEST_GUIDE.md#2-launcher-and-settings)
now specifies both settings surfaces, all three choices, persistence, defaults,
and the physical controller/candidate identity to record. This is an acceptance
procedure, not new device evidence.

## #60 — Skip launcher after setup

[Report](https://github.com/akratch/goldenballoon/issues/60): optionally enter
the game directly when opening the executable.

**Implemented in the candidate, not yet released.** `Launcher.SkipWhenReady`
is opt-in; Shift or both controller shoulders restore the launcher. Invalid
ROMs and failed-boot recovery must not be bypassed.

**Verification (optimized native build).** On a dedicated test desktop:

```sh
MDKR_DEDICATED_TEST_DESKTOP=1 MDKR_APP_TESTS_ALLOWED=1 MDKR_AUDIO=0 \
  python3 tests/check_launcher_skip.py --build build-rel --rom "$ROM" -v
```

Result: `PASS launcher skip: arms=9 boots=2 stays=7 controls=2`. Covers enabled,
default-off, Shift, two shoulders, one shoulder, late hold, invalid ROM,
controller teardown ordering, and failed-boot recovery. These are native
launcher decision/dispatch tests, not proof of a shipped Windows executable.

**Remaining acceptance:** Windows/package proof of direct launch, the escape
gestures, invalid-ROM refusal, and failed-boot recovery on the final artifact.
The renewed source sweep also found two gaps outside the historical scripted
hold tests: separate controllers could combine one shoulder each into the
two-shoulder gesture, and controllers attached during ROM validation were not
discovered. The candidate now recognizes pairs on one controller and reconciles
borrowed handles by stable SDL instance ID, including detachment and index races.
A production-sampler fixture with a deterministic SDL boundary is added, not
executed. Real controller and final-package acceptance remain required. See
[the follow-up sweep](release-issue-followup.md).

## #61 — Three separate split-screen symptoms

[Report](https://github.com/akratch/goldenballoon/issues/61): Walrus Cove's
purple viewport after the loop/tunnel, black sky edges on multiple tracks, and
lower-resolution pause UI when player two pauses.

The [separate symptom walkthrough](../RELEASE_CANDIDATE_TEST_GUIDE.md#3b-separate-split-screen-acceptance-for-issue-61)
requires independent sky, pause-owner, and Windows/NVIDIA Walrus Cove verdicts.
Its addition does not change any of the evidence statuses below.

- **Sky edges: fixed and rechecked in the candidate.** See
  [the mechanism and mutation control](renderer.md#fixed-the-split-screen-sky-quad-is-a-43-object-in-a-widescreen-frustum--issue-61).
  `check_split_screen_backdrop.py --build build-rel --rom "$ROM" -v` passes:
  4:3 derives the original `200x150` extent, 16:9 derives `267x150`; worst
  sky-band black coverage is 0.16% for P1 and 4.27% for P2, below the respective
  1.5% and 5% ceilings.
- **Purple viewport: unresolved acceptance.** Must reproduce the reported
  two-player Walrus Cove loop/tunnel route and inspect both viewports. A
  single-player cave flash or a Fossil Canyon sky-edge pass does not settle it.
  The subsequent dense investigation below isolates a blue obstruction on
  this route and qualifies a local candidate, but does not yet establish
  equivalence to the reporter's Windows/NVIDIA purple symptom.
- **P2 pause resolution: automated macOS acceptance passes; Windows remains.**
  The dedicated two-player check now compares both pause owners in Restored
  and Remastered on GL and WebGPU, including real scaled-UI and wrong-font
  controls. This is not a verdict about the reporter's Windows/NVIDIA setup.

**Focused native investigation.** An optimized US 1.1/WebGPU two-player
Walrus Cove run reached 6,200 presentation frames, with P1 on lap 2 and P2
on lap 1. The 100-frame capture sampling did not establish the reported full
purple viewport; inspection did show camera/wall clipping, which needs finer
sampling and a causal comparison, not a blanket retail-fidelity explanation.
Separate same-input two-player Ancient Lake runs paused with P1 or P2 at frame
3,200 and captured at 3,350, using Restored/WebGPU and 2x render scale. Visual
inspection showed matching pause lettering and the expected different panel
colors. This is a current macOS observation, not a Windows/GPU-matrix verdict
or a substitute for a quantitative pause-resolution regression.

**Quantitative pause acceptance (2026-09-05).** The packaged arm64 executable
from clean commit `f1fd70e4c1ff5d14df69c4aa8d8288bf6ff6b10b` passes
`check_split_screen_pause_resolution.py` on both GL and WebGPU: eight arms
per backend, with independent saves/configuration. P1's blue and P2's red
panel prove the requested owner. The common RESTART RACE glyph contours
overlap by 98.29% in Restored and 99.25% in Remastered. Native-output text
edge detail exceeds each owner's scaled-UI control by 36.4%/36.5% in
Restored and 33.3%/33.2% in Remastered. Substituting the original bitmap
font for Remastered P2 gives only 57.16% contour overlap and is rejected.
Both racers' simulation streams remain identical across all eight arms;
output-pass telemetry has no late world draws or begin failures.

The check uses a 960x540-point window (1920x1080 drawable on this Retina
host) and 2x scene rendering. At 720 points, GL's Restored bitmap glyphs
align to an integer pixel grid and the downsampled control has essentially
the same edges; the non-integer source-pixel scale is necessary to make
that control discriminate. No product renderer change or reduced threshold
was used to obtain these passes. The test is registered as serial GPU work
in the main suite. All image evidence remains local and ROM-derived.

Keep the issue open until all three are accounted for.

**Walrus Cove curtain investigation and candidate (2026-09-05).** Every-frame
sampling over frames 2700–3519, with zero dropped captures, establishes a
large flat-blue obstruction in each player's viewport after the loop. The
camera-relative void curtain was drawn after scenery with a depth-writing
opaque material, allowing its artificial 250-unit depth to hide valid bridge
geometry. Enabling Modern camera correction or removing the activation
plane's 14-unit padding does not fix this reproducer. Removing the curtain
exposes the bridge but also removes its intended coverage over holes.

The candidate draws the same curtain before scenery without depth writes in
Restored/Remastered; Pure retains the authored final pass. Geometry, activation
rules, and shadow-caster exclusion remain intact. The registered serial
`check_split_screen_void_coverage.py` passes all 20 US 1.1 arms on local
shipping-SDL2 GL/WebGPU: Restored and Remastered recover valid scenery for
both players, Pure preserves ten byte-identical authored captures per backend,
and every policy comparison preserves SIMHASH v3 and both racer streams.
At frame 3061 P1's exact flat-colour coverage falls from 84.86% to 5.67%; at
frame 3100 P2 falls from 83.03% to 4.64% in Restored. The retained pixels
disagree with the no-curtain control, proving coverage was not simply deleted.
The identical acceptance predicate rejects authored, deleted-curtain, and
early-but-depth-writing controls for each player and backend. Synthetic
controls also reject an unrelated-colour replacement for actual scenery.

**Neighboring gates completed locally.** The saved `neighbor-gates-final.log`
records PASS for `world_fx_matrix`, `render_purity`,
`camera_snapshot_coverage`, `split_screen_backdrop`,
`split_screen_pause_resolution`, and `split_screen_void_coverage`: six selected
tasks out of the then-271-task suite. This covers representative 1P/2P/4P
world-effect ownership, render/simulation separation, multiplayer and cutscene
camera snapshots, and the three focused split-screen candidates. It does not
complete the full renderer, waterfall, or release matrix.

The same log emits stale-artifact warnings, including that the native binary
predates `cmake/tests.cmake`. Preserve these passes as local candidate evidence;
they do not certify the current source tree or final package. Rebuild the final
candidate and repeat the required gates on an authorized dedicated test desktop.
European revision coverage and Windows/NVIDIA acceptance, including equivalence
to the reported purple symptom, remain pending. All captures and raw logs remain
private, ROM-derived evidence.

**Fresh candidate follow-up (2026-09-06).** The rebuilt native game passes
all 20 US void arms again. The initial European probe misses P1's obstruction
at the US witness frame and remains a failed qualification attempt. A dense
European survey fixes its witnesses at frame 2980 for P1 and 3058 for P2.
All 20 European arms then pass the same pixel thresholds and broken-render
controls, with identical authoritative streams and twelve byte-identical Pure
captures per backend. This follow-up does not establish Windows/NVIDIA
acceptance or the full European release matrix.

## #58 — Bonus portraits and NDS content

[Report and clarification](https://github.com/akratch/goldenballoon/issues/58):
Taj's results portrait should resemble a blue elephant, with portraits also for
Terry and Wizpig; separately, the reporter asks about Dixie, Tiny, and NDS tracks.

**Portrait correction implemented and verified in the candidate, not released.**
The first-party procedural Taj card now has blue elephant ears, face and trunk;
no extracted portrait is bundled. Inspection of the actual two-player Rankings
capture confirmed the changed appearance beside the unchanged Diddy card.

`check_taj_results_portrait.py` and `check_taj_hud_portrait.py` both pass on
optimized native WebGPU and OpenGL. Results coverage includes two races and a
full intervening menu/stage teardown; the Adventure HUD check preserves the
lead player. The new face assertions reject both captures from the old tan
binary. A separate pixel control erases the gold band/jewel and must fail the
gold assertion, correcting the former detector's accidental reliance on tan
face pixels. Both gates now default to WebGPU and expose `--renderer gl` for
the diagnostic comparison.

**Existing packs remain compatible.** `check_bonus_portrait_pack.py` passes
all nine native arms, including existing Taj packs on GL and WebGPU, the
current digest in the author dump, unchanged Wizpig/Terry replacements, and
byte-identical stock rendering with packs disabled. `mod_texture_store` pins
current-name precedence, toggle behavior, and isolation from unrelated digests.
Removing only the alias makes its compatibility assertion fail; restoring it
passes. That unit also passes under ASan/UBSan. A freshly rebuilt UBSan binary
passes `check_array_bounds_sweep.py --no-build`, including its eight routes,
static class sweep and boundary controls.

Character Workshop and portrait-pack support do not by themselves implement
Dixie, Tiny, or any NDS track. Record those requests separately from the portrait
fix, respect the repository's no-ROM-derived-assets boundary, and do not close
the entire issue by citing the existence of an import UI.

**Remaining scope decision:** the maintainer must explicitly decide whether
Dixie, Tiny, and NDS tracks belong in this release or remain future work. None
of those content requests is implemented by the portrait candidate.
