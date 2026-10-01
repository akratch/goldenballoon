#!/usr/bin/env python3
"""ROM-free event-context reporting regressions; never launches the game."""

import contextlib
import io
import json
import os
import unittest
from pathlib import Path
from unittest import mock

import check_camera_motion_quality as motion
import check_camera_obstruction_runtime as runtime


class MotionReportingTests(unittest.TestCase):
    def test_policy_runner_isolates_state_and_ambient_faults(self):
        roots = []
        def capture(command, **kwargs):
            env, root = kwargs["env"], kwargs["cwd"]
            roots.append(root)
            self.assertTrue(root.is_dir())
            self.assertEqual(env["MDKR_SAVE_DIR"], str(root / "save"))
            self.assertEqual(env["MDKR_VIDEO_CONFIG_PATH"], str(root / "missing.ini"))
            self.assertNotIn("MDKR_TEST_CAMERA_EXACT_INVALID_TICK", env)
            self.assertNotIn("GE007_STALE_TEST", env)
            self.assertNotIn("MDKR_CAMERA_OBSTRUCTION", env)
            self.assertTrue(Path(command[0]).is_absolute())
            self.assertTrue(Path(command[command.index("--input-script") + 1]).is_absolute())
            return mock.Mock(returncode=0, stdout="clean")
        with mock.patch.dict(os.environ, {"MDKR_TEST_CAMERA_EXACT_INVALID_TICK": "2136",
                "GE007_STALE_TEST": "1", "MDKR_SAVE_DIR": "/not-test-state",
                "MDKR_CAMERA_OBSTRUCTION": "modern"}), \
                mock.patch.object(runtime.subprocess, "run", side_effect=capture):
            runtime.run("build-rel/mdkr64", "private.z64", "unset", 1, 1)
            runtime.run("build-rel/mdkr64", "private.z64", "unset", 1, 1)
        self.assertNotEqual(roots[0], roots[1])
        self.assertTrue(all(not root.exists() for root in roots))

    def test_policy_runner_cleans_up_failed_arms(self):
        for failure in ("exit", "timeout", "launch"):
            roots = []
            def fail(command, **kwargs):
                roots.append(kwargs["cwd"])
                self.assertTrue(roots[-1].is_dir())
                if failure == "timeout":
                    raise runtime.subprocess.TimeoutExpired(command, 1)
                if failure == "launch":
                    raise OSError("injected launch failure")
                return mock.Mock(returncode=1, stdout="injected child failure")
            expected = {"exit": RuntimeError, "timeout": runtime.subprocess.TimeoutExpired,
                        "launch": OSError}[failure]
            with self.subTest(failure=failure), \
                    mock.patch.object(runtime.subprocess, "run", side_effect=fail), \
                    self.assertRaises(expected):
                runtime.run("build-rel/mdkr64", "private.z64", "modern", 1, 1)
            self.assertEqual(len(roots), 1)
            self.assertFalse(roots[0].exists())

    def test_held_correction_exemption_requires_matching_safe_publication(self):
        detail = ("camera_obstruction_observe detail tick=1 physical_slot=0 "
                  "resolve={status=2 valid=1 corrected=1 alternate=1 clearance=0 source_degraded=0}")
        published = "camera_held_shot_published tick=1 slot=0 frame=7 active=1 dwell=0.5000 divergence=12.000"
        self.assertEqual(runtime.held_exemption(detail).nonheld, 1)
        self.assertEqual(runtime.held_exemption(published + "\n" + detail),
                         runtime.HeldExemption(nonheld=0, publications=1, episodes=1))
        self.assertEqual(runtime.held_exemption(detail).nonheld_recovering, 1)
        self.assertEqual(runtime.held_exemption(
            detail.replace("status=2", "status=1")).nonheld_recovering, 0)
        self.assertEqual(runtime.held_exemption("\n".join((
            published, detail, detail.replace("tick=1", "tick=2"), published, detail))).nonheld, 1)
        self.assertEqual(runtime.held_exemption(
            published.replace("active=1", "active=0") + "\n" + detail).nonheld, 1)
        for trace in (published, detail + "\n" + detail,
                      detail + "\n" + published + "\n" + published,
                      detail + "\n" + published.replace("slot=0", "slot=1"),
                      detail + "\n" + published.replace("active=1", "active=2"),
                      detail + "\n" + published.replace(" dwell=0.5000 divergence=12.000", "")):
            with self.subTest(trace=trace), self.assertRaises(RuntimeError):
                runtime.held_exemption(trace)
        for old, new in (("status=2", "status=0"), ("valid=1", "valid=0"),
                         ("corrected=1", "corrected=0"), ("alternate=1", "alternate=0"),
                         ("clearance=0", "clearance=1"), ("source_degraded=0", "source_degraded=1")):
            with self.subTest(field=old), self.assertRaises(RuntimeError):
                runtime.held_exemption(detail.replace(old, new) + "\n" + published)

    def test_held_exemption_is_bounded(self):
        detail = ("camera_obstruction_observe detail tick={t} physical_slot=0 "
                  "resolve={{status=2 valid=1 corrected=1 alternate=1 clearance=0 source_degraded=0}}")
        published = ("camera_held_shot_published tick={t} slot=0 frame=7 active=1 "
                     "dwell={d} divergence={v}")
        def trace(*pairs):
            return "\n".join(f"{published.format(t=t, d=d, v=v)}\n{detail.format(t=t)}"
                             for t, d, v in pairs)
        bound_dwell = motion.MAX_HELD_DWELL_SECONDS
        bound_divergence = motion.MAX_HELD_DIVERGENCE_DEGREES
        # Two episodes (ticks 1-2 and 4) inside the bounds.
        self.assertEqual(runtime.held_exemption(trace((1, 0.1, 1.0), (2, bound_dwell, bound_divergence),
                                                      (4, 0.1, 1.0))),
                         runtime.HeldExemption(nonheld=0, publications=3, episodes=2))
        for pair, reason in (((1, bound_dwell + 0.1, 1.0), "dwell"),
                             ((1, 0.1, bound_divergence + 1.0), "divergence")):
            with self.subTest(reason=reason), self.assertRaisesRegex(RuntimeError, "bounds"):
                runtime.held_exemption(trace(pair))

    def test_comfort_guard_bounds_held_inflation(self):
        modern = runtime.HeldExemption(nonheld=100, publications=50, episodes=5,
                                       nonheld_recovering=60)
        # Retraction at 2x and recovery at 3x are both inside the bounds.
        self.assertEqual(runtime.comfort_inflation_failures(
            runtime.HeldExemption(nonheld=80 + 180, publications=150, episodes=10,
                                  nonheld_recovering=180), modern), [])
        for comfort, reason in (
                (runtime.HeldExemption(nonheld=81 + 60, publications=50, episodes=5,
                                       nonheld_recovering=60), "non-recovery"),
                (runtime.HeldExemption(nonheld=40 + 181, publications=50, episodes=5,
                                       nonheld_recovering=181), "recovering"),
                (runtime.HeldExemption(nonheld=100, publications=50, episodes=11,
                                       nonheld_recovering=60), "held shots"),
                (runtime.HeldExemption(nonheld=100, publications=151, episodes=5,
                                       nonheld_recovering=60), "held ticks")):
            with self.subTest(reason=reason):
                failures = runtime.comfort_inflation_failures(comfort, modern)
                self.assertEqual(len(failures), 1)
                self.assertIn(reason, failures[0])

    def test_cut_census_must_measure_every_measurable_cut(self):
        def census(adjacent=90, unadjacent=6, discontinuities=96, cut_ticks=90,
                   pos=90, ang=90):
            summary = {"cut_adjacent": adjacent, "cut_unadjacent": unadjacent,
                       "discontinuities": discontinuities, "cut_ticks": cut_ticks}
            stats = {"cut_pos_step": {"unit": "wu/cut", "samples": pos},
                     "cut_ang_step": {"unit": "deg/cut", "samples": ang}}
            return summary, stats
        self.assertEqual(motion.cut_census_failures("route", *census()), [])
        self.assertEqual(motion.cut_census_failures(
            "route", *census(adjacent=0, unadjacent=0, discontinuities=0,
                             cut_ticks=0, pos=0, ang=0)), [])
        for kwargs, reason in (
                # The recorder that silently never fires: a cut-heavy route
                # whose steps all vanished from the report.
                (dict(pos=0, ang=0), "sampled 0 of the 90"),
                (dict(ang=89), "cut_ang_step sampled 89"),
                (dict(unadjacent=5), "splits 90+5"),
                (dict(adjacent=0, unadjacent=96, pos=0, ang=0), "none with a measurable step")):
            with self.subTest(reason=reason):
                failures = motion.cut_census_failures("route", *census(**kwargs))
                self.assertTrue(any(reason in failure for failure in failures), failures)
        summary, stats = census()
        del stats["cut_pos_step"]
        self.assertIn("route: missing or invalid cut_pos_step census",
                      motion.cut_census_failures("route", summary, stats))

    def test_held_bound_failures_measure_the_leash(self):
        def summary(**overrides):
            values = {"held_dwell_bound_ms": 2000.0, "held_divergence_bound_deg": 60.0,
                      "held_leash_bound_deg": 45.0, "max_held_dwell_ms": 1100.0,
                      "max_held_divergence_deg": 45.02, "held_bound_handovers": 0,
                      "held_bound_handovers_unflagged": 0, "held_leash_ticks": 24,
                      "held_leash_unreduced": 0}
            values.update(overrides)
            return values
        self.assertEqual(motion.held_bound_failures("route", summary()), [])
        for overrides, reason in (
                # A leash that plans its turn and never applies it.
                (dict(held_leash_unreduced=24), "24 of 24 leashed held views did not turn"),
                (dict(held_bound_handovers=1, held_bound_handovers_unflagged=1),
                 "not published as a cut"),
                (dict(max_held_divergence_deg=60.5), "turned 60.50 degrees"),
                (dict(held_leash_bound_deg=60.0), "not the shipped")):
            with self.subTest(reason=reason):
                failures = motion.held_bound_failures("route", summary(**overrides))
                self.assertTrue(any(reason in failure for failure in failures), failures)

    def test_only_exact_event_rows_are_retained(self):
        event = "camera_motion reengagement tick=42 viewport=0 gap=11"
        output = ("camera_motion summary slot_ticks=100\n"
                  f"prefix {event}\n{event}\n"
                  "camera_motion detail tick=42\n")
        self.assertEqual(motion.reengagement_evidence(output), [event])

    def test_report_keeps_arm_identity_and_event_context(self):
        output = "camera_motion reengagement tick=42 viewport=1 gap=11\n"
        captured = io.StringIO()
        with contextlib.redirect_stdout(captured):
            motion.report_reengagements("route@240", output, 1)
        report = captured.getvalue()
        self.assertIn("[route@240]", report)
        self.assertIn("1 event rows, 1 census events", report)
        self.assertIn(output.strip(), report)
        self.assertNotIn("incomplete", report)

    def test_missing_or_extra_context_does_not_claim_complete_evidence(self):
        for output, count in (("", 1),
                              ("camera_motion reengagement tick=42\n", 0)):
            with self.subTest(output=output, count=count):
                captured = io.StringIO()
                with contextlib.redirect_stdout(captured):
                    motion.report_reengagements("route", output, count)
                self.assertIn("aggregate hard verdict is unchanged",
                              captured.getvalue())

    def test_console_is_bounded_without_dropping_retained_rows(self):
        output = "\n".join(f"camera_motion reengagement tick={tick}"
                           for tick in range(20))
        self.assertEqual(len(motion.reengagement_evidence(output)), 20)
        captured = io.StringIO()
        with contextlib.redirect_stdout(captured):
            motion.report_reengagements("route", output, 20)
        self.assertEqual(captured.getvalue().count("camera_motion reengagement"), 16)
        self.assertIn("4 additional rows omitted from console", captured.getvalue())
        self.assertIn("--baseline-out", captured.getvalue())

    def test_empty_clean_run_adds_no_failure_context(self):
        captured = io.StringIO()
        with contextlib.redirect_stdout(captured):
            motion.report_reengagements("route", "", 0)
        self.assertEqual(captured.getvalue(), "")

    def test_failed_authored_and_high_rate_arms_keep_separate_evidence(self):
        arm = motion.HIGH_RATE_ARMS[0]
        authored = "camera_motion reengagement tick=42 viewport=0 gap=11"
        high_rate = "camera_motion reengagement tick=84 viewport=1 gap=10"
        summary = {"correction_reengagements": 1}
        captured = io.StringIO()
        # Exercise main's real report/serialization path, replacing only the
        # process/measurement boundaries. No product or ROM process is launched.
        with mock.patch("sys.argv", ["check_camera_motion_quality.py",
                                     "--only", arm.route,
                                     "--baseline-out", "unused.json"]), \
                mock.patch.object(motion, "resolve_binary", return_value="unused-game"), \
                mock.patch.object(motion.Path, "is_file", return_value=True), \
                mock.patch.object(motion.Path, "write_text") as write, \
                mock.patch.object(motion, "run_route", side_effect=[authored, high_rate]), \
                mock.patch.object(motion, "inspect",
                                  return_value=(summary, {}, ["authored hard failure"])), \
                mock.patch.object(motion, "inspect_high_rate",
                                  return_value=({"summary": summary},
                                                ["high-rate hard failure"])), \
                mock.patch.object(motion, "report_baseline"), \
                contextlib.redirect_stdout(captured), \
                contextlib.redirect_stderr(captured):
            self.assertEqual(motion.main(), 1)
        saved = json.loads(write.call_args.args[0])
        self.assertEqual(saved["routes"][arm.route]["reengagement_events"], [authored])
        label = f"{arm.route}@{arm.present_rate}"
        self.assertEqual(saved["high_rate"][label]["reengagement_events"], [high_rate])
        report = captured.getvalue()
        self.assertIn(f"[{label}] reengagement evidence", report)
        self.assertIn("authored hard failure", report)
        self.assertIn("high-rate hard failure", report)


if __name__ == "__main__":
    unittest.main()
