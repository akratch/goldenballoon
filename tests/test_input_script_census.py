#!/usr/bin/env python3
"""Every input script the suite can hand the engine fits its loader.

The native loader refuses a script longer than its capacity outright (it used
to truncate it silently), so a generator that grew past the limit would turn
its gate into an abort with nothing to do with the code under test. This holds
the scripts that exist without a run to check_input_script_capacity.CAPACITY,
which that ROM gate holds to the engine's MAX_SCRIPT:

* every committed script under tests/input_scripts/;
* every native arm of every oracle route (tools/dkr_oracle_route.py
  native-script), which check_audio_level_reference, check_simulation_cadence
  and check_authored_rng_compat generate;
* check_resource_plateau's soak route at its default length and at the
  longer --frames its documentation suggests (490 entries, the largest);
* check_taj_challenges' dialogue-advance route, the longest a check writes
  inline (416).

Entries are counted the way platform_input_load_script counts them: text
after '#' dropped, and a line counts once it has a frame and a button field.
The other scripts checks write inline are fixed lists or shorter loops
(check_challenge_modes 84 taps, check_trophy_series 71, ...).

ROM-free.
"""

from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

TESTS = Path(__file__).resolve().parent
ROOT = TESTS.parent
sys.path.insert(0, str(TESTS))
sys.path.insert(0, str(ROOT / "tools"))

import check_input_script_capacity  # noqa: E402
import check_resource_plateau  # noqa: E402
import check_taj_challenges  # noqa: E402
import dkr_oracle_route  # noqa: E402

CAPACITY = check_input_script_capacity.CAPACITY


def entries(text: str) -> int:
    count = 0
    for line in text.splitlines():
        if len(line.split("#", 1)[0].split()) >= 2:
            count += 1
    return count


def oracle_scripts() -> dict[str, str]:
    scripts: dict[str, str] = {}
    for path in sorted((ROOT / "tools" / "oracle_routes").glob("*.json")):
        route = dkr_oracle_route.load_route(str(path))
        arms = [None] + [arm.get("name") for arm in dkr_oracle_route.native_arms(route)]
        for arm in dict.fromkeys(arms):
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                dkr_oracle_route.emit_native_script(route, arm)
            scripts[f"{path.stem}:{arm or 'default'}"] = out.getvalue()
    return scripts


class InputScriptCensus(unittest.TestCase):
    def test_counting_matches_the_loader(self):
        self.assertEqual(entries("# comment only\n\n100 A\n200 A+B 3 P2  # held\n300\n"), 2)

    def assert_fits(self, scripts: dict[str, str]) -> None:
        self.assertTrue(scripts, "the census found nothing to count")
        sizes = {name: entries(text) for name, text in scripts.items()}
        over = {name: size for name, size in sizes.items() if size > CAPACITY}
        self.assertEqual(over, {}, f"scripts over the loader's {CAPACITY} entries")
        largest = max(sizes, key=sizes.get)
        print(f"  {len(sizes)} script(s); largest {largest} = {sizes[largest]} "
              f"of {CAPACITY}")

    def test_committed_scripts(self):
        scripts = {path.name: path.read_text(encoding="utf-8")
                   for path in sorted((TESTS / "input_scripts").rglob("*"))
                   if path.is_file()}
        self.assert_fits(scripts)

    def test_oracle_route_scripts(self):
        self.assert_fits(oracle_scripts())

    def test_taj_challenges_route(self):
        with tempfile.TemporaryDirectory(prefix="mdkr-input-census-") as raw:
            path = Path(raw) / "taj.txt"
            check_taj_challenges.make_script(path)
            self.assert_fits({"taj_challenges": path.read_text(encoding="utf-8")})

    def test_resource_plateau_soak(self):
        self.assert_fits({
            f"resource_plateau:{frames}": check_resource_plateau.extended_route(frames)
            for frames in (check_resource_plateau.DEFAULT_FRAMES, 75000)})


if __name__ == "__main__":
    unittest.main(verbosity=2)
