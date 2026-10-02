#!/usr/bin/env python3
"""Source-custody controls use disposable repositories, not the user's index."""
import importlib.util
import os
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

    def test_service_and_library_dependencies_are_release_inputs(self):
        names = ["services/party/package.json", "services/party/package-lock.json",
                 "services/party/src/worker.ts", "lib/fixture/reader.c"]
        for name in names:
            source = self.repo / name
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text("fixture\n")
        expected = [{"path": name, "status": "untracked"} for name in sorted(names)]
        self.assertEqual(custody.inventory(self.repo), expected)
        self.git("add", "services", "lib")
        self.assertEqual(custody.inventory(self.repo),
                         [{**item, "status": "index-only"} for item in expected])
        self.commit()
        self.assertEqual(custody.inventory(self.repo), [])
        (self.repo / names[0]).unlink()
        self.assertEqual(custody.inventory(self.repo),
                         [{"path": names[0], "status": "missing-worktree"}])

    def test_packaging_and_resource_inputs_are_release_inputs(self):
        # What CMake and the package scripts read besides compiled source: the
        # vendored libraries and their licences, the Windows resource and
        # manifest templates and icon, the launcher's embedded art, the macOS
        # bundle scripts and Info.plist, the web shell, the build presets,
        # and the licence every package ships.
        names = ["third_party/qrcodegen/qrcodegen.cpp", "windows/mdkr64.rc.in",
                 "windows/mdkr64.ico", "brand/hero.png", "macos/Resources/Info.plist",
                 "dist/web/new-module.js", "CMakePresets.json", "LICENSE",
                 ".githooks/pre-commit"]
        for name in names:
            source = self.repo / name
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text("fixture\n")
        self.assertEqual(custody.inventory(self.repo),
                         [{"path": name, "status": "untracked"} for name in sorted(names)])

    def test_generated_web_outputs_stay_out_when_ignored(self):
        (self.repo / ".gitignore").write_text("dist/web/*.wasm\n")
        (self.repo / "dist/web").mkdir(parents=True)
        (self.repo / "dist/web/mdkr64_web.wasm").write_bytes(b"\0asm")
        self.assertEqual(custody.inventory(self.repo), [])

    def test_every_tracked_top_level_entry_is_classified(self):
        # Derived from the real tree: a new top-level directory or root file
        # must be named as a release input or as something the build and the
        # packages never read, so custody cannot silently fall behind them.
        top = custody.git(MODULE_PATH.parents[2], "ls-tree", "--name-only", "-z", "HEAD")
        unclassified = sorted(custody.paths(top) - custody.SOURCE_AREAS
                              - custody.ROOT_INPUTS - set(custody.NOT_INPUTS))
        self.assertEqual(unclassified, [])
        self.assertFalse(set(custody.NOT_INPUTS) & (custody.SOURCE_AREAS | custody.ROOT_INPUTS))
        self.assertTrue(all(reason for reason in custody.NOT_INPUTS.values()))

    def test_git_failure_does_not_become_an_empty_inventory(self):
        with patch.object(custody, "git", side_effect=subprocess.CalledProcessError(73, "git")):
            with self.assertRaises(subprocess.CalledProcessError):
                custody.inventory(self.repo)

    def test_filename_boundaries_and_missing_terminator(self):
        # Windows cannot hold a newline in a filename; the quote still counts.
        name = ("tests/quote' fixture.py" if os.name == "nt"
                else "tests/quote' newline\nfixture.py")
        source = self.repo / name
        source.parent.mkdir()
        source.write_text("# fixture\n")
        self.assertEqual(custody.inventory(self.repo), [{"path": name, "status": "untracked"}])
        with self.assertRaises(ValueError):
            custody.paths(b"unterminated")


if __name__ == "__main__":
    unittest.main()
