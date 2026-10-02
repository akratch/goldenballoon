#!/usr/bin/env python3
"""Frame-wait attribution in tools/measure_texture_pack_stalls.py (T12c).

Two things the stall tables rest on:
  * a long frame is charged to the part of the frame that grew, not to the
    part that is always big (the pacer's ordinary sleep), and the parts tile
    the frame;
  * the game's output reaches the run log through a drained pipe, so a slow
    log file stalls the tool's reader instead of the game's write -- and every
    byte still lands, and a timed-out game is still killed.
"""
from __future__ import annotations

import os
import signal
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import measure_texture_pack_stalls as tool  # noqa: E402


def row(frame: int, **fields: int) -> str:
    base = {
        "tick": frame, "swap": 1, "dtUs": 16667, "cpuUs": 1500,
        "walkUs": 400, "endUs": 500, "gameUs": 1500, "startUs": 100,
        "acquireUs": 40, "submitUs": 60, "presentUs": 10, "paceUs": 14800,
        "paceReqUs": 13000, "paceLateUs": 1800, "paceCalls": 1,
        "eventsUs": 100, "swapUs": 0, "fenceUs": 0, "audioUs": 100,
        "rowUs": 50, "procCpuUs": 2000, "runnableUs": 2200, "lowQosUs": 0,
        "pageins": 0, "vcsw": 0, "ivcsw": 30, "prio": 31, "visible": 0,
        "active": 0, "tUs": 1000000 + frame * 16667,
    }
    base.update(fields)
    body = " ".join(f"{key}={value}" for key, value in base.items())
    return f"[TEXPACK-FRAME] frame={frame} {body}"


def run_log(stall_rows: dict[int, dict]) -> str:
    lines = ["texture_cache_state: level=21 players=1 cutscene=0"]
    for frame in range(1, 400):
        lines.append(row(frame, **stall_rows.get(frame, {})))
    return "\n".join(lines) + "\n"


class AttributionTest(unittest.TestCase):
    def analyse(self, stalls: dict[int, dict]) -> dict:
        return tool.analyse(run_log(stalls))["waits"]

    def test_parts_tile_an_ordinary_frame(self) -> None:
        frames = tool.displayed_frames(tool.parse_run(run_log({}))[0])
        split = tool.wait_split(frames[10])
        self.assertEqual(sum(split.values()), frames[10]["dtUs"])

    def test_replay_frames_still_tile(self) -> None:
        # Smoothing on: a presentation replay ran in the host boundary. Its
        # walk is inside walkUs too; the game pass's own walk is the rest.
        replay = {"gameUs": 1500, "startUs": 100, "walkUs": 1600,
                  "replayWalkUs": 1200, "endUs": 500, "replayUs": 3000,
                  "paceUs": 11917, "paceReqUs": 11917, "paceLateUs": 0}
        frames = tool.displayed_frames(
            tool.parse_run(run_log({10: replay}))[0])
        frame = next(f for f in frames if f["frame"] == 10)
        split = tool.wait_split(frame)
        self.assertEqual(sum(split.values()), frame["dtUs"])
        self.assertEqual(split["walk"], 400)
        self.assertEqual(split["replay"], 3000)
        self.assertEqual(split["game logic"], 500)

    def test_blocked_log_write_is_the_row_print(self) -> None:
        # The T12c shape: the pacer did not sleep at all because the row
        # print ate the frame.
        waits = self.analyse({200: {"dtUs": 40000, "rowUs": 37900,
                                    "paceUs": 0, "paceReqUs": 0,
                                    "paceLateUs": 0}})
        self.assertEqual(waits["causes"], {"row print": 1})
        self.assertEqual(waits["stalls"][0]["frame"], 200)

    def test_pacer_oversleep_is_not_its_ordinary_sleep(self) -> None:
        waits = self.analyse({150: {"dtUs": 52000, "paceUs": 50000,
                                    "paceReqUs": 13000,
                                    "paceLateUs": 37000}})
        self.assertEqual(waits["causes"], {"pacer oversleep": 1})
        self.assertEqual(waits["stalls"][0]["pace_late_ms"], 37.0)

    def test_the_cause_is_the_excess_not_the_biggest_part(self) -> None:
        # The pacer's 16 ms sleep is the largest part of this 34 ms frame,
        # but it is ordinary; the audio service grew by 13 ms.
        waits = self.analyse({120: {"dtUs": 34000, "paceUs": 16000,
                                    "paceReqUs": 16000, "paceLateUs": 0,
                                    "audioUs": 13100}})
        self.assertEqual(waits["causes"], {"audio": 1})

    def test_blocked_acquire_and_off_cpu_game_logic(self) -> None:
        waits = self.analyse({
            100: {"dtUs": 390000, "gameUs": 389700, "endUs": 389600,
                  "acquireUs": 389200, "paceUs": 0, "paceReqUs": 0},
            300: {"dtUs": 60000, "gameUs": 45000, "runnableUs": 47000,
                  "procCpuUs": 2000},
        })
        self.assertEqual(waits["causes"], {"acquire": 1, "game logic": 1})
        off_cpu = {stall["frame"]: stall["off_cpu_ms"]
                   for stall in waits["stalls"]}
        self.assertEqual(off_cpu[300], 45.0)

    def test_window_transitions_are_counted(self) -> None:
        stalls = {frame: {"visible": 1, "active": 1}
                  for frame in range(50, 60)}
        waits = self.analyse(stalls)
        self.assertEqual(waits["window_transition_count"], 2)
        self.assertEqual(waits["visible_frames"], 10)

    def test_logs_without_the_split_say_so(self) -> None:
        old = "\n".join(
            f"[TEXPACK-FRAME] frame={frame} tick={frame} swap=1 dtUs=16667 "
            f"cpuUs=1000 walkUs=500 endUs=100" for frame in range(1, 50))
        self.assertFalse(tool.analyse(old)["waits"]["available"])


