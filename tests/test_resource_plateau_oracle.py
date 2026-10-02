"""Synthetic controls for cache plateau evidence; no ROM or renderer needed."""
import importlib.util
import sys
import unittest
from pathlib import Path
import subprocess
import tempfile
from unittest.mock import patch

from check_resource_plateau import (CACHE_RACES_REQUIRED, CACHE_WARMUP_RACES,
                                    DEFAULT_RUN_TIMEOUT,
                                    GENERATED_PACK_TIMEOUT, cache_gate_error,
                                    cache_plateau_error, extended_route,
                                    run_backend, warm_reupload_error)

ROOT = Path(__file__).resolve().parent.parent


class CachePlateauTests(unittest.TestCase):
    def test_launch_scrubs_all_inherited_game_controls(self):
        with tempfile.TemporaryDirectory(prefix="mdkr-resource-env-") as temporary:
            root = Path(temporary)
            with patch.dict("os.environ", {"MDKR64_HIDDEN": "poison",
                                           "GE007_RENDERER": "poison",
                                           "MDKR_SAVE_DIR": "/not-the-fixture"}), \
                 patch("check_resource_plateau.subprocess.run",
                       return_value=subprocess.CompletedProcess([], 1)) as launch:
                error = run_backend(Path("unused"), Path("unused-rom"), "gl",
                                    25000, 1, False, root)
            self.assertIn("exit code 1", error)
            env = launch.call_args.kwargs["env"]
            self.assertFalse("MDKR64_HIDDEN" in env, "inherited MDKR64 control leaked")
            self.assertFalse("GE007_RENDERER" in env, "inherited GE007 control leaked")
            self.assertEqual(Path(env["MDKR_SAVE_DIR"]).parent,
                             launch.call_args.kwargs["cwd"])
            self.assertEqual(env["MDKR_RENDERER"], "gl")

    def setUp(self):
        self.row = (5, 0, 100, 1000, 4096, 320, 4)

    def test_stable_and_warmup(self):
        self.assertIsNone(cache_plateau_error([self.row] * 4))
        cold = (5, 0, 100, 500, 4096, 160, 2)
        self.assertIsNone(cache_plateau_error([cold] + [self.row] * 3))

    def test_missing_or_wrong_scene(self):
        for rows in ([], [self.row] * 3, [(0, *self.row[1:])] * 4):
            self.assertIsNotNone(cache_plateau_error(rows))

    def test_each_retained_field_growth(self):
        for field in (3, 5, 6):
            changed = list(self.row)
            changed[field] += 1
            self.assertIsNotNone(cache_plateau_error([self.row] * 3 + [tuple(changed)]))

    def test_over_budget_even_before_warmup(self):
        oversized = (5, 0, 100, 4097, 4096, 320, 4)
        self.assertIsNotNone(cache_plateau_error([oversized] + [self.row] * 3))
        self.assertIsNotNone(cache_plateau_error([(5, 0, 100, 0, 0, 0, 0)] * 4))

    # Issue #66: the census carries what the cache did, and growth that is
    # exactly first use passes. Rows gain inflight, pending, firstResident,
    # firstResidentBytes, reloads, evictions.
    ACCOUNTED = (5, 0, 100, 1000, 4096, 320, 4, 0, 0, 4, 1000, 0, 0)

    def accounted(self, **changes):
        names = ("level", "players", "cutscene", "pixels", "limit", "table",
                 "entries", "inflight", "pending", "first", "first_bytes",
                 "reloads", "evictions")
        row = dict(zip(names, self.ACCOUNTED))
        row.update(changes)
        return tuple(row[name] for name in names)

    def test_late_first_use_is_not_growth(self):
        late = self.accounted(pixels=1100, entries=5, first=5, first_bytes=1100)
        self.assertIsNone(cache_plateau_error([self.ACCOUNTED] * 3 + [late]))
        # A new key the pack does not supply: a table entry, no pixels.
        absent = self.accounted(entries=5)
        self.assertIsNone(cache_plateau_error([self.ACCOUNTED] * 3 + [absent]))

    def test_growth_beyond_first_use_fails(self):
        for grown in (
                self.accounted(pixels=1100),                       # no first use
                self.accounted(pixels=1100, first=5, first_bytes=1050),
                self.accounted(pixels=1100, first_bytes=1100, reloads=1),
                self.accounted(evictions=1),
                self.accounted(table=640),                         # no entries
                self.accounted(entries=3)):
            self.assertIsNotNone(
                cache_plateau_error([self.ACCOUNTED] * 3 + [grown]), grown)

    def steady(self, **per_race):
        """Four race samples, each adding `per_race` to the settled row."""
        rows = []
        for race in range(4):
            changes = {name: value * race for name, value in per_race.items()}
            base = dict(zip(("pixels", "table", "entries", "first",
                             "first_bytes"), (1000, 320, 4, 4, 1000)))
            rows.append(self.accounted(**{
                name: base[name] + changes.get(name, 0) for name in base}))
        return rows

    def test_recurring_first_use_is_growth(self):
        # Store slots are never released, so an identity that is new in EVERY
        # race is the unbounded growth this gate exists for, even when each
        # step is exactly accounted as first use.
        for label, rows in (
                ("new entries every race", self.steady(entries=5)),
                ("first-use pixels every race",
                 self.steady(pixels=100, entries=1, first=1, first_bytes=100)),
                ("table doubling with its entries",
                 [self.accounted(table=320 * 2 ** race, entries=4 + 300 * race)
                  for race in range(4)])):
            self.assertIsNotNone(cache_plateau_error(rows), label)

    def test_a_burst_of_first_use_is_bounded(self):
        burst = self.accounted(entries=4 + 50)
        self.assertIsNotNone(cache_plateau_error([self.ACCOUNTED] * 3 + [burst]))

    def test_the_t12a_late_texture_still_passes(self):
        # One 138,240-byte texture first drawn inside the window, then flat:
        # the shape T12a's flake actually had.
        budget = 1 << 30
        settled = self.accounted(limit=budget)
        late = self.accounted(limit=budget, pixels=1000 + 138240, entries=6,
                              first=5, first_bytes=1000 + 138240)
        self.assertIsNone(cache_plateau_error([settled] * 2 + [late] * 2))
        self.assertIsNone(cache_plateau_error([settled] * 3 + [late]))

    def races(self, entries):
        """One accounted race sample per value in `entries`, each new entry
        a first-use identity of 100 bytes: growth exactly accounted, inside
        a budget large enough never to be the reason a case fails."""
        return [self.accounted(limit=1 << 30, pixels=1000 + 100 * (value - 4),
                               entries=value, first=value,
                               first_bytes=1000 + 100 * (value - 4))
                for value in entries]

    def periodic(self, period, phase, races=None):
        """`races` samples whose entries grow by one on every `period`-th
        step after warm-up, the first growth on step `phase`."""
        races = CACHE_RACES_REQUIRED if races is None else races
        entries = [4] * races
        for race in range(CACHE_WARMUP_RACES + 1, races):
            step = race - CACHE_WARMUP_RACES - 1
            entries[race] = entries[race - 1] + (step % period == phase)
        return self.races(entries)

    def test_periodic_growth_is_growth(self):
        # T12d: growth that recurs every second, third or fourth race is
        # unbounded growth too, in whatever phase it starts. The window holds
        # every race after warm-up, and the gate requires enough races
        # (CACHE_RACES_REQUIRED) for it to show two changed steps of any of
        # those periods.
        for period in (2, 3, 4):
            for phase in range(period):
                rows = self.periodic(period, phase)
                self.assertIsNotNone(cache_gate_error(rows), (period, phase))
        # The review's two shapes, on the route it had: they pass the oracle
        # over eight races, which is why the gate refuses so short a run.
        for entries in ([4, 4, 5, 5, 5, 6, 6, 6], [4, 4, 5, 5, 5, 5, 6, 6]):
            rows = self.races(entries)
            self.assertIsNone(cache_plateau_error(rows), entries)
            self.assertIsNotNone(cache_gate_error(rows), entries)

    def test_the_gate_needs_its_races(self):
        flat = self.races([4] * CACHE_RACES_REQUIRED)
        self.assertIsNone(cache_gate_error(flat))
        self.assertIsNotNone(cache_gate_error(flat[1:]))

    def test_one_late_first_use_after_warm_up_passes(self):
        # A single late texture anywhere after warm-up, then flat: T12a's
        # flake, on the full route.
        races = CACHE_RACES_REQUIRED
        for late in range(2, races):
            entries = [4] * late + [5] * (races - late)
            self.assertIsNone(cache_gate_error(self.races(entries)), late)

    def test_warm_up_growth_is_not_judged(self):
        # The real route: races 1->2 add ~113 identities, 2->3 a few, then
        # flat. Only the races after warm-up are held to the plateau rule.
        entries = [4, 117, 119] + [119] * (CACHE_RACES_REQUIRED - 3)
        self.assertIsNone(cache_gate_error(self.races(entries)))

    def test_accounted_rows_still_respect_the_budget(self):
        over = self.accounted(pixels=5000, first_bytes=5000, first=9)
        self.assertIsNotNone(cache_plateau_error([self.ACCOUNTED] * 3 + [over]))

    # T12d: with a pack, a warm race load re-uploads the replacements the
    # store already holds outside the upload budget. Rows: level, players,
    # cutscene, deferred, unbudgeted, one per completed generation.
    def test_warm_race_loads_reupload_at_once(self):
        rows = [(5, 0, 100, 0, 60)] + [(5, 0, 100, 0, 84)] * 6
        self.assertIsNone(warm_reupload_error(rows))
        # The cold first race may have nothing resident yet.
        self.assertIsNone(warm_reupload_error([(5, 0, 100, 3, 0)] + rows[1:]))

    def test_a_warm_race_load_with_no_reupload_fails(self):
        rows = [(5, 0, 100, 0, 60)] + [(5, 0, 100, 0, 84)] * 6
        rows[4] = (5, 0, 100, 9, 0)
        self.assertIsNotNone(warm_reupload_error(rows))
        self.assertIsNotNone(warm_reupload_error(rows[:2]))
        other_scene = [(0, 0, 0, 0, 5)] * 7
        self.assertIsNotNone(warm_reupload_error(other_scene))

    def test_ceilings_fit_the_suite_budget(self):
        # T12d: the dump run and both backend runs, each at its own ceiling,
        # must fit inside the run_checks task's budget, so a slow run fails
        # with the gate's message rather than being killed by the runner.
        spec = importlib.util.spec_from_file_location(
            "run_checks_for_budget", ROOT / "tools" / "run_checks.py")
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        task = next(check for check in module.CHECKS
                    if check.name == "resource_plateau")
        # Plus a margin for the gate's own work between runs.
        self.assertLessEqual(
            GENERATED_PACK_TIMEOUT + 2 * DEFAULT_RUN_TIMEOUT + 120,
            task.timeout)

    def test_long_route_keeps_retrying(self):
        rows = extended_route(75000).splitlines()
        ticks = [int(row.split()[0]) for row in rows
                 if row and row.split()[0].isdigit()]
        self.assertGreater(max(ticks), 74000)
        self.assertLess(max(ticks), 75000)
        self.assertLessEqual(len(ticks), 512)


if __name__ == "__main__":
    unittest.main()
