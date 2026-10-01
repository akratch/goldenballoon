#!/usr/bin/env python3
"""ROM-free evidence rules for check_camera_held_shot_faults.py; never launches the game."""
import unittest

from check_camera_held_shot_faults import (BOUND_ARMS,
                                           NEAR_CONTACT_EXTENSION_TICKS,
                                           RELEASE_HOLD_TICKS, epochs,
                                           release_hold_invariant, probes,
                                           verify)

SLOT = 0
CENSUS = ("camera_motion summary slot_ticks=100 ticks=100 cut_ticks=0 "
          "cut={adjacent=0 unadjacent=0} "
          "profiles={full=100 safety_only=0 depenetrate_only=0} "
          "events={retract=1 recovery=1 alternate_entry=1 alternate_exit=1 "
          "emergency_entry=0 discontinuity=0 degenerate=0} "
          "release_hold={held_ticks=0 spans=0 window=%d} "
          "chatter={oscillation_cycles=0 oscillation_cycles_excused=0 "
          "correction_reengagements=%d} "
          "shoulder={flips=0 continuous_surface=0 new_surface=0 basis_crossings=0} "
          "churn={blocker_changes=0 same_surface=0 new_surface=0} "
          "emergency={max_dwell=0} "
          "held={episodes=1 max_dwell_ms=%.1f max_divergence_deg=%.3f bound_exits=%d "
          "dwell_bound_ms=%.1f divergence_bound_deg=%.3f max_episode_ms=100.0} "
          "discontinuity_per_1000_ticks=0.0000")
STATS = ("camera_motion stat name=cut_pos_step unit=wu/cut samples=0 min=0.0 mean=0.0 p95=0.0 max=0.0\n"
         "camera_motion stat name=cut_ang_step unit=deg/cut samples=0 min=0.0 mean=0.0 p95=0.0 max=0.0")


def probe(tick, continued, length, recovering=0, contact=1, near=0, valid=1,
          rejection="none", dwell=0.0167, divergence=1.0):
    return (f"camera_held_chain_probe tick={tick} slot={SLOT} continued={continued} "
            f"prior_length={length} valid={valid} target=(0.10000,-0.20000) "
            f"recovering={recovering} remaining={'2.00000' if valid else 'nan'} "
            f"returned=0 rejection={rejection} contact={contact} near={near} "
            f"dwell={dwell:.4f} divergence={divergence:.3f}")


def summary(tick, degraded=0, hidden=0, penetrated=0):
    return (f"camera_obstruction_observe summary tick={tick} selected=1 "
            f"gate=MODERN(logical_camera_unchanged) projection_mismatches=0 "
            f"resolved={{corrected=1 penetrated={penetrated} invalid=0 degraded=0}} "
            f"probe_degraded={degraded} target_hidden={hidden} "
            f"transition={{invoked=0 clear=0 cut=0 tuple_cut=0}} end")


def detail(tick, status=1, offset=(0.0, 50.0, -120.0), extra=""):
    pivot = (float(tick), 0.0, float(tick))
    eye = tuple(p + o for p, o in zip(pivot, offset))
    return (f"camera_obstruction_observe detail tick={tick} viewport=0 normal_slot=0 "
            f"physical_slot={SLOT} desired=(0,0,0) "
            f"effective=({eye[0]:.3f},{eye[1]:.3f},{eye[2]:.3f}) "
            f"intent={{fresh=1 pivot=({pivot[0]:.3f},{pivot[1]:.3f},{pivot[2]:.3f}) }} "
            f"resolve={{status={status} valid=1 corrected=1 alternate=1 {extra}"
            f"discontinuity=0 resolved=(0,0,0) clearance=0 end}}")


def motion_detail(tick, state="blocked=1 retracted=1 recovering=0 held=0",
                  forward="0.00000,0.00000,1.00000"):
    return (f"camera_motion detail tick={tick} viewport=0 normal_slot=0 "
            f"physical_slot={SLOT} family=1 profile=full eye=(0,0,0) "
            f"forward=({forward}) state={{{state} alternate=1 emergency=0}}")


