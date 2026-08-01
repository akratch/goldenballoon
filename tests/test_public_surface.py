#!/usr/bin/env python3
"""ROM-free positive controls for the public Git-surface guard."""

from __future__ import annotations

import importlib.util
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    "check_public_surface", ROOT / "tools" / "check_public_surface.py"
)
assert SPEC is not None and SPEC.loader is not None
guard = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = guard
SPEC.loader.exec_module(guard)


class PublicSurfaceGuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.patterns = guard.load_patterns(ROOT)

    def test_private_directories_and_plan_names_are_rejected(self) -> None:
        self.assertIsNotNone(guard.forbidden_path_reason(".private/working.md"))
        self.assertIsNotNone(guard.forbidden_path_reason("./.private/working.md"))
        private_dir = "docs/" + "super" + "powers/plans/work.md"
        self.assertIsNotNone(guard.forbidden_path_reason(private_dir))
        self.assertIsNotNone(
            guard.forbidden_path_reason("docs/notes/POST_RELEASE_ENGINEERING_PLAN.md")
        )
        self.assertIsNone(guard.forbidden_path_reason("docs/architecture/input.md"))
        self.assertIsNone(guard.forbidden_path_reason("ROADMAP.md"))

    def test_absolute_personal_path_is_detected(self) -> None:
        value = "/" + "Users" + "/person/Desktop/dev/project"
        hits = guard.blob_hits("docs/example.md", value.encode(), self.patterns)
        self.assertEqual(len(hits), 1)

    def test_agent_marker_is_detected(self) -> None:
        marker = "Cl" + "aude generated this working note"
        hits = guard.blob_hits("docs/example.md", marker.encode(), self.patterns)
        self.assertEqual(len(hits), 1)

    def test_detector_files_are_the_only_content_exemption(self) -> None:
        marker = "Cl" + "aude"
        self.assertEqual(
            guard.blob_hits(guard.DENYLIST_PATH, marker.encode(), self.patterns), []
        )
        sdk_marker = "UNPUBLISHED " + "PROPRIETARY"
        self.assertEqual(
            guard.blob_hits(
                "tools/check_native_sdk_surface.py",
                sdk_marker.encode(),
                self.patterns,
            ),
            [],
        )
        self.assertTrue(
            guard.blob_hits("docs/contributor.md", marker.encode(), self.patterns)
        )

    def test_public_commit_identity_policy(self) -> None:
        self.assertTrue(guard.email_is_public("akratch@users.noreply.github.com"))
        self.assertTrue(guard.email_is_public("release@example.invalid"))
        self.assertFalse(guard.email_is_public("person@personal-domain.test"))

    def test_tag_message_uses_the_same_content_policy(self) -> None:
        marker = "Cl" + "aude generated release notes"
        self.assertTrue(
            guard.blob_hits("<tag-message>", marker.encode(), self.patterns)
        )

    def test_commit_and_annotated_tag_metadata_are_scanned(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            repo = Path(temporary)
            (repo / "tools").mkdir()
            shutil.copy2(ROOT / guard.DENYLIST_PATH, repo / guard.DENYLIST_PATH)

            def git(*arguments: str) -> str:
                result = subprocess.run(
                    ["git", "-C", str(repo), *arguments],
                    check=True,
                    stdout=subprocess.PIPE,
                    text=True,
                )
                return result.stdout.strip()

            git("init", "-q")
            git("config", "user.name", "Public Test")
            git("config", "user.email", "public-test@example.invalid")
            (repo / "README.md").write_text("public fixture\n", encoding="utf-8")
            git("add", ".")
            git("commit", "-q", "-m", "initial fixture")
            safe_commit = git("rev-parse", "HEAD")
            patterns = guard.load_patterns(repo)
            self.assertEqual(guard.scan_commit(repo, safe_commit, patterns), [])

            (repo / "README.md").write_text("second fixture\n", encoding="utf-8")
            git("add", "README.md")
            git(
                "-c",
                "user.email=person@personal-domain.test",
                "commit",
                "-q",
                "-m",
                "identity control",
            )
            unsafe_commit = git("rev-parse", "HEAD")
            self.assertTrue(guard.scan_commit(repo, unsafe_commit, patterns))

            marker = "Cl" + "aude generated release notes"
            git("tag", "-a", "v-test", "-m", marker, safe_commit)
            tag_object = git("rev-parse", "v-test")
            self.assertTrue(guard.scan_annotated_tag(repo, tag_object, patterns))


if __name__ == "__main__":
    unittest.main()
