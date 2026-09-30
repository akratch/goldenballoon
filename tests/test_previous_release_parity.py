#!/usr/bin/env python3
"""ROM-free unit cover for the previous-release parity gate.

The gate needs a build of the published release and minutes of engine time, so
the parts that decide PASS, FAIL or SKIP are pinned here: which run counts as
having raced the route, the skip contract, the release-to-release wording of
the shared divergence report, and the facts the saves arm reads out of a run.
"""

import tempfile
import unittest
from pathlib import Path

import check_previous_release_parity as gate
from simhash_streams import SimHashRow, divergence_report

ANCIENT_LAKE = gate.ROUTES[0]
WHALE_BAY = next(r for r in gate.ROUTES if r.name == "whale-bay")
TRICKY = next(r for r in gate.ROUTES if r.name == "tricky-tops-1")


def load(level, players=0, vehicle=0, frame=2620):
    return (f"[TRACE] level_load: levelId={level} numPlayers={players} entrance=0 "
            f"vehicle={vehicle} cutscene=0 @frame~{frame}\n")


class RouteSetTests(unittest.TestCase):
    def test_the_ruling_routes_are_all_present(self):
        # Two worlds of AI racing, a boss, and a challenge arena at minimum.
        whats = " ".join(route.what for route in gate.ROUTES)
        self.assertIn("Dino Domain", whats)
        self.assertIn("Sherbet Island", whats)
        self.assertIn("boss", whats)
        self.assertIn("arena", whats)
        self.assertGreaterEqual(len(gate.ROUTES), 4)

    def test_the_budget_leaves_the_minimum_of_racing_on_every_route(self):
        self.assertGreaterEqual(gate.DEFAULT_FRAMES - 2974, gate.MIN_RACE_TICKS)

    def test_a_vehicle_is_only_forced_where_the_route_names_one(self):
        self.assertEqual(ANCIENT_LAKE.load_track, "5")
        self.assertEqual(WHALE_BAY.load_track, "8:1")


class RaceStartTests(unittest.TestCase):
    def test_the_menu_background_and_preview_loads_do_not_count(self):
        output = load(5, players=-1, frame=2020) + load(23, frame=568)
        self.assertIsNone(gate.race_start(output, ANCIENT_LAKE))

    def test_the_race_load_is_found(self):
        output = load(5, players=-1, frame=2020) + load(5, frame=2620)
        self.assertEqual(gate.race_start(output, ANCIENT_LAKE), 2620)

    def test_the_boss_race_counts_from_its_second_load(self):
        output = load(38, frame=2620) + load(38, frame=2974)
        self.assertEqual(gate.race_start(output, TRICKY), 2974)

    def test_the_wrong_vehicle_is_not_the_route(self):
        self.assertIsNone(gate.race_start(load(8, vehicle=0), WHALE_BAY))
        self.assertEqual(gate.race_start(load(8, vehicle=1), WHALE_BAY), 2620)


class RouteProblemTests(unittest.TestCase):
    def test_a_run_that_raced_long_enough_is_evidence(self):
        self.assertEqual(
            gate.route_problems("r", ANCIENT_LAKE, load(5), 6600, "this build"), [])

    def test_a_run_that_never_raced_the_level_is_refused(self):
        problems = gate.route_problems("r", ANCIENT_LAKE, load(8), 6600, "this build")
        self.assertTrue(problems)
        self.assertIn("never loaded as the race", problems[0])

    def test_too_little_racing_is_refused(self):
        problems = gate.route_problems("r", ANCIENT_LAKE, load(5, frame=4000), 6600, "x")
        self.assertTrue(any("raise --frames" in p for p in problems))

    def test_an_active_content_pack_is_refused(self):
        output = load(5) + "[MODS] 1 pack(s) active, 0 skipped\n"
        problems = gate.route_problems("r", ANCIENT_LAKE, output, 6600, "x")
        self.assertTrue(any("content pack" in p for p in problems))


class SkipContractTests(unittest.TestCase):
    def test_the_skip_code_is_not_success_or_failure(self):
        self.assertNotIn(gate.EXIT_SKIPPED, (0, 1, 2))

    def test_the_banner_says_nothing_was_compared(self):
        banner = gate.skip_banner("no --reference-build was supplied")
        self.assertIn("SKIPPED", banner)
        self.assertIn("Nothing was compared", banner)
        self.assertIn("--reference-build", banner)


class ReportWordingTests(unittest.TestCase):
    def test_a_release_divergence_is_not_called_architectural(self):
        rows = [SimHashRow(t, 7, "0000000000000001", f"[SIMHASH] tick={t}")
                for t in range(6)]
        other = list(rows)
        other[3] = other[3]._replace(digest="00000000000000ff")
        report = "\n".join(divergence_report(
            "US arena", rows, other, 3, labels=("v1.7.0", "this build"),
            kind="retail simulation", parties="the two releases",
            value_suspects="a changed decision"))
        self.assertIn("retail simulation divergence at tick 3", report)
        self.assertIn("v1.7.0", report)
        self.assertIn("this build", report)
        self.assertNotIn("architecture", report)
        self.assertIn("a changed decision", report)


class SavesArmParsingTests(unittest.TestCase):
    def test_the_best_finish_is_the_lowest_clock_of_every_finish(self):
        output = "".join(
            f"[TRACE] [PACE] frame={f} R=2 | racer x=0 y=0 z=0 clock={c} cp=1 lap=2 "
            f"rlap=3 fin={fin} ghost=0 gbank=-1\n"
            for f, c, fin in ((1, 4000, 0), (2, 4568, 1), (3, 4568, 1), (4, 0, 0),
                              (5, 4552, 1)))
        self.assertEqual(gate.best_finish_clock(output), 4552)
        self.assertIsNone(gate.best_finish_clock("no pace rows\n"))

    def test_ghost_events_are_read_by_kind(self):
        output = ("[TRACE] [TTGHOST] frame=10 event=load level=5 vehicle=0 status=5 "
                  "nodes=0 character=-1 time=-1\n"
                  "[TRACE] [TTGHOST] frame=20 event=save level=5 vehicle=0 status=0 "
                  "nodes=172 character=9 time=4568\n")
        self.assertEqual(gate.ghost_events(output, "save"), [(5, 0, 0, 172, 9, 4568)])
        self.assertEqual(gate.ghost_events(output, "load"), [(5, 0, 5, 0, -1, -1)])

    def test_a_macos_sidecar_is_not_taken_for_the_pal_rom(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "._Diddy Kong Racing (Europe) (Rev 1).z64").write_bytes(b"\0" * 4096)
            self.assertIsNone(gate.find_pal_rom(root))
            rom = root / "Diddy Kong Racing (Europe) (Rev 1).z64"
            rom.write_bytes(b"\0" * gate.ROM_BYTES)
            self.assertEqual(gate.find_pal_rom(root), rom.resolve())


if __name__ == "__main__":
    unittest.main()
