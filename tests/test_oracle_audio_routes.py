#!/usr/bin/env python3
"""ROM-free audio-route schema and runner refusal controls."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import check_audio_level_reference as audio_check

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import dkr_oracle_route as route_tool


class AudioRoutes(unittest.TestCase):
    def test_capture_completion_witness(self):
        # Exercise the exact embedded validator used by the shell runner.
        runner = (ROOT / "tools/run_oracle.sh").read_text()
        validator = runner.rsplit("<<'PY'\n", 1)[1].split("\nPY\n", 1)[0]
        completion = "reached frame limit 11005 (dumped 0 frame(s), 22050 audio sample-frame(s)), exiting"
        boss = "forced retail race load 5 -> 38"
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            audio = root / "capture.raw"
            audio.write_bytes(bytes(22050 * 4))
            Path(str(audio) + ".rate").write_text("22050")
            log = root / "ares.log"
            for text, accepted in (
                    (boss + "\n" + completion, True),
                    (boss, False),
                    (completion, False),
                    (boss + "0\n" + completion, False),
                    (boss.replace("5 ->", "15 ->") + "\n" + completion, False),
                    (boss + "\n" + completion.replace("22050 audio", "44100 audio"), False),
                    (boss + "\n" + completion.replace("22050 audio", "11025 audio"), False),
                    (boss + "\n" + completion.replace("11005", "100"), False)):
                log.write_text(text)
                result = subprocess.run(
                    [sys.executable, "-c", validator, str(audio), str(log),
                     "11005", "5", "38"], text=True, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, timeout=30)
                self.assertEqual(result.returncode == 0, accepted, result.stdout)

    def test_native_boss_identity_is_required(self):
        for trace, accepted in (
                ("level_load: levelId=38 numPlayers=0 entrance=0 vehicle=0 cutscene=0 @frame~1374", True),
                ("", False),
                ("level_load: levelId=5 numPlayers=0 entrance=0 vehicle=0 cutscene=0 @frame~1374", False),
                ("level_load: levelId=38 numPlayers=0 entrance=0 vehicle=0 cutscene=1 @frame~1374", False)):
            with tempfile.TemporaryDirectory() as tmp:
                failures = []

                def capture(*args, **kwargs):
                    Path(args[4]).touch()
                    return 0, trace

                with mock.patch.object(audio_check, "run_capture", side_effect=capture):
                    wav, _ = audio_check.capture_reference_arm(
                        "unused-binary", "unused-rom", tmp, "boss_stomp_oracle",
                        None, failures.append, lambda _: None)
                self.assertEqual(wav is not None, accepted)
                self.assertEqual(bool(failures), not accepted)

    def test_explicit_audio_schema(self):
        for name in ("boss_stomp_oracle", "wizpig_stomp_oracle"):
            route = json.loads((ROOT / "tools/oracle_routes" / (name + ".json")).read_text())
            route_tool.validate_route(route)
            for value in (False, "true", 1):
                invalid = copy.deepcopy(route)
                invalid["audio_capture"] = value
                with self.assertRaises(SystemExit):
                    route_tool.validate_route(invalid)

    def test_runner_refuses_unscored_and_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            rom = root / "placeholder.z64"
            rom.touch()  # Guards must refuse before opening this non-ROM.
            existing = root / "existing.raw"
            existing.write_bytes(b"preserve")
            cases = [
                (["--skip-native", "--skip-ares"], "audio-only route requires --audio-dump"),
                (["--audio-dump", str(existing)], "destination already exists"),
            ]
            for args, expected in cases:
                result = subprocess.run(
                    ["bash", str(ROOT / "tools/run_oracle.sh"), "boss_stomp_oracle",
                     "--rom", str(rom), *args], cwd=ROOT, text=True,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
                self.assertEqual(result.returncode, 2, result.stdout)
                self.assertIn(expected, result.stdout)
            self.assertEqual(existing.read_bytes(), b"preserve")


if __name__ == "__main__":
    unittest.main()
