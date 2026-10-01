"""Synthetic controls for cache plateau evidence; no ROM or renderer needed."""
import unittest
from pathlib import Path
import subprocess
import tempfile
from unittest.mock import patch

from check_resource_plateau import cache_plateau_error, extended_route, run_backend


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

    def test_accounted_rows_still_respect_the_budget(self):
        over = self.accounted(pixels=5000, first_bytes=5000, first=9)
        self.assertIsNotNone(cache_plateau_error([self.ACCOUNTED] * 3 + [over]))

    def test_long_route_keeps_retrying(self):
        rows = extended_route(75000).splitlines()
        ticks = [int(row.split()[0]) for row in rows
                 if row and row.split()[0].isdigit()]
        self.assertGreater(max(ticks), 74000)
        self.assertLess(max(ticks), 75000)
        self.assertLessEqual(len(ticks), 512)


if __name__ == "__main__":
    unittest.main()