def published(tick, active=1, dwell=0.0167, divergence=1.0):
    return (f"camera_held_shot_published tick={tick} slot={SLOT} frame={tick + 70} "
            f"active={active} dwell={dwell:.4f} divergence={divergence:.3f}")


def census(reengagements=0, max_dwell=100.0, max_divergence=1.0, exits=0,
           bounds=(2000.0, 60.0)):
    return CENSUS % (RELEASE_HOLD_TICKS, reengagements, max_dwell, max_divergence,
                     exits, bounds[0], bounds[1])


def trace(armed, ticks, reengagements=0, extra=(), census_row=None):
    """The armed row prints at the start of its own tick, as the runtime does."""
    fault = int(armed.split(" tick=")[1].split()[0]) if armed else None
    rows = []
    for tick, lines in ticks:
        if tick == fault:
            rows.append(armed)
        rows.extend(lines)
        rows.append(summary(tick))
    rows.extend(extra)
    rows.append(census_row or census(reengagements))
    rows.append(STATS)
    return "\n".join(rows)


def holding(start, count):
    """`count` accepted contact holds from `start`, with published evidence."""
    return [(tick, [probe(tick, int(tick > start), tick - start), published(tick),
                    detail(tick), motion_detail(tick)])
            for tick in range(start, start + count)]


def false_clear(moved_at=None, recovered_at=None):
    fault = 14
    ticks = holding(10, 4)
    for tick in range(fault, fault + RELEASE_HOLD_TICKS - 1):
        offset = (0.0, 50.0, -120.0) if tick != moved_at else (3.0, 50.0, -120.0)
        ticks.append((tick, [
            probe(tick, 1, tick - 10, recovering=int(tick == recovered_at), contact=0),
            published(tick), detail(tick, offset=offset),
            motion_detail(tick, "blocked=1 retracted=1 recovering=0 held=1")]))
    armed = (f"camera_held_fault armed kind=false_clear tick={fault} slot={SLOT} "
             f"ticks={RELEASE_HOLD_TICKS - 1} prior_length=4")
    return trace(armed, ticks)


def false_contact(counted=True):
    ticks = holding(10, 2)
    # A served hold, then a release and two recovering ticks.
    for tick in range(12, 12 + RELEASE_HOLD_TICKS - 1):
        ticks.append((tick, [probe(tick, 1, tick - 10, contact=0), published(tick)]))
    release = 12 + RELEASE_HOLD_TICKS - 1
    for tick in range(release, release + 3):
        ticks.append((tick, [probe(tick, 1, tick - 10, recovering=1, contact=0),
                             published(tick)]))
    fault = release + 3
    ticks.append((fault, [probe(fault, 1, fault - 10, contact=1), published(fault)]))
    armed = f"camera_held_fault armed kind=false_contact tick={fault} slot={SLOT} ticks=0 prior_length=13"
    events = [f"camera_motion reengagement tick={fault} viewport=0 normal_slot=0 "
              f"physical_slot={SLOT} release_tick={release} gap=3 "
              f"state={{prior_recovering=1 recovering=0 held=0 alternate=1 emergency=0 degenerate=0}}"]
    return trace(armed, ticks, reengagements=int(counted), extra=events if counted else ())


def projection(retired=True):
    ticks = holding(10, 4)
    fault = 14
    ticks.append((fault, [detail(fault, status=4, extra="discontinuity=1 ")]))
    ticks.append((fault + 1, [probe(fault + 1, 0 if retired else 1, 0 if retired else 4)]))
    armed = f"camera_held_fault armed kind=projection tick={fault} slot={SLOT} ticks=0 prior_length=4"
    return trace(armed, ticks)


