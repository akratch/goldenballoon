#!/usr/bin/env python3
"""ROM-free schema and runner refusal controls for the RDRAM snapshot lane.

The Spaceport Alpha ring routes (tools/oracle_routes/spaceport_pulse_ring*.json)
capture console RDRAM through the instrumented ares. Nothing here runs ares:
these are the guards that refuse a capture which could not mean what its
caller asked for.
"""
import contextlib
import copy
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import dkr_oracle_route as route_tool

ROUTES = ("spaceport_pulse_ring", "spaceport_pulse_ring_tt", "spaceport_pulse_ring_second_race")


def route_errors(route) -> str:
    """The errors validate_route prints before it exits; '' when valid."""
    printed = io.StringIO()
    try:
        with contextlib.redirect_stdout(printed):
            route_tool.validate_route(route)
    except SystemExit:
        return printed.getvalue() or "invalid"
    return ""


class RdramRoutes(unittest.TestCase):
    def test_routes_declare_the_capture_and_the_forced_race(self):
        for name in ROUTES:
            with self.subTest(route=name):
                route = route_tool.load_route(name)
                self.assertEqual(route_errors(route), "")
                self.assertEqual(route_tool.route_field(route, "rdram_capture"), 1)
                self.assertEqual(route_tool.route_field(route, "compare_frames"), 0)
                self.assertEqual(route_tool.route_field(route, "forced_track"), 15)
        second = route_tool.load_route("spaceport_pulse_ring_second_race")
        self.assertEqual(route_tool.route_field(second, "forced_track_occurrence"), 2)

    def test_schema_refuses_bad_occurrence_and_capture_values(self):
        base = route_tool.load_route("spaceport_pulse_ring")
        for key, value, expected in (
                ("forced_track_occurrence", -1, "forced_track_occurrence must be 0"),
                ("forced_track_occurrence", "2", "forced_track_occurrence must be an integer"),
                ("rdram_capture", 1, "rdram_capture must be a boolean")):
            with self.subTest(key=key, value=value):
                invalid = copy.deepcopy(base)
                invalid[key] = value
                self.assertIn(expected, route_errors(invalid))
        invalid = copy.deepcopy(base)
        invalid.pop("forced_track")
        self.assertIn("forced_track_occurrence requires forced_track",
                      route_errors(dict(invalid, forced_track_occurrence=2)))
        invalid = copy.deepcopy(base)
        invalid["rdram_capture"] = False
        self.assertIn("must enable", route_errors(invalid))

    def test_runner_refuses_mismatched_requests(self):
        with tempfile.TemporaryDirectory() as tmp:
            rom = Path(tmp) / "placeholder.z64"
            rom.touch()  # Guards must refuse before opening this non-ROM.
            cases = [
                ("spaceport_pulse_ring", ["--skip-native"], "rdram-only route requires --rdram-dump"),
                ("boss_stomp_oracle", ["--rdram-dump", "100:" + tmp + "/x"],
                 "--rdram-dump requires an rdram_capture route"),
                ("spaceport_pulse_ring", ["--rdram-dump", "frame:" + tmp + "/x"],
                 "--rdram-dump expects FRAME[,FRAME...]:PREFIX"),
                ("spaceport_pulse_ring", ["--rdram-dump", "100:" + tmp + "/x", "--skip-ares"],
                 "--rdram-dump cannot reuse an old ares run"),
            ]
            for name, args, expected in cases:
                with self.subTest(route=name, args=args):
                    result = subprocess.run(
                        ["bash", str(ROOT / "tools/run_oracle.sh"), name, "--rom", str(rom), *args],
                        cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        timeout=30)
                    self.assertEqual(result.returncode, 2, result.stdout)
                    self.assertIn(expected, result.stdout)


if __name__ == "__main__":
    unittest.main()
