#!/usr/bin/env python3
"""ROM-free pacing integrity and arm-owned failure reporting; launches no app."""

import ast
import contextlib
from dataclasses import replace
import inspect
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import check_pacing_quality as pacing
import harness_utils


def sample_run(label: str, anchors: int) -> pacing.Run:
    return pacing.Run(
        label=label,
        output=("[PRESENT-DISCIPLINE] active=1\n"
                "[PRESENT-MODE] backend=webgpu policy=display "
                "effective=fifo tearing=0\n"),
        summary={"slotanchors": anchors},
        hist={
            "displayed-interval": {"p50": 16000, "p95": 18000, "p99": 21000,
                                   "max": 23000, "var": 1000000, "n": 1795},
            "alpha-delta": {"p50": 500000, "p95": 500000, "p99": 1000000,
                            "var": 2000000, "gridppm": 500000,
                            "displayed": 1795, "stalls": 0, "regressions": 0,
                            "binwidth": 1000},
        },
        latency={"meandepthmilli": 1000, "maxdepth": 1,
                 "meanlatencyus": 16666, "maxlatencyus": 16666, "refreshhz": 60},
        audio={}, pressure={}, arm="display/smoothing/realtime",
    )


class PacingReportingTests(unittest.TestCase):
    def test_attempt_evidence_is_labelled_and_never_overwrites(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for label, count in (("strict", 0), ("companion-retry2", 43)):
                attempt = root / label
                attempt.mkdir()
                output = f"{label}: slotanchors={count}\n"
                pacing.write_arm_evidence(attempt, label, output, 0, False)
                self.assertEqual((attempt / "process.log").read_text(), output)
                record = json.loads((attempt / "process.json").read_text())
                self.assertEqual(record, {
                    "schema": "mdkr-pacing-process/1", "label": label,
                    "returncode": 0, "timed_out": False,
                })
                with self.assertRaises(FileExistsError):
                    pacing.write_arm_evidence(attempt, label, "replacement", 1, False)
                self.assertEqual((attempt / "process.log").read_text(), output)

    def test_failed_process_and_parse_failure_keep_full_output(self):
        for returncode, expected_error in ((7, "exit 7"), (0, "emitted no")):
            with self.subTest(returncode=returncode), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                output = "early evidence\n" + "details\n" * 600 + "last evidence\n"
                completed = subprocess.CompletedProcess(["unused"], returncode, output)
                with mock.patch.object(pacing.subprocess, "run", return_value=completed):
                    with self.assertRaisesRegex(RuntimeError, expected_error):
                        pacing.run_arm(Path("unused"), Path("unused-rom"), root,
                                       "companion", "display", "interpolate", 900, 5, False)
                self.assertEqual((root / "companion/process.log").read_text(), output)
                record = json.loads((root / "companion/process.json").read_text())
                self.assertEqual(record["returncode"], returncode)
                self.assertFalse(record["timed_out"])

    def test_successful_attempt_is_retained_before_normal_parsing(self):
        output = (
            "[PRESENTSCHED-SUMMARY] slotanchors=43\n"
            "[PRESENTPERF-LATENCY] periodus=16666\n"
            "[PRESENTPERF-HIST] series=present-interval arm=display/smoothing/realtime n=2\n"
            "[PRESENTPERF-HIST] series=displayed-interval arm=display/smoothing/realtime n=2\n"
            "[PRESENTPERF-HIST] series=alpha-delta arm=display/smoothing/realtime n=2\n"
        )
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            completed = subprocess.CompletedProcess(["unused"], 0, output)
            with mock.patch.object(pacing.subprocess, "run", return_value=completed):
                result = pacing.run_arm(Path("unused"), Path("unused-rom"), root,
                                        "companion", "display", "interpolate", 900, 5, False,
                                        realtime=True)
            self.assertEqual(result.output, output)
            self.assertEqual(result.summary["slotanchors"], 43)
            self.assertEqual(result.arm, "display/smoothing/realtime")
            self.assertEqual((root / "companion/process.log").read_text(), output)

    def test_timeout_retains_partial_bytes_and_is_a_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            error = subprocess.TimeoutExpired(["unused"], 5, output=b"partial\n\xff")
            with mock.patch.object(pacing.subprocess, "run", side_effect=error):
                with self.assertRaisesRegex(RuntimeError, "companion: timed out after 5s"):
                    pacing.run_arm(Path("unused"), Path("unused-rom"), root,
                                   "companion", "display", "interpolate", 900, 5, False)
            self.assertEqual((root / "companion/process.log").read_text(), "partial\n\ufffd")
            record = json.loads((root / "companion/process.json").read_text())
            self.assertIsNone(record["returncode"])
            self.assertTrue(record["timed_out"])

    def test_spawn_failure_is_recorded_without_an_exit_status(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with mock.patch.object(pacing.subprocess, "run", side_effect=OSError("could not spawn")):
                with self.assertRaisesRegex(RuntimeError, "process could not start"):
                    pacing.run_arm(Path("unused"), Path("unused-rom"), root,
                                   "companion", "display", "interpolate", 900, 5, False)
            self.assertEqual((root / "companion/process.log").read_text(), "could not spawn")
            record = json.loads((root / "companion/process.json").read_text())
            self.assertIsNone(record["returncode"])
            self.assertFalse(record["timed_out"])

    def test_realtime_integrity_accepts_muted_and_nonstarved_runs(self):
        result = sample_run("companion", 0)
        self.assertEqual(pacing.check_realtime_integrity(result), [])
        result = replace(result, audio={"underruns": 0})
        self.assertEqual(pacing.check_realtime_integrity(result), [])

    def test_realtime_integrity_rejects_wrong_arm_and_missing_counters(self):
        result = sample_run("companion", 0)
        result = replace(result, arm="display/smoothing/synthetic", audio={"samples": 1})
        del result.hist["alpha-delta"]["regressions"]
        failures = pacing.check_realtime_integrity(result)
        self.assertEqual(len(failures), 3)
        for expected in ("not a realtime run", "underruns must be 0",
                         "regression counter is missing"):
            self.assertTrue(any(expected in failure for failure in failures))
        self.assertTrue(all(failure.startswith("companion:") for failure in failures))

    def test_realtime_integrity_rejects_regression_underrun_and_tearing(self):
        result = sample_run("companion", 0)
        result.hist["alpha-delta"]["regressions"] = 1
        result = replace(result, audio={"underruns": 1},
                         output=result.output.replace("tearing=0", "tearing=1"))
        failures = pacing.check_realtime_integrity(result)
        self.assertEqual(len(failures), 3)
        for expected in ("ran backwards", "audio sink starved 1", "tearing=1"):
            self.assertTrue(any(expected in failure for failure in failures))

    def test_realtime_integrity_requires_presentation_mode_evidence(self):
        result = sample_run("companion", 0)
        result = replace(result, output="[PRESENT-DISCIPLINE] active=1\n")
        failures = pacing.check_realtime_integrity(result)
        self.assertEqual(len(failures), 1)
        self.assertIn("no [PRESENT-MODE] row", failures[0])

    def test_both_realtime_loops_preserve_integrity_before_baseline_exemptions(self):
        tree = ast.parse(inspect.getsource(pacing.main))
        realtime_loops = []
        for loop in (node for node in ast.walk(tree) if isinstance(node, ast.For)):
            # Examine direct statements only: do not count an enclosing loop
            # again just because it contains these two realtime retry loops.
            launches = [statement for statement in loop.body
                        if isinstance(statement, ast.Assign) and
                        isinstance(statement.value, ast.Call) and
                        isinstance(statement.value.func, ast.Name) and
                        statement.value.func.id == "run_arm" and
                        any(keyword.arg == "realtime" and
                            isinstance(keyword.value, ast.Constant) and
                            keyword.value.value is True
                            for keyword in statement.value.keywords)]
            if not launches:
                continue
            self.assertEqual(len(launches), 1)
            result_name = launches[0].targets[0].id
            realtime_loops.append(result_name)
            integrity = [statement for statement in loop.body
                         if isinstance(statement, ast.Expr) and
                         ast.dump(statement.value) == ast.dump(ast.parse(
                             f"failures.extend(check_realtime_integrity({result_name}))"
                         ).body[0].value)]
            self.assertEqual(len(integrity), 1)
            self.assertLess(loop.body.index(launches[0]), loop.body.index(integrity[0]))
            exemptions = [statement for statement in loop.body
                          if any(isinstance(node, ast.Call) and
                                 isinstance(node.func, ast.Name) and
                                 node.func.id in {"explain_no_display_session",
                                                  "explain_unthrottled_presentation",
                                                  "classify_attempt"}
                                 for node in ast.walk(statement))]
            self.assertTrue(exemptions)
            for statement in exemptions:
                self.assertLess(loop.body.index(integrity[0]), loop.body.index(statement))
        self.assertCountEqual(realtime_loops, ["result", "honest_result"])

    def test_slot_failure_keeps_its_own_distribution_and_counter(self):
        strict = sample_run("strict-attempt", 0)
        companion = sample_run("companion-retry2", 43)
        failures = pacing.check_slot_quality(companion)
        self.assertEqual(len(failures), 1)
        self.assertIn("43 slot re-anchors exceed 3", failures[0])
        self.assertEqual(pacing.check_slot_quality(strict), [])
        measured = pacing.baseline_note(companion)
        for expected in ("companion-retry2", "p95=500000ppm", "grid=500000ppm",
                         "displayed=1795", "stalls=0", "regressions=0",
                         "slotanchors=43"):
            self.assertIn(expected, measured)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = pacing.report_failure(
                failures, [pacing.baseline_note(strict)],
                [f"slot measurement: {measured}", "retry classifier evidence"])
        self.assertEqual(status, 1)
        self.assertIn("check_pacing_quality: FAIL", output.getvalue())
        self.assertNotIn("PASS", output.getvalue())
        self.assertIn("measured: strict-attempt", output.getvalue())
        self.assertIn("slot measurement: companion-retry2", output.getvalue())
        self.assertIn("slotanchors=43", output.getvalue())
        self.assertIn("retry classifier evidence", output.getvalue())

    def test_main_routes_companion_evidence_without_promoting_strict_baseline(self):
        calls = [node for node in ast.walk(ast.parse(inspect.getsource(pacing.main)))
                 if isinstance(node, ast.Call)]
        destinations = []
        for call in calls:
            if not (isinstance(call.func, ast.Attribute) and
                    call.func.attr == "append" and
                    isinstance(call.func.value, ast.Name)):
                continue
            if any(isinstance(node, ast.Call) and
                   isinstance(node.func, ast.Name) and node.func.id == "baseline_note" and
                   len(node.args) == 1 and isinstance(node.args[0], ast.Name) and
                   node.args[0].id == "honest_result" for node in ast.walk(call)):
                destinations.append(call.func.value.id)
        self.assertEqual(destinations, ["notes"])
        reports = [call for call in calls if isinstance(call.func, ast.Name) and
                   call.func.id == "report_failure"]
        self.assertEqual(len(reports), 1)
        self.assertEqual([arg.id for arg in reports[0].args if isinstance(arg, ast.Name)],
                         ["failures", "baselines", "notes"])

    def test_an_arm_isolates_prefs_mods_and_characters(self):
        """run_arm drops every inherited MDKR* variable, so the pins a suite
        task exported never reach the process. The arm has to set them itself,
        under its private run directory, or the launch writes the owner's
        preferences and can read a mods folder beside some other cwd."""
        captured = {}

        def stop(command, **kwargs):
            captured["env"] = kwargs["env"]
            raise OSError("stop before launch")

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            binary, rom = root / "mdkr64", root / "rom.z64"
            binary.touch()
            rom.touch()
            with mock.patch.object(pacing.subprocess, "run", side_effect=stop):
                with self.assertRaises(RuntimeError) as raised:
                    pacing.run_arm(binary, rom, root, "realtime-display-smoothing",
                                   "display", "interpolate", 10, 5, False,
                                   realtime=True)
                self.assertIn("process could not start", str(raised.exception))
                env = captured["env"]
                run_dir = root / "realtime-display-smoothing"
                for name in ("MDKR_SAVE_DIR", "MDKR_VIDEO_CONFIG_PATH",
                             "MDKR_APP_PREFS_DIR", "MDKR_MODS_DIR",
                             "MDKR_CUSTOM_CHARACTER_DIRECTORY"):
                    self.assertIn(name, env)
                    self.assertTrue(Path(env[name]).is_relative_to(run_dir), name)
                self.assertFalse((run_dir / "mods").exists())
                self.assertFalse((run_dir / "characters").exists())
                self.assertNotIn("MDKR_TEST_FOREGROUND", env)
                with mock.patch.dict(os.environ, {"MDKR_TEST_FOREGROUND": "1"}):
                    with self.assertRaises(RuntimeError):
                        pacing.run_arm(binary, rom, root,
                                       "realtime-display-smoothing-front",
                                       "display", "interpolate", 10, 5, False,
                                       realtime=True)
                self.assertEqual(captured["env"].get("MDKR_TEST_FOREGROUND"), "1")

    def test_two_slot_boundary_fallback_is_the_end_of_tick_phase(self):
        # 60 Hz display, 30 Hz tick: two grid points. The legacy quantizer
        # returns the raw phase when rounding would cross the tick, and that
        # bin sits just below one tick (measured p95 upper edge 958464,
        # binwidth 4096). The midpoint p50 stays on the grid.
        result = sample_run("strict", 0)
        alpha = result.hist["alpha-delta"]
        alpha["p50"] = 503808
        alpha["p95"] = 958464
        alpha["gridppm"] = 500000
        alpha["binwidth"] = 4096
        self.assertEqual(pacing.check_vblank_projection(result), [])

    def test_a_mid_gap_phase_still_fails_on_a_two_slot_grid(self):
        result = sample_run("strict", 0)
        alpha = result.hist["alpha-delta"]
        alpha["p50"] = 503808
        alpha["p95"] = 700416
        alpha["gridppm"] = 500000
        alpha["binwidth"] = 4096
        problems = pacing.check_vblank_projection(result)
        self.assertTrue(any("p95=700416ppm" in item for item in problems),
                        problems)

    def test_a_four_slot_grid_still_rejects_an_off_grid_p95(self):
        # 120 Hz: four slots. An end-of-tick-looking bin is not a free pass.
        result = sample_run("strict", 0)
        alpha = result.hist["alpha-delta"]
        alpha["p50"] = 253952
        alpha["p95"] = 958464
        alpha["gridppm"] = 250000
        alpha["binwidth"] = 4096
        problems = pacing.check_vblank_projection(result)
        self.assertTrue(any("p95=958464ppm" in item for item in problems),
                        problems)

    def test_two_slot_midpoint_p50_stays_on_the_grid(self):
        # The end-of-tick allowance is the tail only. A median that has left
        # the midpoint is the projection failing, even when p95 is the raw
        # phase the quantizer is allowed to emit.
        result = sample_run("strict", 0)
        alpha = result.hist["alpha-delta"]
        alpha["p50"] = 700416
        alpha["p95"] = 958464
        alpha["gridppm"] = 500000
        alpha["binwidth"] = 4096
        problems = pacing.check_vblank_projection(result)
        self.assertTrue(any("p50=700416ppm" in item for item in problems),
                        problems)


# The rows a locked console produced (task T12c, 2026-10-02, console locked
# from 00:30): presents complete, nothing consumes them, so the drawable acquire
# never blocks -- p50 and p95 both in the first 100 us bin -- and the pacer
# free-runs. Its slot re-anchors (10-98 per attempt) then read as a pacing
# failure. An unlocked run of the same arm on the same machine reports
# blockp95us=2200-2400.
LOCKED_DISCIPLINE = ("[PRESENT-DISCIPLINE] active=1 samples=3592 blockp50us=100 "
                     "blockp95us=100 blockmaxus=180 unavailable=0 slewtotalus=0 "
                     "periodns=8333333 expiries=0 leads=0 leadmissmeanus=0 "
                     "leadmissmaxus=0\n")
UNLOCKED_DISCIPLINE = ("[PRESENT-DISCIPLINE] active=1 samples=3592 blockp50us=100 "
                       "blockp95us=2200 blockmaxus=10597 unavailable=0 "
                       "slewtotalus=129936 periodns=8333333 expiries=0 leads=639 "
                       "leadmissmeanus=759 leadmissmaxus=14559\n")
LOCKED_IOREG = ('    | "IOConsoleUsers" = ({"kCGSSessionOnConsoleKey"=Yes,'
                '"CGSSessionScreenIsLocked"=Yes,"kCGSessionLoginDoneKey"=Yes})\n')
UNLOCKED_IOREG = ('    | "IOConsoleUsers" = ({"kCGSSessionOnConsoleKey"=Yes,'
                  '"kCGSessionLoginDoneKey"=Yes})\n')


class LockedConsoleTests(unittest.TestCase):
    def test_a_locked_console_is_named_not_measured(self):
        locked = replace(sample_run("realtime", 89),
                         output=sample_run("realtime", 89).output + LOCKED_DISCIPLINE)
        cause = pacing.explain_no_display_session(locked)
        self.assertIsNotNone(cause)
        self.assertIn("never blocked", cause)
        unlocked = replace(sample_run("realtime", 0),
                           output=sample_run("realtime", 0).output + UNLOCKED_DISCIPLINE)
        self.assertIsNone(pacing.explain_no_display_session(unlocked))

    def test_too_few_acquires_prove_nothing(self):
        short = LOCKED_DISCIPLINE.replace("samples=3592", "samples=40")
        run = replace(sample_run("realtime", 0), output=sample_run("realtime", 0).output + short)
        self.assertIsNone(pacing.explain_no_display_session(run))

    def test_the_console_session_is_read_from_ioreg(self):
        import harness_utils
        self.assertEqual(harness_utils.console_lock_reason(LOCKED_IOREG),
                         "the console is locked")
        self.assertIsNone(harness_utils.console_lock_reason(UNLOCKED_IOREG))
        self.assertEqual(harness_utils.console_lock_reason("no session"),
                         "no user session is on the console")

    def test_a_host_that_cannot_be_judged_is_a_loud_skip(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = pacing.report_skip("the console is locked", ["synthetic arms passed"])
        self.assertEqual(code, pacing.SKIP_EXIT)
        self.assertIn("SKIPPED", out.getvalue())
        self.assertIn("the console is locked", out.getvalue())
        self.assertNotIn("PASS", out.getvalue())


class RealtimeVerdictTests(unittest.TestCase):
    """main()'s combined verdict over both realtime arms.

    The process launches and the per-attempt measurements are scripted; the
    arm loops, the attempt classification and the final verdict are main()'s
    own. Each attempt is one of: "paced" (a consumer, inside its bounds),
    "bounds" (a consumer, outside its bounds), "blocked-never" (the acquire
    never waited), while the console reads as scripted for that attempt.
    """

    UNLOCKED = (harness_utils.CONSOLE_UNLOCKED, "console unlocked")
    LOCKED = (harness_utils.CONSOLE_UNAVAILABLE, "the console is locked")

    def run_main(self, strict, honest, consoles, extra_args=()):
        """strict/honest: one kind per attempt; consoles: the console state
        read before the arms, after each attempt (strict then honest), and
        after the arms. extra_args is appended to the argv main() parses,
        so a test can pass the same flags the suite does."""
        plan = {"realtime-display-smoothing": strict,
                "realtime-display-smoothing-quantum-honest": honest}
        console = iter(consoles)

        def next_state():
            return next(console)

        def next_unavailable():  # the older single-answer reader
            state, reason = next_state()
            return reason if state == harness_utils.CONSOLE_UNAVAILABLE else None

        def fake_run_arm(binary, rom, root, label, policy, smoothing, ticks,
                         timeout, verbose, realtime=False, extra_env=None):
            if not realtime:
                kind = next(k for _, value, k in pacing.POLICIES if value == policy)
                name = next(n for n, value, _ in pacing.POLICIES if value == policy)
                smooth = next(n for n, value in pacing.SMOOTHINGS if value == smoothing)
                arm = f"{'capped' if kind == pacing.KIND_CAPPED else name}/{smooth}/synth"
                return replace(sample_run(label, 0), arm=arm)
            base, _, retry = label.partition("-retry")
            outcome = plan[base][int(retry) if retry else 0]
            run = sample_run(label, 0)
            row = LOCKED_DISCIPLINE if outcome == "blocked-never" else UNLOCKED_DISCIPLINE
            return replace(run, output=run.output + row,
                           summary={"slotanchors": 9 if outcome == "bounds" else 0})

        def quality(result):
            return ([f"{result.label}: outside the bounds"]
                    if result.summary["slotanchors"] else [])

        with tempfile.TemporaryDirectory() as temp:
            binary, rom = Path(temp) / "mdkr64", Path(temp) / "rom.z64"
            binary.touch()
            rom.touch()
            argv = ["check_pacing_quality.py", "--build", str(binary),
                    "--rom", str(rom), *extra_args]
            output = io.StringIO()
            with mock.patch.object(pacing.sys, "argv", argv), \
                    mock.patch.object(pacing, "run_arm", side_effect=fake_run_arm), \
                    mock.patch.object(pacing, "run_display_switch_arm",
                                      return_value=(sample_run("switch", 0), 120)), \
                    mock.patch.object(pacing, "check_plumbing", return_value=[]), \
                    mock.patch.object(pacing, "check_display_change", return_value=[]), \
                    mock.patch.object(pacing, "check_display_margin_change", return_value=[]), \
                    mock.patch.object(pacing, "check_alpha_quantum_honest", return_value=[]), \
                    mock.patch.object(pacing, "check_realtime_quality", side_effect=quality), \
                    mock.patch.object(pacing, "check_slot_quality", side_effect=quality), \
                    mock.patch.object(pacing, "console_state", next_state, create=True), \
                    mock.patch.object(pacing, "console_unavailable", next_unavailable,
                                      create=True), \
                    contextlib.redirect_stdout(output):
                code = pacing.main()
        return code, output.getvalue()

    def test_both_arms_paced_and_in_bounds_pass(self):
        code, text = self.run_main(["paced"], ["paced"], [self.UNLOCKED] * 4)
        self.assertEqual(code, 0, text)
        self.assertIn("check_pacing_quality: PASS --", text)

    def test_one_arm_refused_while_the_other_fails_its_bounds_is_a_failure(self):
        # The reviewer's case: on an unlocked console the strict arm trips the
        # never-blocked heuristic and the honest arm paces and fails.
        code, text = self.run_main(["blocked-never"] * 3, ["bounds"] * 3,
                                   [self.UNLOCKED] * 8)
        self.assertEqual(code, 1, text)
        self.assertIn("outside the bounds", text)
        self.assertNotIn("SKIPPED", text)
        # And when the strict arm's attempts saw a locked console.
        code, text = self.run_main(["blocked-never"] * 3, ["bounds"] * 3,
                                   [self.UNLOCKED] + [self.LOCKED] * 3 +
                                   [self.UNLOCKED] * 4)
        self.assertEqual(code, 1, text)
        self.assertIn("outside the bounds", text)
        self.assertNotIn("SKIPPED", text)
        # The suite flag does not excuse an arm that actually paced and missed.
        code, text = self.run_main(["blocked-never"] * 3, ["bounds"] * 3,
                                   [self.UNLOCKED] * 8,
                                   extra_args=("--allow-no-baseline",))
        self.assertEqual(code, 1, text)
        self.assertIn("outside the bounds", text)
        self.assertNotIn("REALTIME QUALITY NOT MEASURED", text)

    def test_both_arms_refused_on_a_locked_console_skip(self):
        code, text = self.run_main(["blocked-never"] * 3, ["blocked-never"] * 3,
                                   [self.UNLOCKED] + [self.LOCKED] * 7)
        self.assertEqual(code, pacing.SKIP_EXIT, text)
        self.assertIn("SKIPPED", text)
        self.assertNotIn("FAIL", text)

    def test_never_blocking_on_an_unlocked_console_is_a_failure(self):
        code, text = self.run_main(["blocked-never"] * 3, ["blocked-never"] * 3,
                                   [self.UNLOCKED] * 8)
        self.assertEqual(code, 1, text)
        self.assertIn("no consumer on an unlocked console", text)
        self.assertNotIn("SKIPPED", text)

    def test_allow_no_baseline_notes_an_unlocked_never_block(self):
        # The suite always passes this flag. An occluded automation window
        # can lack a consumer on a correct build, so that case is a loud
        # note, not a failure. The prepared-machine run omits the flag and
        # still fails (the test above).
        code, text = self.run_main(["blocked-never"] * 3, ["blocked-never"] * 3,
                                   [self.UNLOCKED] * 8,
                                   extra_args=("--allow-no-baseline",))
        self.assertEqual(code, 0, text)
        self.assertIn("REALTIME QUALITY NOT MEASURED", text)
        self.assertIn("no consumer on an unlocked console", text)
        self.assertNotIn("SKIPPED", text)

    def test_allow_no_baseline_still_downgrades_a_locked_or_unreadable_host(self):
        # A locked or unreadable host is the same loud note.
        for consoles in (
            [self.UNLOCKED] + [self.LOCKED] * 7,
            [self.UNLOCKED] + [(harness_utils.CONSOLE_UNKNOWN,
                                "ioreg unavailable")] * 7,
        ):
            code, text = self.run_main(
                ["blocked-never"] * 3, ["blocked-never"] * 3, consoles,
                extra_args=("--allow-no-baseline",))
            self.assertEqual(code, 0, text)
            self.assertIn("REALTIME QUALITY NOT MEASURED", text)
            self.assertNotIn("check_pacing_quality: FAIL", text)

    def test_never_blocking_with_an_unreadable_session_is_not_a_pass(self):
        unknown = (harness_utils.CONSOLE_UNKNOWN, "ioreg unavailable")
        code, text = self.run_main(["blocked-never"] * 3, ["blocked-never"] * 3,
                                   [self.UNLOCKED] + [unknown] * 7)
        self.assertEqual(code, pacing.SKIP_EXIT, text)

    def test_a_lock_after_a_paced_failure_keeps_the_failure(self):
        code, text = self.run_main(["bounds", "blocked-never", "blocked-never"],
                                   ["blocked-never"] * 3,
                                   [self.UNLOCKED, self.UNLOCKED] + [self.LOCKED] * 6)
        self.assertEqual(code, 1, text)
        self.assertIn("realtime-display-smoothing: outside the bounds", text)
        self.assertNotIn("SKIPPED", text)

    def test_a_console_locked_before_the_arms_skips_them(self):
        code, text = self.run_main([], [], [self.LOCKED])
        self.assertEqual(code, pacing.SKIP_EXIT, text)


if __name__ == "__main__":
    unittest.main()