class SchedulingTest(unittest.TestCase):
    def record(self, prio: int) -> dict:
        stalls = {frame: {"prio": prio} for frame in range(1, 400)}
        return {"waits": tool.analyse(run_log(stalls))["waits"]}

    def test_a_background_only_process_is_refused(self) -> None:
        self.assertIn("priority 31", tool.scheduling_error(self.record(31)))

    def test_an_application_priority_passes(self) -> None:
        self.assertIsNone(tool.scheduling_error(self.record(46)))
        self.assertIsNone(tool.scheduling_error(self.record(47)))

    def test_an_unknown_priority_is_not_judged(self) -> None:
        self.assertIsNone(tool.scheduling_error(self.record(-1)))


class PipeLoggingTest(unittest.TestCase):
    def test_every_byte_lands_through_the_pipe(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "run.log"
            script = ("import sys\n"
                      "for i in range(20000):\n"
                      "    sys.stderr.write(f'row {i} ' + 'x' * 60 + '\\n')\n"
                      "print('done')\n")
            code = tool.run_game([sys.executable, "-c", script],
                                 Path(directory), {}, log, 60, False)
            self.assertEqual(code, 0)
            lines = log.read_text().splitlines()
            self.assertEqual(sum(1 for line in lines
                                 if line.startswith("row ")), 20000)
            self.assertIn("done", lines)

    def test_the_game_writes_into_a_pipe_not_the_file(self) -> None:
        probe = ("import os, stat\n"
                 "print('fifo', stat.S_ISFIFO(os.fstat(1).st_mode),"
                 " stat.S_ISFIFO(os.fstat(2).st_mode))\n")
        for direct, expected in ((False, "fifo True True"),
                                 (True, "fifo False False")):
            with tempfile.TemporaryDirectory() as directory:
                log = Path(directory) / "run.log"
                tool.run_game([sys.executable, "-c", probe],
                              Path(directory), {}, log, 60, direct)
                self.assertIn(expected, log.read_text())

    def test_a_held_pipe_is_reported_not_waited_on(self) -> None:
        # The game exits but leaves a helper holding its output open: the
        # reader must give up and say so instead of hanging the run.
        script = ("import subprocess, sys\n"
                  "helper = subprocess.Popen(['sleep', '30'])\n"
                  "print('helper', helper.pid, flush=True)\n")
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "run.log"
            started = time.monotonic()
            with self.assertRaises(tool.LogReaderError):
                tool.run_game([sys.executable, "-c", script],
                              Path(directory), {}, log, 60, False,
                              reader_timeout=1.0)
            self.assertLess(time.monotonic() - started, 20)
            helper = int(log.read_text().split()[1])
            os.kill(helper, signal.SIGKILL)

    def test_a_timed_out_game_is_killed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "run.log"
            started = time.monotonic()
            code = tool.run_game(
                [sys.executable, "-c",
                 "import time\nprint('up', flush=True)\ntime.sleep(60)\n"],
                Path(directory), {}, log, 1.0, False)
            self.assertIsNone(code)
            self.assertLess(time.monotonic() - started, 30)
            self.assertIn("up", log.read_text())

    def test_direct_mode_still_writes_the_log(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "run.log"
            code = tool.run_game([sys.executable, "-c", "print('direct')"],
                                 Path(directory), {}, log, 60, True)
            self.assertEqual(code, 0)
            self.assertIn("direct", log.read_text())


if __name__ == "__main__":
    unittest.main()