def query(consumed=True, rejected=True, marked=True):
    ticks = holding(10, 4)
    fault = 14
    row = probe(fault, 1, 4, valid=0, rejection="endpoint") if rejected else probe(fault, 1, 4)
    ticks.append((fault, [row]))
    text = trace(f"camera_held_fault armed kind=query tick={fault} slot={SLOT} ticks=0 prior_length=4",
                 ticks)
    if consumed:
        text = text.replace(summary(fault),
                            f"camera_held_fault consumed kind=query tick={fault} slot={SLOT}\n" +
                            summary(fault))
    if marked:
        text = text.replace(summary(fault), summary(fault, degraded=1))
    return text


def bound_arm(kind="dwell_bound", republished=False, published_past=False,
              ordinary_ok=True, exits=1, lowered=True):
    """A held shot refused by a lowered bound, then the ordinary path."""
    dwell_bound, divergence_bound = BOUND_ARMS[kind]
    reason = "dwell" if kind == "dwell_bound" else "divergence"
    ticks = holding(10, 3)
    refused = 13
    status = "1" if ordinary_ok else "1 valid=0"
    ticks.append((refused, [probe(refused, 1, 3, valid=0, rejection=reason),
                            detail(refused, status=status)]))
    ticks.append((14, [detail(14, status=2)] + ([published(14)] if republished else [])))
    ticks.append((15, [detail(15, status=0, extra="").replace("corrected=1", "corrected=0")]))
    ticks.append((16, [probe(16, 0, 0), published(16)]))
    if published_past:
        ticks[0] = (10, [probe(10, 0, 0), published(10, dwell=dwell_bound + 1.0,
                                                    divergence=divergence_bound + 1.0),
                         detail(10), motion_detail(10)])
    bounds = (dwell_bound * 1000.0, divergence_bound) if lowered else (2000.0, 60.0)
    return trace(None, ticks, census_row=census(exits=exits, bounds=bounds,
                                                  max_dwell=50.0, max_divergence=1.0))


def soft_arm(held_too=False, declined=True, deferrals=1):
    ticks = holding(10, 2)
    for tick in range(12, 12 + deferrals):
        soft = "soft_blocker={seen=1 declined=%d extent=1.000} " % int(declined)
        lines = [f"camera_held_soft_decline tick={tick} slot={SLOT} blocker=317 extent=1.000",
                 detail(tick, status=0).replace(" clearance=0 ", f" clearance=0 {soft}")]
        if held_too:
            lines.append(published(tick))
        ticks.append((tick, lines))
    return trace(None, ticks)


