#!/usr/bin/env python3
"""ROM-free pacing integrity and arm-owned failure reporting; launches no app."""

import ast
import contextlib
from dataclasses import replace
import inspect
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import check_pacing_quality as pacing


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
                                                  "explain_unthrottled_presentation"}
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


if __name__ == "__main__":
    unittest.main()
