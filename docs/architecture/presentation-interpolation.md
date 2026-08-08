# Presentation interpolation — ownership proof and seam inventory

> Derived by reading `main` at commit `d707767`, not by reading the historical
> ledgers. Where a ledger and the code disagree, the code is recorded here and
> the disagreement is called out by name in
> [Corrections to inherited descriptions](#corrections-to-inherited-descriptions).
> This document is the review anchor for presentation work: a change that
> contradicts it updates it first. The 1.0.1 "Historical experiment —
> generation-keyed core-object interpolation" note referenced throughout is at
> `CHANGELOG.md:780-834`.

The authoritative simulation runs in fixed original-region quanta. The host
presents at whatever rate the player's policy asks for. Everything between those
two facts is presentation, and presentation's whole contract is that it may
create more images and must never create more game updates. The question this
document answers is narrower and older than that contract: **when an
intermediate image is drawn by re-walking an already-built display list, whose
memory is it reading?**

That question closed the 1.0.1 experiment. `docs/FPS_UNCAP_RELEASE_AUDIT.md:50`
(`PRES-001`) states the hazard exactly: *a walk delayed until after task `K+1`
begins can observe rewritten viewport, matrix, vertex, texture, and nested
display-list dependencies from mutable arenas*. The audit's remedy was scope
reduction — turn the replay off. The remedy on `main` today is different and
stronger: the replay still runs, but every dependency it reads is one the real
walk copied. That is a claim about the *arena* and about *explicitly captured*
externals. It is now also a proven claim about every other non-arena pointer
the address registry can resolve, because an interpolated walk that cannot
prove ownership of one refuses to draw — see
[residual obligation 2](#residual-obligations).

---

## 1. The timeline proof

### 1.1 One authoritative pass, in order

DKR double-buffers its graphics tasks, and that skew is the reason the naive
timing argument does not work. The ordering below is the real one, read out of
the game loop rather than assumed.

Call one iteration of `main_game_loop` (`game/src/thread3_main.c:288`) pass *N*.

| # | Site | What happens |
|---|---|---|
| 1 | `thread3_main.c:323` | `gfxtask_run_xbus(gDisplayLists[gSPTaskNum], …)` submits the list **authored during pass N−1**. `gSPTaskNum` flips immediately after (`:324`). **Guarded** by `gDrawFrameTimer == 0 && !elideCatchupRender` (`:318-322`): a frame-timer hold or an elided catch-up pass submits nothing, and steps 2-4 do not happen. |
| 2 | `stubs_dkr.c:584` → `:480` | `osSendMesg` dispatches that task synchronously. `sched_dispatch_task` calls `gfx_start_frame()` (`:502`), `gfx_run()` (`:520`), `gfx_end_frame()` (`:521`). **This is the real HLE walk of task K.** |
| 3 | `gfx_pc_dkr.c:6466-6481` | Inside `gfx_start_frame`, while replay is armed: `gfx_retained_task_capture_begin(authored_tick, g_dkrArenaBase, g_dkrArenaSize)`, `gfx_presentation_packet_capture_begin`, and a byte copy of the walk-entry `rdp`/`rsp`/`gfx_segment_table`/texrect state. |
| 4 | `gfx_pc_dkr.c:6589-6596` | Inside `gfx_end_frame`, while replay is armed: `gfx_retained_task_capture_commit(dkr_last_walked_dl, dkr_walk_entry_segments)`, then `gfx_presentation_packet_freeze()` and `gfx_shadow_replay_freeze()`. **Task K's ownership transaction publishes here.** |
| 5 | `thread3_main.c:331-427` | The game authors **task K+1** into the alternate list/matrix/vertex/triangle heaps and terminates it with `gDPFullSync`/`gSPEndDisplayList`. |
| 6 | `thread3_main.c:436` | `gfxtask_wait()` collects the completion message for task K. **Guarded** by `gDrawFrameTimer != 1` (`:430`) and `gSkipGfxTask == FALSE && !elideCatchupRender` (`:431-435`) — the same conditions that decided whether step 1 submitted anything. |
| 7 | `thread3_main.c:442` | `mempool_free_queue_clear()` — deferred frees land here. |
| 8 | `thread3_main.c:466` → `video.c:381` | `fb_update` reaches its blocking `osRecvMesg(gVideoMesgQueue, …)`. **That call is the host boundary.** |
| 9 | `stubs_dkr.c:612-905` | The host-boundary adapter runs: live settings apply, pacing, instruments, the alpha-0 endpoint present, and the presentation subloop. |
| 10 | return to `thread3_main.c:471` | Pass N ends. Pass N+1 begins at step 1 and submits task K+1. |

A pass whose guards skip steps 1-4 publishes no new retained task and does not
advance `dkr_frame_index`, so `dl_fresh` (`stubs_dkr.c:741-743`) is false at step
9 and the interval holds the front image instead of replaying — the guards
subtract replay opportunities, never add them.

Two consequences follow, and only the second one is a defence.

**Task K+1 is already fully authored when the subloop runs.** Step 5 precedes
step 9 unconditionally. The code says so in three independent places:
`gfx_pc_dkr.c:977-980` ("during presentation replay the live arena may already
contain task K+1"), `gfx_pc_dkr.c:6731-6733` ("by the time a presentation replay
runs, the NEXT tick's display list has already been built and registered"), and
`docs/UNCAPPED_PRESENTATION.md:121-126`. `stubs_dkr.c:714-724` depends on it
deliberately: `presentation_task_peek_authored()` hands the already-built K+1
list to `gfx_dkr_capture_future_deformations()` precisely because it exists.

**No replay walk ever runs after the next real HLE walk begins.** Step 2 of pass
N+1 is the next `gfx_start_frame`, and it is separated from step 9 of pass N by
the return from `osRecvMesg` — a single-threaded return, with no replay call
site between them. Every call to a replay walk in the tree is inside step 9 or
inside step 2 itself:

- `stubs_dkr.c:758` — the alpha-0 endpoint replay, step 9, test-gated;
- `stubs_dkr.c:861` — the midpoint replay, step 9, in the subloop;
- `stubs_dkr.c:532` — the zero-delta harness, inside step 2 immediately after
  `gfx_end_frame` of the same task, gated on `MDKR_TEST_REPLAY_WALK`.

There are no others (`gfx_dkr_replay_walk*` has exactly these three call sites).

So the hazard's precondition is **half** gone. What is gone is the possibility of
a replay observing a *newer walk's* HLE state — `dkr_walk_entry_*`, the frozen
registry, the retained token. What is **not** gone is that a newer authored
task's bytes are already in the arena. That half is closed by ownership, below,
and the distinction matters: a future change that narrowed the retained copy on
the belief that "the subloop runs before K+1" would reintroduce `PRES-001`
exactly.

### 1.2 Every read the interpolated walk performs

`gfx_dkr_replay_walk_impl` (`gfx_pc_dkr.c:6644-6744`) is the whole surface.
Reads, in order, with the tick boundary each retained copy was taken at:

| Read | Source | Taken at |
|---|---|---|
| Refusal preconditions | `dkr_last_walked_dl`, `dkr_replay_pass`, `dkr_walk_entry_valid`, `gfx_presentation_packet_frozen()` (`:6655-6658`) | `dkr_last_walked_dl`, `dkr_walk_entry_valid` and the packet's frozen flag are cleared by `gfx_dkr_replay_invalidate()` (`:6758-6765`). `dkr_replay_pass` is **not** — it is the re-entrancy guard, set at `:6667` and cleared at `:6738` inside the replay's own cleanup. |
| The authored task | `gfx_retained_task_acquire(dkr_last_walked_authored_tick, …)` (`:6659-6662`) — refuses any tick but the one requested | step 4 of pass N |
| Frozen matrix/camera registry | `gfx_shadow_replay_restore(overrides, override_count)` (`:6664`) | step 4 (`gfx_shadow_replay_freeze`) |
| HLE entry state | `memcpy(&rdp, dkr_walk_entry_rdp, …)`, `&rsp`, `gfx_segment_table` ← `retained_task.segments`, `dkr_in_texrect` (`:6703-6707`) | step 3 of pass N |
| The arena itself | `g_dkrArenaBase`/`g_dkrArenaSize` are **swapped to the private image** for the duration of the walk (`:6693-6695`), restored at `:6719-6722` | step 3/4 — one 16 MiB copy per armed task |
| Every DL pointer | `dkr_resolve` (`:940-1025`); during replay it consults `gfx_retained_task_resolve_arena_token` before the live arena (`:982-987`) and maps external hits through `dkr_retain_resolved_pointer` (`:927-938`) | step 3/4 |
| Bounds for bulk reads | `dkr_arena_room` (`:1032-1045`), which resolves retained external spans through `gfx_retained_task_dependency_room` during replay | step 3/4 |
| Interpolated camera | `mdkr_camera_interpolated_view_projections(num, den, …)` (`stubs_dkr.c:846-848`) over the presentation snapshot pair | authoritative tick boundaries T and T+1 (`presentation_snapshot_capture`, `stubs_dkr.c:712`) |
| Interpolated object/vertex/effect data | the frozen presentation packet, looked up by *observed* identity (`gfx_pc_dkr.c:5630`, `:5620`) | T at step 3/4; T+1 from the forward census at `stubs_dkr.c:720` |

Writes during a replay are confined to the backend (`start_frame`,
`end_frame`, `finish_render`) and to interpreter-local state that
`gfx_dkr_reset_interpreter_state()` re-seeds. Caster capture is explicitly
suppressed for the duration (`:6676`, released `:6730`) so the replay cannot
republish the shadow frame the real walk already committed. `gfx_run` is called
on the retained list, never on the game's render tree — the structural reason
no `game/src` code can advance during presentation (`:6628-6643`).

### 1.3 The five branch classes that could break it

The honesty question for this document is whether any branch lets an
interpolated walk run in a state it does not own. Each candidate, resolved:

**Catch-up.** `catchup_ticket = present_sched_pending_ticks() != 0u`
(`stubs_dkr.c:642-643`). Both the endpoint replay (`:745`) and the subloop
(`:788`) require `!catchup_ticket`. A host opportunity that issued more than one
ticket takes `:679-685`, presents the authored image, and returns without ever
entering a presentation interval. **Closed.**

**Multi-tick due inside the subloop.** `present_sched_advance_units` is called at
the top of the loop body (`:824`) and `ticks_due != 0` breaks *before* any
replay (`:827-829`). A replay can therefore never execute on an opportunity at
which the next tick is already due. **Closed.**

**Render elide.** `elideCatchupRender` is decided once per pass at
`thread3_main.c:303-304` and suppresses the graphics task entirely
(`stubs_dkr.c:490-498`), so `dkr_frame_index` does not advance and
`dl_fresh` (`stubs_dkr.c:741-743`) is false for the whole interval — the subloop
holds the front image (`:870-874`). Surface elision can additionally flip
`present_sched_render_elided()` (`present_sched.c:419-425`) mid-interval, but
only in the direction of *not* drawing; the guards at `:835` and `:888` both
read it. **Closed.**

**Live config apply.** `mdkr_video_config_apply_pending(MDKR_VIDEO_SCOPE_LIVE)`
has exactly one call site, `stubs_dkr.c:640` — after the previous pass's subloop
has exited and before this pass's `platform_present_subloop_fields()`. The
applier `platform_present_config_apply()`
(`platform_sdl_min.c:3263-3286`) runs `gfx_dkr_replay_invalidate()` *first*, then
the snapshot stage reset, the one-shot re-arm, the policy push, and the pacer
re-init. The settings panel cannot bypass it: `publish()` calls
`mdkr_video_config_push_presentation()` only while the presentation domain has no
registered applier (`video_config_runtime.c:704-706`), which under a live engine
it always has. **Closed.**

**Endpoint replay.** The default endpoint is the real walk's own image —
`endpoint_drew = true` with no replay (`stubs_dkr.c:760-762`). The delayed-replay
form at `:758` is a negative control gated on `MDKR_TEST_DELAYED_ENDPOINT_REPLAY`
*and* the versioned token `MDKR_INTERNAL_TEST_TOKEN=mdkr64-presentation-replay-v1`
(`stubs_dkr.c:458-466`, `present_sched.c:77-85`), and when it is on the subloop's
midpoint replay is disabled by the same predicate (`:837`). It also still runs at
step 9, not later. **Closed, test-only.**

**No OPEN rows.** No path was found on which an interpolated walk executes after
the next tick's real HLE walk has begun, or on which it reads live-arena bytes.

---

## 2. Seam inventory

Every capability the 1.0.1 "historical experiment" note (`CHANGELOG.md:780-834`)
describes, mapped to what is actually in the tree. **Gate** is the control that
turns the capability off; **arm** is what must be true for it to run at all.

The common arm for all of them is `present_sched_replay_armed()`
(`present_sched.c:315-335`): a present policy above the tick rate
(`present_sched_present_subloop()`) **and** `present_sched_smoothing_enabled()`.
Both resolve from player-facing config keys whose defaults are `original` and
`off` (`video_config.c:799-806`). One short-circuit precedes both:
`present_sched_test_replay_walk()` returns armed before either check runs
(`present_sched.c:316-318`), so `MDKR_TEST_REPLAY_WALK` arms the capture, freeze
and snapshot machinery even under Original with smoothing off — which is exactly
what the zero-delta harness needs. It fails closed without the versioned token
(`present_sched.c:222-225`), which is why that variable is a test seam rather
than a diagnostic. Nothing here is `#if 0`, nothing is dead code,
and nothing is behind a build flag: it all compiles into every native and browser
build and runs when a player selects Frame limit above original with Motion
smoothing = Interpolated.

| Capability | Symbols | Gate | Current test | Staleness |
|---|---|---|---|---|
| Retained authored task (arena image, list, segments, external deps) | `gfx_retained_task.{h,c}`; begin `gfx_pc_dkr.c:6467`, commit `:6591`, acquire `:6659` | armed only; budget valve `MDKR_RETAINED_ARENA_COPY_BUDGET_BYTES` (`gfx_retained_task.c:71`) | `tests/test_gfx_retained_task.c`, `tests/test_gfx_retained_task_budget.c` (ctest `gfx_retained_task`, `gfx_retained_task_budget`) | Current. Post-dates the audit; this is the mechanism that closes `PRES-001`. |
| Generation-keyed object root/child matrices | `presentation_snapshot.{h,c}`; `mdkr_camera_replay_object_world` (`gfx_pc_dkr.c:419`); consumed at `:5251-5269` | `MDKR_TEST_OBJECT_INTERPOLATION=off\|0` (`gfx_pc_dkr.c:445-453`) — **opt-out**, on by default when armed | `tests/test_presentation_snapshot.c`; `tests/check_presentation_matrix.py` | Current, and **shipping** — not a disabled prototype. |
| Billboard local matrices and world anchors | `mdkr_camera_replay_billboard_anchor`/`_matrix` (`gfx_pc_dkr.c:426-431`); `gfx_presentation_packet_register_matrix` | object-interpolation gate | `tests/test_presentation_packet.c`; `check_presentation_matrix.py` | Current. Both halves take the camera precondition from one function, `mdkr_camera_replay_billboard_camera` (`camera.c`): a billboard whose viewport camera is not interpolated holds its authored anchor **and** its authored local matrix. The anchor lacked that check from the day the matrix gained it, so for one frame after every camera cut sprite positions interpolated while sprite scale and tilt held at T. The anchor now takes the binding's `viewport` for exactly this reason. |
| Model vertex deformation (XYZ) | `gfx_presentation_packet_capture_deformation`/`_lookup_deformation`; walk `gfx_pc_dkr.c:5619-5680` | `MDKR_TEST_DEFORMATION_INTERPOLATION=off\|0` (`:455-463`) — opt-out | `test_presentation_packet.c`; `check_presentation_matrix.py` model control | Current. |
| Vertex shade RGBA | `gfx_presentation_packet_note_deformation_color` | `MDKR_TEST_VERTEX_COLOR_INTERPOLATION=off\|0` (`:497-505`) — opt-out | `check_presentation_matrix.py` colour control | Current. |
| Direct world-space particle meshes (point/line) | `gfx_presentation_packet_register_vertex_identity`, `_note_particle_deformation` | `MDKR_TEST_PARTICLE_INTERPOLATION=off\|0` (`:487-495`) — opt-out | `check_presentation_matrix.py` particle control | Current. |
| Primitive alpha (sprite/model/line-particle) | `gfx_presentation_packet_note_primitive_alpha` | `MDKR_TEST_PRIMITIVE_ALPHA_INTERPOLATION=off\|0` (`:507-516`) — opt-out | `check_presentation_matrix.py` fade control | Current. Note the 8.8-in-signed-field decode the CHANGELOG records is in the shipped path. |
| Shield/magnet shear (two-lifetime recipe) | `mdkr_camera_replay_effect_world` (`gfx_pc_dkr.c:422-425`); `_note_effect_override` | `MDKR_TEST_EFFECT_INTERPOLATION=off\|0` (`:518-526`) — opt-out | `check_presentation_matrix.py` effect control; `check_effect_shell_envelope.py` (displacement inside the interpolation envelope); the `ownertickmismatch` arm of `check_smoothing_stage_bisection.py` | Current. **It anchored the shell to the WRONG endpoint from `d2808f9` (1.0.1) until 2026-08-08** — see residual obligation 4 and `docs/evidence/smoothing-artifact-repro-2026-08.md` §2, §4. |
| Authored UV scroll | `GfxPresentationUvScroll` (`gfx_presentation_packet.h:34-56`); capture `gfx_pc_dkr.c:4396-4461`, apply `:5884-5889` | `MDKR_TEST_UV_SCROLL_INTERPOLATION=off\|0` (`:528-536`) — opt-out | `tests/check_presentation_matrix.py:72`, `:799`, plus `tests/test_presentation_packet.c`'s `check_uv_scroll` — capture refusals, tick-exactness of the published table, and one case per confirmation clause. | Current. **Unit-covered since 2026-08-08**; before that the only evidence was an integration arm that needs a ROM. |
| Authored UV scroll — authored rate | `GfxPresentationUvScroll.authored/rate_*/phase_*`; the rate registry in `presentation_snapshot.h`, filled by `obj_loop_texscroll` and read by `dkr_capture_uv_scroll_endpoints` | `MDKR_TEST_UV_SCROLL_AUTHORED_RATE=off\|0` — opt-out, and the red arm of the gate below | `tests/check_smoothing_stage_bisection.py`'s route C (`[UV-SCROLL-AUTHORED]`), plus `check_uv_scroll_authored` in `tests/test_presentation_packet.c`. | Current. |
| Projected shadow deformation | `gfx_presentation_packet_register_projected_shadow_vertex`, `_note_projected_shadow_deformation` | `MDKR_TEST_PROJECTED_SHADOW_INTERPOLATION` requires the versioned token to turn **off** (`:465-474`); `MDKR_TEST_PROJECTED_SHADOW_VERTEX_LERP` requires it to turn **on** (`:476-485`) | `tests/check_presentation_shadows.py:155`, `:157` — the only gate that sets either variable; `check_presentation_matrix.py` does **not**. Registration itself is unit-covered at `tests/test_presentation_packet.c:170-190`. | Current. Asymmetric gating is deliberate: the production behaviour is unreachable from a bare env var in either direction. |
| Forward `{T+1}` structural census | `gfx_dkr_capture_future_deformations` (`gfx_pc_dkr.c:4673`), scanner `dkr_scan_future_deformations` (`:4477`) | armed only; requires `presentation_snapshot_replay_target_tick` agreement (`:4695-4701`) | `test_presentation_packet.c` forward-packet cases | Current. Read-only: flow control, segment/billboard state, owner matrices, `G_VTX`, `G_TRIN` only. |
| Camera snapshot / bank endpoints | `gfx_shadow_frame.{h,c}`; `gfx_shadow_camera_endpoint_validate` (`stubs_dkr.c:754`) | `MDKR_TEST_CAMERA_VP_ENDPOINTS` (`gfx_shadow_frame.c:219`) | `tests/check_camera_snapshot_coverage.py` | Current. This is `PRES-002`'s subject and it is production, not diagnostic. |
| Zero-delta replay harness | `gfx_dkr_replay_walk(NULL, 0)` (`stubs_dkr.c:532`) | `MDKR_TEST_REPLAY_WALK` **and** the versioned token (`present_sched.c:216-241`); `recompose` additionally forces recompose | `tests/check_render_purity.py:194`, `:198`, `:228` — the only gate that sets it | Test seam. Never armed on a shipping path. |
| Delayed endpoint replay (negative control) | `stubs_dkr.c:756-766` via `gfx_dkr_replay_walk_endpoint` | `MDKR_TEST_DELAYED_ENDPOINT_REPLAY=1` **and** `MDKR_INTERNAL_TEST_TOKEN` | `check_presentation_matrix.py` | Test seam. Restores the pre-fix ordering on purpose. It has its own entry point rather than reusing `_walk_interpolated(…, 0, 1)` so that endpoint-ness is declared, not inferred — see residual obligation 2. |
| Live-arena poison gate | `dkr_test_live_arena_poison_enabled` (`gfx_pc_dkr.c:538-546`); replay `:6684-6692`, `:6723-6728` | `MDKR_TEST_RETAINED_ARENA_POISON` **and** the token | `check_presentation_matrix.py` lifetime-safety arm | Test seam. It proves the replay reads no live **arena** byte. Non-arena reads are covered by the row below instead — see [residual obligation 2](#residual-obligations). |
| Uncaptured-external fail-closed | `dkr_retain_resolved_pointer` (`gfx_pc_dkr.c`), capture at `dkr_capture_nonarena_list`; stats `gfx_dkr_replay_get_uncaptured_stats` | always on for a strictly interior alpha; `MDKR_TEST_UNCAPTURED_EXTERNAL` **and** the token force every lookup to miss | `tests/check_smoothing_stage_bisection.py` `[UNCAPTURED-EXTERNAL]` row | Current. Production counter is zero; the seam is the only way to reach the refusal branch. |
| Retained-dependency rewrite gate | `gfx_presentation_packet.c:95` (`MDKR_TEST_RETAINED_DEPENDENCY_REWRITE`) | env | `tests/check_presentation_matrix.py:697`, `:716` — the only gate that sets it. `tests/test_presentation_packet.c` does **not** exercise it (zero occurrences of `rewrite`). | Test seam. |
| Presentation perf census | `present_perf_*` (`present_sched.c:652`, `MDKR_PRESENT_PERF`) | env | `check_presentation_matrix.py` cost gate | Test/telemetry seam. |
| Rotation shortest-arc / snap census | `rotation_arc_audit` (`presentation_snapshot.c`); `[SNAPSHOT] rotarccheck/rotarcsnap/rotarcviolation` | always on — counters only, no branch reads them | `tests/check_motion_quality_battery.py` R1 | Current. The audit recomputes the arc from `(prev, curr, out)` rather than trusting `presentation_lerp_angle`'s own arithmetic, which is the mutation it grades. **Snap policy (deliberate):** beyond a quarter turn per tick the axis returns the CURRENT endpoint, per axis. Any choice mixes channels for a >0x4000 delta — position is at T+α either way — and snapping forward is the boundary-continuous one: the pose shown at α→1 is the pose the next pair shows at α→0, where snapping back would pop the full delta at every tick boundary. Per-axis rather than whole-pose so one spinning axis does not forfeit smoothing on the others; the composed pose can mix ticks across axes for at most the presents of one tick, bounded by that tick's own rotation delta. Revisit only with device evidence. |
| Long-arc angle smear (negative control) | `presentation_snapshot.c` seam block; `presentation_lerp_angle` | `MDKR_TEST_ROTATION_LONG_ARC=1` **and** the versioned token | `tests/check_motion_quality_battery.py` R1 — the only gate that sets it | Test seam. Restores artifact class C3 on every delta, on purpose. |
| Discontinuity hold census | `[SNAPSHOT] disconthold/discontblend` (`presentation_snapshot.c`, object and camera resolve) | always on — counters only | `tests/check_motion_quality_battery.py` R2 | Current. The blend counter is the non-vacuity witness for the hold counter's zero. |
| Camera-cut notes | `presentation_snapshot_note_camera_cut`; `[SNAPSHOT] camcutnote/camcutconsumed/camcutunconsumed` | always on | `tests/check_camera_snapshot_coverage.py` (both race arms); `tests/test_presentation_snapshot.c` | Current. The note names the **viewport**, not the `gCameras[]` slot. The recorder composes the slot as `viewport + (gCutsceneCameraActive ? 4 : 0)` (`camera.c`, `camSetProjMtx`), so a note filed in slot space from a player index missed by four bits on exactly the ticks a cutscene camera owned the viewport — and the old commit cleared the whole mask unconditionally, destroying the miss. Notes are now consumed only by the capture of the viewport they name, carried otherwise, and `camcutunconsumed` counts any that survive that viewport's publish. A pose-based classifier cannot witness this class: a cut invisible from the pose is invisible to a pose-based classifier, and the 2000-unit teleport threshold is 5-30x above the distances these cuts move. |
| Identity-table fail-closed | `presentation_snapshot_note_spawn`; `[SNAPSHOT] identityinsertfail` | always on | `tests/test_presentation_snapshot.c` | Current. A spawn the identity table cannot register sets a sticky flag that fails the next commit whole. It previously returned silently on the belief that "the next capture fails whole" — true only while the table is still full at capture time. Failing open there publishes a new object under a recycled address's dead generation, which `resolve_object_pair`'s generation check then accepts. |
| Discontinuity suppression (negative control) | `presentation_snapshot.c` seam block; both resolve paths | `MDKR_TEST_IGNORE_DISCONTINUITY=1` **and** the versioned token | `tests/check_motion_quality_battery.py` R2 — the only gate that sets it | Test seam. Blends across spawns, respawns, warps and camera cuts, on purpose. |

Staleness summary: **no shelved code was found.** The 1.0.1 note describes a
prototype; what is in the tree is that prototype's production successor, arm-gated
on two player settings rather than disabled, with the opt-out env vars retained as
per-capability isolation controls for the evidence gates in
`docs/UNCAPPED_PRESENTATION.md:205-226`.

---

## 3. PRES-001 / PRES-002 disposition

The audit's five rewritten-dependency classes, each with the retained copy or
timing argument that closes it **today**.

| Class | Disposition | Closure on `main` @ `d707767` |
|---|---|---|
| Viewport | **CLOSED** | `G_MOVEMEM`/`G_MV_VIEWPORT` copies `sizeof(Vp_t)` as an explicit dependency during the real walk (`gfx_pc_dkr.c:5905-5909`); the replay resolves the same original address to those private bytes via `dkr_retain_resolved_pointer` (`:927-938`). |
| Matrix | **CLOSED** | `G_MTX` copies `sizeof(Mtx)` and registers the same key with the shadow registry and the presentation packet (`gfx_pc_dkr.c:5251-5259`); camera/object substitution is limited to matrix keys the completed real walk observed (`gfx_shadow_replay_restore`, `:6664`). |
| Vertex | **CLOSED** | `G_VTX` copies the exact `retained_n` batch, plus the remastered smooth-normal stream when present (`gfx_pc_dkr.c:5638-5658`); `G_TRIN` copies the triangle batch (`:5881-5883`). Endpoint pairing requires the *whole* span to be retained (`gfx_retained_task_retained_span`, `gfx_retained_task.h:98-104`) or the batch holds. |
| Texture | **CLOSED** | `LOADBLOCK` (`gfx_pc_dkr.c:3671-3674`), `LOADTILE` (`:3700-3705`) and `LOADTLUT` (`:3731-3734`) each copy the exact byte span the real walk is about to consume, bounded by `dkr_arena_room`. |
| Nested display list | **CLOSED** | `G_DL`/`G_DMADL` targets go through `dkr_resolve` (`:5167`, `:5195`), which during replay decodes arena tokens against the retained *original* window first (`:982-987`) and maps non-arena children through the copied-dependency table. The top-level list and arena-backed segment bases are rebased into the private image at commit (`gfx_retained_task.h:70-74`), and `gfx_segment_table` is restored from `retained_task.segments` rather than from live state (`:6705-6706`). |

| Audit row | Disposition | Basis |
|---|---|---|
| `PRES-001` (`FPS_UNCAP_RELEASE_AUDIT.md:50`) | **CLOSED — superseded, not by scope reduction** | The row's remedy was disabling the replay. `main` instead re-enabled it on top of an atomic retained-task transaction: whole-arena private image + explicitly copied external dependencies + transactional publish (a failed capture leaves the last complete task intact and the opportunity holds). The row's *hazard statement* remains literally true of any implementation that reads live memory — see §1.1: task K+1 **is** already authored when the subloop runs. It is the reading, not the timing, that changed. **Scope of this closure:** each of the five classes has a verified capture site (above). It is no longer a per-handler proof either way: `dkr_retain_resolved_pointer` fails **closed** on a lookup miss during an interpolated walk, so a handler that reads an uncaptured non-arena dependency costs a held authored image rather than a silent misread. See residual obligation 2. |
| `PRES-002` (`FPS_UNCAP_RELEASE_AUDIT.md:51`) | **CLOSED — its stated precondition is met** | The row permits re-enabling once there is "an immutable `{T,T+1}` ownership model and new endpoint/midpoint evidence." The ownership model is §3's table plus the forward census (`gfx_pc_dkr.c:4673`). The evidence contract is the four layers at `docs/UNCAPPED_PRESENTATION.md:188-226` — authority, endpoint identity, midpoint sensitivity, lifetime safety — with `check_presentation_matrix.py`, `check_camera_snapshot_coverage.py` and the live-arena poison gate (arena reads only — residual obligation 2) as the principal arms. Camera snapshots latch at display-list projection emission, not later from mutable camera globals (`UNCAPPED_PRESENTATION.md:134-139`). |

Both rows carry the audit's own header caveat (`FPS_UNCAP_RELEASE_AUDIT.md:3-10`):
that ledger is historical and superseded, and its "release blocked" framing
describes 2026-08-01.

### Residual obligations

Not `PRES-001` failures, but the properties any future change must preserve:

1. **Do not narrow the retained copy on a timing argument.** The full 16 MiB copy
   exists because the arena exposes no safe high-water mark or closed pointer
   graph (`UNCAPPED_PRESENTATION.md:161-176`). §1.1 shows the timing argument
   that would justify narrowing it is false.
2. **CLOSED — the uncaptured-external gap now fails closed.** *(was: not
   covered by any current gate)*

   The hazard as recorded: `dkr_retain_resolved_pointer` redirected a resolved
   pointer to private bytes *only if* `gfx_retained_task_lookup_dependency`
   found it, and on a miss it **returned the live pointer**. That is
   fail-**open**, and the live-arena poison gate cannot see it —
   `dkr_test_live_arena_poison` memsets **only the arena**, so registry-resolved
   globals and rodata display lists pass the gate green.

   Both halves are now closed, and the second is what makes the first
   permanent:

   - `dkr_retain_resolved_pointer` **fails closed** for a *strictly interior*
     alpha: on a lookup miss for a pointer outside the retained arena window it
     sets `dkr_replay_dependency_failed`, returns `NULL`, and counts
     `uncapturedext`. The walk aborts and the subloop holds the authored image.
     The exclusion of endpoint walks (the zero-delta harness and the
     delayed-endpoint negative control) is deliberate: their contract is
     byte-exact reproduction of an image the authoritative walk already drew,
     so refusing them would break the exactness evidence rather than protect
     anything.

     **The exclusion is declared by the caller, not inferred from the alpha.**
     `gfx_dkr_replay_walk_impl` takes an `endpoint` bool, set true only by
     `gfx_dkr_replay_walk()` and `gfx_dkr_replay_walk_endpoint()`, and
     `dkr_replay_interior_alpha = !endpoint && numerator < denominator`. It
     previously read `numerator != 0u`, which made this proof rest on the
     pacer's ticket accounting — the subloop breaks on `ticks_due != 0` before
     an interior walk can reach numerator 0 — rather than on the caller's
     declared intent, which is the exact belief this obligation was written to
     remove. `[REPLAY-SUMMARY] zeroalphainterior` counts any interpolated walk
     that still arrives at alpha 0 (also a duplicated image) and
     `check_presentation_matrix.py` asserts it zero on both backends.
   - The real walk now copies non-arena `G_DL`/`G_DMADL` targets
     (`dkr_capture_nonarena_list`). Instrumenting the miss path on the pre-fix
     tree found 18 distinct such lists per walk — `dRdpInit`, `dRspInit`, the
     `dRenderSettings*` table, `dDebugFontSettings`, the dialogue-box and
     transition-fade lists, `dTextureRectangleModes` — all authored static
     storage nothing rewrites. That is why the fail-open never produced a
     visible defect, and exactly why it could not stay: the safety of the
     replay rested on a belief about which storage is immutable.

   Evidence: `tests/check_smoothing_stage_bisection.py`'s
   `[UNCAPTURED-EXTERNAL]` row asserts `uncapturedext=0` across every
   production arm, forces every lookup to miss under
   `MDKR_TEST_UNCAPTURED_EXTERNAL` plus the versioned token and requires the
   walks to refuse into held authored images, and asserts the seam does not arm
   without the token. Measurements in
   `docs/evidence/smoothing-stage-attribution-2026-08-08.md`.

   "The replay reads nothing live" is now true of the arena, of explicitly
   captured externals, **and** of anything else: an uncaptured read is a
   refused replay, not a misread.
3. **Keep the live-arena poison gate in the acceptance set** for the coverage it
   does have: it is the only check that fails if a handler reads a live *arena*
   byte during replay.
4. **A replayed recipe must be measured against its own tick.** Every class
   that reconstructs a pose does it as
   `interpolated_pose + (captured_source - alpha_zero_pose)`, and that residual
   carries only the render-only adjustments it is meant to carry when both
   terms describe the same authored tick. Hand it a capture from a different
   tick and it becomes a constant offset of one tick of the owner's travel, in
   position *and* heading, at every alpha — invisible to endpoint exactness, to
   the authoritative hashes, and to every "does this stage change pixels"
   control, because a wrong pose is as coherent as a right one. Two instances
   shipped: `mdkr_camera_replay_effect_world` passing the `{T+1}` recipe
   (v1.0.1–v1.0.3), and billboard registrations from a pass that never
   submitted its list surviving into the next freeze. Both are now witnessed
   structurally: `GfxPresentationMatrixOwner.capture_tick` is stamped at
   authoring time and compared on every replayed recipe, counted as
   `ownertickcheck`/`ownertickmismatch` in `[PRESENT-PACKET]` and asserted zero
   on every production arm of `check_smoothing_stage_bisection.py`.
5. **Keep `MDKR_VIDEO_SCOPE_LIVE` apply at a single call site.** The safety of
   the presentation domain is a property of `stubs_dkr.c:640` being the only
   place it runs (`video_config.c:106-146`).

---

## Corrections to inherited descriptions

Recorded so later readers do not re-derive them:

### Why a texscroll batch publishes its rate instead of its result

`obj_loop_texscroll` advances a level texture through a **two-bit
accumulator**: it adds the authored rate (quarter units per authored tick) to
a residue, emits the whole units that completes into the triangle UVs, and
keeps `residue & 3` for next time. The surface moves at a perfectly constant
speed; the bytes do not. An authored rate that is not a multiple of four
alternates its emitted whole-unit step by one, forever — 127 quarter units at
`updateRate` 2 emits 63 units on one tick and 64 on the next.

Recovering that speed by differencing two authored ticks cannot work, and not
because the differencing is weak: **there is no constant difference to find**.
The two readings genuinely disagree, the confirm-or-hold rule refuses (it is
right to: an unrepeated displacement is exactly the mis-resolved wrap it
exists to catch), and the batch holds its texture phase on every interpolated
present. The surface keeps its 30 Hz cadence while the world around it glides,
which is what reads as shimmer on water and lava.

So the driver publishes the rate itself, together with the residue the bytes
already owe, and the replay reconstructs the position from first principles:

    offset(alpha) = (phase + rate * alpha) / 4        [whole UV units]

At `alpha` 1 that is the emitted whole units plus the *next* tick's residue,
which is where the next record starts from its own advanced bytes — so the
quantisation cancels across the tick boundary instead of beating against the
present rate. One observation suffices, nothing is inferred from the bytes,
and **the confirmation rule is unchanged for every measured scroller**: waves,
rain, the skydome and the texture animator's flipbooks never enter the
registry and still have to agree with their previous tick.

- **"Production interpolation scope is camera + UV-scroll only" is wrong.**
  `gfx_dkr_replay_walk_interpolated` passes `object_alpha_valid = true`
  (`gfx_pc_dkr.c:6751-6756`), and every per-capability env gate is an *opt-out*
  that defaults on (`:445-536`). Object roots, billboards, model deformation,
  vertex colour, particles, primitive alpha, shield/magnet shear and UV scroll
  are all live whenever smoothing is armed. `docs/UNCAPPED_PRESENTATION.md:32`
  states the same production scope.
- **The prototype is not "disabled".** There is no `#if 0`, no build flag, and no
  dead code. It is off by default only because `Video.FrameLimit` defaults to
  `original` and `Video.MotionSmoothing` to `off` (`video_config.c:799-806`).
- **`CHANGELOG.md:823-834` describes the 1.0.1 endpoint of the experiment,** not
  `main`. Its closing sentence ("the release path therefore disables motion
  smoothing and retained replay completely") is historical.
- **Authored UV scroll is absent from the CHANGELOG's historical note** because it
  post-dates it.

---

## Build and test evidence

Commit `d707767`, macOS arm64, `cmake --preset rel`.

```
$ cmake --build build-rel --target mdkr_presentation_packet_test \
        mdkr_gfx_retained_task_test mdkr_presentation_snapshot_test -j8
[100%] Built target mdkr_presentation_snapshot_test
Built target mdkr_presentation_packet_test
Built target mdkr_gfx_retained_task_test

$ ctest --test-dir build-rel -R "presentation_packet|gfx_retained_task|presentation_snapshot"
1/4 Test #57: presentation_snapshot ............   Passed    0.51 sec
2/4 Test #58: presentation_packet ..............   Passed    0.90 sec
3/4 Test #59: gfx_retained_task ................   Passed    0.30 sec
4/4 Test #60: gfx_retained_task_budget .........   Passed    0.29 sec

100% tests passed, 0 tests failed out of 4
```

The CTest target name is `presentation_packet`; the executable is
`mdkr_presentation_packet_test` (`cmake/tests.cmake:708-713`). There is no
`test_presentation_packet` build target.

`tests/test_presentation_packet.c` compiles and passes against the current
`gfx_presentation_packet.c` — it is a live unit, not an orphaned prototype test.
The ROM-driven gates named in §2 (`check_presentation_matrix.py` and friends) were
not run here; they require a ROM and a display and belong to the stage that
enables behaviour, not to this ownership derivation.
