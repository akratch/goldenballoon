#!/usr/bin/env python3
"""The shifted-slot control must see a decoded fact change, not a refusal."""

import json
import unittest
from pathlib import Path

import check_save_v170_compat as compat


def slot_output(slot: dict, taj: int | None = None) -> str:
    taj_value = slot["tajFlags"] if taj is None else taj
    courses = []
    for index, status in enumerate(slot["courses"]):
        level = 5 if index == 3 else index
        courses.append(f"{level}:{status}")
    return (
        f"[SAVE_COMPAT] slot=0 status=valid taj={taj_value} "
        f"trophies={slot['trophies']} bosses={slot['bosses']} "
        f"ttAmulet={slot['ttAmulet']} wizpigAmulet={slot['wizpigAmulet']} "
        f"keys={slot['keys']} cutscenes={slot['cutscenes']} filename=0\n"
        f"[SAVE_COMPAT] slot=0 courses={','.join(courses)}\n"
        f"[SAVE_COMPAT] slot=0 balloons={','.join(str(v) for v in slot['balloons'])}\n"
        f"[SAVE_COMPAT] slot=0 worldFlags="
        f"{','.join(str(v) for v in slot['worldFlags'])}\n"
    )


class ShiftedControlTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        fixture = Path(compat.__file__).resolve().parent / "fixtures" / "save_v170"
        cls.expected = json.loads(
            (fixture / "inspect-1.7.0.json").read_text(encoding="utf-8"))
        cls.slot = cls.expected["slots"][0]

    def test_a_rejected_shifted_slot_does_not_pass_the_control(self):
        problems = compat.shifted_control_problems(self.expected, "no decode\n")
        self.assertTrue(problems)
        self.assertIn("not decoded", problems[0])

    def test_the_same_decoded_facts_fail_the_control(self):
        problems = compat.shifted_control_problems(
            self.expected, slot_output(self.slot))
        self.assertTrue(problems)
        self.assertIn("same facts", problems[0])

    def test_a_decoded_fact_difference_passes_the_control(self):
        problems = compat.shifted_control_problems(
            self.expected, slot_output(self.slot, taj=1))
        self.assertEqual(problems, [])


if __name__ == "__main__":
    unittest.main()