class HeldShotEvidence(unittest.TestCase):
    def test_positive(self):
        verify(false_clear(), "false_clear")
        verify(false_contact(), "false_contact")
        verify(projection(), "projection")
        verify(query(), "query")

    def test_false_clear_negative_controls(self):
        for output, reason in (
                (false_clear(moved_at=16), "held eye moved"),
                # The route-wide hysteresis invariant sees it first.
                (false_clear(recovered_at=16), "released after a 3-tick clear run"),
                (false_clear().replace("held=1", "held=0"), "release-held"),
                (false_clear().replace(published(17), ""), "no active held publication"),
                (false_clear().replace("forward=(0.00000,0.00000,1.00000) state={blocked=1 retracted=1 recovering=0 held=1",
                                       "forward=(0.10000,0.00000,0.99500) state={blocked=1 retracted=1 recovering=0 held=1", 1),
                 "orientation moved")):
            with self.subTest(reason=reason), self.assertRaisesRegex(ValueError, reason):
                verify(output, "false_clear")

    def test_false_contact_must_fail_the_motion_gate(self):
        with self.assertRaisesRegex(ValueError, "did not count"):
            verify(false_contact(counted=False), "false_contact")

    def test_fault_reaches_the_held_proof(self):
        for output, reason in ((query(consumed=False), "never reached"),
                               (query(rejected=False), "did not reject"),
                               (query(marked=False), "degraded held probe"),
                               # A chain cannot continue across a tick it never
                               # proved; the chain-evidence audit sees that first.
                               (projection(retired=False), "lacks matching accepted history")):
            with self.subTest(reason=reason), self.assertRaisesRegex(ValueError, reason):
                verify(output, "projection" if "history" in reason else "query")

    def test_common_safety_and_location(self):
        good = projection()
        for output in (good.replace("penetrated=0", "penetrated=1", 1),
                       good.replace("window=9", "window=8"),
                       good.replace("camera_held_fault armed", "camera_held_fault_armed"),
                       good + "\n" + next(row for row in good.splitlines()
                                           if row.startswith("camera_held_fault armed"))):
            with self.subTest(output=output[:60]), self.assertRaises(ValueError):
                verify(output, "projection")

    def test_release_hold_invariant(self):
        def episode(clear_run, near_from=None):
            rows = [probe(1, 0, 0)]
            for index in range(1, clear_run + 1):
                tick = 1 + index
                near = int(near_from is not None and index >= near_from)
                rows.append(probe(tick, 1, index, recovering=int(index == clear_run),
                                  contact=0, near=near))
            return probes("\n".join(rows))
        hold, bound = RELEASE_HOLD_TICKS, RELEASE_HOLD_TICKS + NEAR_CONTACT_EXTENSION_TICKS
        self.assertEqual(release_hold_invariant(episode(hold)), 1)
        # Grazing from the served hold on: waits, released by leaving the band...
        rows = episode(hold + 3, near_from=hold)
        rows[(1 + hold + 3, SLOT)]["near"] = 0
        self.assertEqual(release_hold_invariant(rows), 1)
        # ...or by the bound.
        self.assertEqual(release_hold_invariant(episode(bound, near_from=hold)), 1)
        for rows, reason in ((episode(hold - 1), "outside"),
                             (episode(hold + 1), "not grazing"),
                             (episode(bound + 1, near_from=hold), "outside"),
                             (episode(hold, near_from=hold), "while grazing")):
            with self.subTest(reason=reason), self.assertRaisesRegex(ValueError, reason):
                release_hold_invariant(rows)

    def test_bound_arms(self):
        for kind in BOUND_ARMS:
            with self.subTest(kind=kind):
                self.assertEqual(verify(bound_arm(kind), kind), 1)
                for output, reason in (
                        (bound_arm(kind, lowered=False), "lowered bound"),
                        (bound_arm(kind, exits=0), "reached the lowered"),
                        (bound_arm(kind, republished=True), "re-seeded"),
                        (bound_arm(kind, published_past=True), "past its bound"),
                        (bound_arm(kind, ordinary_ok=False), "ordinary path")):
                    with self.subTest(reason=reason), self.assertRaisesRegex(ValueError, reason):
                        verify(output, kind)
        other = "divergence_bound"
        with self.assertRaisesRegex(ValueError, "refused by its divergence bound"):
            verify(bound_arm("dwell_bound").replace("dwell_bound_ms=200.0 divergence_bound_deg=180.000",
                                                    "dwell_bound_ms=3600000.0 divergence_bound_deg=2.000"),
                   other)

    def test_soft_decline_arm(self):
        self.assertEqual(verify(soft_arm(deferrals=3), "soft_decline"), 3)
        for output, reason in ((soft_arm(deferrals=0), "never deferred"),
                               (soft_arm(held_too=True), "held the view"),
                               (soft_arm(declined=False), "did not decline")):
            with self.subTest(reason=reason), self.assertRaisesRegex(ValueError, reason):
                verify(output, "soft_decline")

    def test_epochs_split_on_tick_restart(self):
        text = "\n".join((summary(5), summary(6), summary(1), summary(2)))
        self.assertEqual([len(epoch.splitlines()) for epoch in epochs(text)], [2, 2])


if __name__ == "__main__":
    unittest.main()
