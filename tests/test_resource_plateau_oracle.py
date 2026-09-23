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

    def test_long_route_keeps_retrying(self):
        rows = extended_route(75000).splitlines()
        ticks = [int(row.split()[0]) for row in rows
                 if row and row.split()[0].isdigit()]
        self.assertGreater(max(ticks), 74000)
        self.assertLess(max(ticks), 75000)
        self.assertLessEqual(len(ticks), 512)


if __name__ == "__main__":
    unittest.main()
