#!/usr/bin/env python3
"""Source-custody controls use disposable repositories, not the user's index."""
import importlib.util
from pathlib import Path
import unittest
import subprocess
from unittest.mock import patch

import test_clean_room_guard as fixtures

MODULE_PATH = Path(__file__).resolve().parents[1] / "tools/ci/check_source_custody.py"
SPEC = importlib.util.spec_from_file_location("source_custody", MODULE_PATH)
custody = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(custody)


class SourceCustodyTests(unittest.TestCase):
    # Reuse fixture setup/helpers without inheriting the clean-room test cases.
    setUp = fixtures.CleanRoomGuardTests.setUp
    git = fixtures.CleanRoomGuardTests.git
    commit = fixtures.CleanRoomGuardTests.commit

    def test_source_lifecycle(self):
        source = self.repo / "platform/new source.c"
        source.parent.mkdir()
        source.write_text("int fixture;\n")
        self.assertEqual(custody.inventory(self.repo),
                         [{"path": "platform/new source.c", "status": "untracked"}])
        self.git("add", "platform/new source.c")
        self.assertEqual(custody.inventory(self.repo),
                         [{"path": "platform/new source.c", "status": "index-only"}])
        self.commit()
        self.assertEqual(custody.inventory(self.repo), [])
        source.unlink()
        self.assertEqual(custody.inventory(self.repo),
                         [{"path": "platform/new source.c", "status": "missing-worktree"}])

    def test_planning_docs_and_ignored_outputs_are_not_source_inputs(self):
        (self.repo / "docs").mkdir()
        (self.repo / "docs/private-plan.md").write_text("private planning fixture")
        (self.repo / ".gitignore").write_text("build/\n")
        (self.repo / "build").mkdir()
        (self.repo / "build/output.c").write_text("generated output")
        self.assertEqual(custody.inventory(self.repo), [])

    def test_root_build_recipe_is_required(self):
        (self.repo / "CMakeLists.txt").write_text("# fixture\n")
        self.assertEqual(custody.inventory(self.repo),
                         [{"path": "CMakeLists.txt", "status": "untracked"}])

    def test_git_failure_does_not_become_an_empty_inventory(self):
        with patch.object(custody, "git", side_effect=subprocess.CalledProcessError(73, "git")):
            with self.assertRaises(subprocess.CalledProcessError):
                custody.inventory(self.repo)

    def test_filename_boundaries_and_missing_terminator(self):
        name = "tests/quote' newline\nfixture.py"
        source = self.repo / name
        source.parent.mkdir()
        source.write_text("# fixture\n")
        self.assertEqual(custody.inventory(self.repo), [{"path": name, "status": "untracked"}])
        with self.assertRaises(ValueError):
            custody.paths(b"unterminated")


if __name__ == "__main__":
    unittest.main()
