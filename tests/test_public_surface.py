#!/usr/bin/env python3
"""ROM-free positive controls for the public Git-surface guard."""

from __future__ import annotations

import hashlib
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


def bash_executable() -> str:
    """Return Bash without accidentally selecting Windows' WSL launcher."""
    if sys.platform == "win32":
        # System32/bash.exe is a WSL launcher, not a shell. GitHub's hosted
        # Windows image exposes it before MSYS2's Bash in PATH, so resolve the
        # unambiguous MSYS2 `sh.exe` and use its sibling instead.
        sh_executable = shutil.which("sh.exe")
        if sh_executable is not None:
            msys_bash = Path(sh_executable).with_name("bash.exe")
            if msys_bash.is_file():
                return str(msys_bash)
    return shutil.which("bash") or "bash"


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
        value = "/" + "Users" + "/person/Desk" + "top/dev/project"
        hits = guard.blob_hits("docs/example.md", value.encode(), self.patterns)
        self.assertEqual(len(hits), 1)

    def test_agent_marker_is_detected(self) -> None:
        marker = "Cl" + "aude generated this working note"
        hits = guard.blob_hits("docs/example.md", marker.encode(), self.patterns)
        self.assertEqual(len(hits), 1)

    def test_nul_cannot_turn_a_text_blob_into_an_unscanned_binary(self) -> None:
        marker = "Cl" + "aude generated this working note"
        data = marker.encode() + b"\0hidden suffix"
        hits = guard.blob_hits("docs/example.md", data, self.patterns)
        self.assertEqual(
            hits,
            ["docs/example.md: NUL byte in text/source-like public blob"],
        )
        self.assertEqual(
            guard.blob_hits("brand/icon.png", b"\x89PNG\r\n\0fixture", self.patterns),
            [],
        )

    def test_detector_paths_are_not_blanket_content_exemptions(self) -> None:
        marker = "Cl" + "aude"
        self.assertTrue(
            guard.blob_hits(guard.DENYLIST_PATH, marker.encode(), self.patterns)
        )
        sdk_marker = "UNPUBLISHED " + "PROPRIETARY"
        self.assertTrue(
            guard.blob_hits(
                "tools/check_native_sdk_surface.py",
                sdk_marker.encode(),
                self.patterns,
            )
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
                    stderr=subprocess.PIPE,
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

    def test_merge_resolution_content_is_scanned(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            repo = Path(temporary)
            (repo / "tools").mkdir()
            shutil.copy2(ROOT / guard.DENYLIST_PATH, repo / guard.DENYLIST_PATH)

            def git(*arguments: str) -> str:
                result = subprocess.run(
                    ["git", "-C", str(repo), *arguments],
                    check=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
                return result.stdout.strip()

            git("init", "-q", "-b", "main")
            git("config", "user.name", "Public Test")
            git("config", "user.email", "public-test@example.invalid")
            (repo / "README.md").write_text("public fixture\n", encoding="utf-8")
            git("add", ".")
            git("commit", "-q", "-m", "initial fixture")

            git("checkout", "-q", "-b", "topic")
            (repo / "topic.txt").write_text("topic\n", encoding="utf-8")
            git("add", "topic.txt")
            git("commit", "-q", "-m", "topic")
            git("checkout", "-q", "main")
            (repo / "main.txt").write_text("main\n", encoding="utf-8")
            git("add", "main.txt")
            git("commit", "-q", "-m", "main")
            git("merge", "-q", "--no-commit", "topic")
            private_path = repo / "docs" / "notes" / "SESSION.md"
            private_path.parent.mkdir(parents=True)
            private_path.write_text("merge-only fixture\n", encoding="utf-8")
            git("add", private_path.relative_to(repo).as_posix())
            git("commit", "-q", "-m", "merge fixture")

            merge_commit = git("rev-parse", "HEAD")
            patterns = guard.load_patterns(repo)
            problems = guard.scan_commit(repo, merge_commit, patterns)
            self.assertTrue(
                any("SESSION.md" in problem for problem in problems), problems
            )

    def test_history_scan_catches_content_committed_then_deleted(self) -> None:
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

            git("init", "-q", "-b", "main")
            git("config", "user.name", "Public Test")
            git("config", "user.email", "public-test@example.invalid")
            (repo / "README.md").write_text("public fixture\n", encoding="utf-8")
            git("add", ".")
            git("commit", "-q", "-m", "initial fixture")

            marker = "Cl" + "aude generated transient note"
            (repo / "transient.md").write_text(marker + "\n", encoding="utf-8")
            git("add", "transient.md")
            git("commit", "-q", "-m", "add transient fixture")
            git("rm", "-q", "transient.md")
            git("commit", "-q", "-m", "remove transient fixture")

            patterns = guard.load_patterns(repo)
            self.assertEqual(guard.scan_tree(repo, patterns), [])
            problems = guard.scan_history(repo, "HEAD", patterns)
            self.assertTrue(
                any("transient.md" in problem for problem in problems), problems
            )

    def test_changed_reviewed_history_blob_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            repo = Path(temporary)
            (repo / "tools").mkdir()
            shutil.copy2(ROOT / guard.DENYLIST_PATH, repo / guard.DENYLIST_PATH)

            def git(*arguments: str) -> str:
                result = subprocess.run(
                    ["git", "-C", str(repo), *arguments],
                    check=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
                return result.stdout.strip()

            git("init", "-q", "-b", "main")
            git("config", "user.name", "Public Test")
            git("config", "user.email", "public-test@example.invalid")
            reviewed_path = repo / ".gitattributes"
            reviewed_data = (
                "Cl" + "aude reviewed migration vocabulary\n"
            ).encode()
            reviewed_path.write_bytes(reviewed_data)
            git("add", ".")
            git("commit", "-q", "-m", "reviewed fixture")

            reviewed = {
                (
                    ".gitattributes",
                    hashlib.sha256(reviewed_data).hexdigest(),
                ),
                (
                    guard.DENYLIST_PATH,
                    hashlib.sha256(
                        (repo / guard.DENYLIST_PATH).read_bytes()
                    ).hexdigest(),
                ),
            }
            patterns = guard.load_patterns(repo)
            self.assertEqual(
                guard.scan_history(
                    repo, "HEAD", patterns, exempt_blobs=reviewed
                ),
                [],
            )

            marker = "Cl" + "aude generated changed fixture\n"
            reviewed_path.write_text(marker, encoding="utf-8")
            git("add", ".gitattributes")
            git("commit", "-q", "-m", "change reviewed fixture")
            problems = guard.scan_history(
                repo, "HEAD", patterns, exempt_blobs=reviewed
            )
            self.assertTrue(
                any(".gitattributes" in problem for problem in problems), problems
            )

    def test_release_history_uses_public_remote_or_head(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            repo = Path(temporary)

            def git(*arguments: str) -> str:
                result = subprocess.run(
                    ["git", "-C", str(repo), *arguments],
                    check=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
                return result.stdout.strip()

            git("init", "-q", "-b", "main")
            git("config", "user.name", "Public Test")
            git("config", "user.email", "public-test@example.invalid")
            (repo / "README.md").write_text("fixture\n", encoding="utf-8")
            git("add", ".")
            git("commit", "-q", "-m", "fixture")
            self.assertEqual(guard.release_history_revision(repo), "HEAD")

            git("remote", "add", "public", "example.invalid:public/repo.git")
            with self.assertRaises(guard.GuardError):
                guard.release_history_revision(repo)

            git(
                "update-ref",
                "refs/remotes/public/main",
                git("rev-parse", "HEAD"),
            )
            self.assertEqual(
                guard.release_history_revision(repo),
                "refs/remotes/public/main",
            )

    def test_pre_push_scans_tip_even_when_no_commit_is_outgoing(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            repo = Path(temporary) / "repo"
            repo.mkdir()
            (repo / "tools" / "ci").mkdir(parents=True)
            shutil.copy2(ROOT / guard.DENYLIST_PATH, repo / guard.DENYLIST_PATH)
            shutil.copy2(
                ROOT / "tools" / "check_public_surface.py",
                repo / "tools" / "check_public_surface.py",
            )
            shutil.copy2(
                ROOT / "tools" / "ci" / "check_public_push.sh",
                repo / "tools" / "ci" / "check_public_push.sh",
            )

            def git(*arguments: str) -> str:
                result = subprocess.run(
                    ["git", "-C", str(repo), *arguments],
                    check=True,
                    stdout=subprocess.PIPE,
                    text=True,
                )
                return result.stdout.strip()

            git("init", "-q", "-b", "main")
            git("config", "user.name", "Public Test")
            git("config", "user.email", "public-test@example.invalid")
            (repo / "README.md").write_text("public fixture\n", encoding="utf-8")
            git("add", ".")
            git("commit", "-q", "-m", "initial fixture")
            (repo / "unsafe.md").write_text(
                "Cl" + "aude generated hidden fixture\n", encoding="utf-8"
            )
            git("add", "unsafe.md")
            git("commit", "-q", "-m", "unsafe fixture")
            unsafe_commit = git("rev-parse", "HEAD")

            # Model a new public ref whose commit is already known through a
            # different remote-tracking ref. The outgoing rev-list is empty;
            # only the mandatory tip-tree scan can reject it.
            git("update-ref", "refs/remotes/origin/known", unsafe_commit)
            zero = "0" * 40
            hook_input = (
                f"refs/heads/publish {unsafe_commit} "
                f"refs/heads/publish {zero}\n"
            )
            result = subprocess.run(
                [bash_executable(), "tools/ci/check_public_push.sh", "origin"],
                cwd=repo,
                input=hook_input,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            output = result.stdout + result.stderr
            self.assertEqual(result.returncode, 1, output)
            self.assertIn("unsafe.md", output)
            self.assertNotRegex(
                output,
                r"\[CRASH\]|\[FATAL\]|AddressSanitizer|"
                r"UndefinedBehaviorSanitizer|runtime error:|SIGSEGV|"
                r"Segmentation fault|SIGABRT|Abort trap",
            )


if __name__ == "__main__":
    unittest.main()
