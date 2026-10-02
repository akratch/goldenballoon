#!/usr/bin/env python3
"""Exercise the public guard in disposable repositories; no ROM data required.

Every refusal must be the refusal its fixture provokes: a negative case passes
only on its own FAIL line with no scan error, so a guard that crashed in the
branch under test cannot pass for it. Git faults are injected by running the
guard in a child interpreter with its Git command replaced by a Python wrapper,
which works on Windows too (an extensionless "git" script on PATH does not:
CreateProcess finds git.exe). Filenames Windows cannot hold are POSIX-only.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GUARD = ROOT / "tools/check_clean_room.sh"
TOOLS = ROOT / "tools"
GIT = shutil.which("git")
IGNORE = "\n".join(("*.z64", "*.n64", "*.v64", "*.zip", "*.7z", "*.rar",
                     "save/", "*.ppm", "mods/", "mod-texture-dump/")) + "\n"
SCAN_ERROR = "check_clean_room: scan error"
# Runs tools/check_clean_room.py's run() with GIT optionally replaced.
LAUNCH = ("import sys\n"
          "sys.path.insert(0, sys.argv[1])\n"
          "import check_clean_room as guard\n"
          "if len(sys.argv) > 2:\n"
          "    guard.GIT = (sys.executable, sys.argv[2])\n"
          "sys.exit(guard.run())\n")
# Stands in for Git: fails one subcommand, or corrupts cat-file's output, and
# otherwise hands the call to the real Git with the same standard streams.
WRAPPER = (
    "import os, subprocess, sys\n"
    "mode = os.environ['FAIL_GIT_COMMAND']\n"
    "real = os.environ['REAL_GIT']\n"
    "args = sys.argv[1:]\n"
    "if args[0] == 'cat-file' and mode == 'metadata-bad' and '--batch' not in args:\n"
    "    sys.stdin.buffer.read(); print('invalid metadata'); sys.exit(0)\n"
    "if args[0] == 'cat-file' and '--batch' in args and mode.startswith('batch-'):\n"
    "    oid = sys.stdin.buffer.readline().strip()\n"
    "    if mode == 'batch-short':\n"
    "        size = subprocess.check_output([real, 'cat-file', '-s', oid.decode()]).strip()\n"
    "        sys.stdout.buffer.write(oid + b' blob ' + size + b'\\n')\n"
    "        sys.stdout.buffer.flush()\n"
    "    sys.exit(0 if mode == 'batch-short' else 73)\n"
    "if args[0] == mode:\n"
    "    print('injected Git failure', file=sys.stderr); sys.exit(73)\n"
    "sys.exit(subprocess.call([real, *args]))\n")
POSIX_ONLY = unittest.skipIf(os.name == "nt", "Windows filenames cannot hold a newline")


class CleanRoomGuardTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="mdkr-clean-room-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self.env = {k: v for k, v in os.environ.items()
                    if not k.startswith("GIT_") and k != "CLEAN_ROOM_MAX_BLOB"}
        self.env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
        self.git("init", "-q")
        (self.repo / ".gitignore").write_text(IGNORE)
        (self.repo / "source.txt").write_text("authored fixture\n" * 100)
        (self.repo / "empty.txt").touch()
        self.commit()

    def git(self, *args):
        return subprocess.run([GIT, *args], cwd=self.repo, env=self.env,
                              check=True, capture_output=True)

    def commit(self):
        self.git("add", "-A")
        self.git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                 "-c", "commit.gpgsign=false", "commit", "-qm", "fixture")

    def run_guard(self, failure=None):
        env = self.env.copy()
        command = [sys.executable, "-c", LAUNCH, str(TOOLS)]
        if failure:
            wrapper = self.root / "git_wrapper.py"
            wrapper.write_text(WRAPPER)
            command.append(str(wrapper))
            env.update(FAIL_GIT_COMMAND=failure, REAL_GIT=GIT)
        return subprocess.run(command, cwd=self.repo, env=env,
                              text=True, capture_output=True, timeout=60)

    def assert_refused(self, result, label):
        """Refused for this fixture's reason: its FAIL line, no scan error."""
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn(f"FAIL: {label}\n", result.stdout, output)
        self.assertNotIn(SCAN_ERROR, result.stderr, output)
        self.assertNotIn("check_clean_room: PASS", result.stdout, output)

    def assert_scan_error(self, result):
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn(SCAN_ERROR, result.stderr, output)
        self.assertNotIn("check_clean_room: PASS", result.stdout, output)

    def test_clean(self):
        result = self.run_guard()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("check_clean_room: PASS", result.stdout)

    @unittest.skipIf(os.name == "nt" or shutil.which("bash") is None,
                     "the shell entry point is for POSIX CI")
    def test_shell_entry_point(self):
        result = subprocess.run(["bash", str(GUARD)], cwd=self.repo, env=self.env,
                                text=True, capture_output=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("check_clean_room: PASS", result.stdout)

    def test_git_failures_never_pass(self):
        for command in ("ls-files", "log", "rev-list", "cat-file"):
            with self.subTest(command=command):
                self.assert_scan_error(self.run_guard(command))

    def test_pass_through_wrapper_keeps_a_clean_repository_clean(self):
        # The failure cases above prove the wrapper is the Git the guard runs;
        # this one proves it changes nothing when it injects nothing.
        result = self.run_guard("no-such-subcommand")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("check_clean_room: PASS", result.stdout)

    def test_filename_is_never_shell_code(self):
        (self.repo / "$(touch injected).txt").write_text("safe fixture")
        result = self.run_guard()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.repo / "injected").exists())

    def test_missing_or_truncated_blob_output_never_passes(self):
        for mode in ("metadata-bad", "batch-short", "batch-fail"):
            with self.subTest(mode=mode):
                self.assert_scan_error(self.run_guard(mode))

    def test_all_header_orders_even_in_small_synthetic_blobs(self):
        for index, magic in enumerate(("80371240", "37804012", "40123780")):
            with self.subTest(magic=magic):
                # Only the four-byte signature, not a ROM or ROM-derived data.
                (self.repo / f"header{index}.bin").write_bytes(bytes.fromhex(magic))
                self.commit()
                result = self.run_guard()
                self.assert_refused(result, "ROM header in historical blob")
                oid = self.git("hash-object", f"header{index}.bin").stdout.decode().strip()
                self.assertIn(oid, result.stdout)

    def test_merge_only_historical_dump_is_rejected(self):
        branch = self.git("branch", "--show-current").stdout.decode().strip()
        self.git("checkout", "-qb", "side")
        (self.repo / "side.txt").write_text("side")
        self.commit()
        self.git("checkout", "-q", branch)
        (self.repo / "main.txt").write_text("main")
        self.commit()
        self.git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                 "merge", "--no-commit", "--no-ff", "side")
        payload = self.repo / ("b" * 32 + ".texels")
        payload.write_text("synthetic merge-only filename fixture")
        self.commit()
        payload.unlink()
        self.commit()
        result = self.run_guard()
        self.assert_refused(result, "dumped texture payload tracked or in history")
        self.assertIn("b" * 32 + ".texels", result.stdout)

    def test_force_added_pack_is_rejected(self):
        folder = self.repo / "mods"
        folder.mkdir()
        (folder / "authored.txt").write_text("synthetic package marker")
        self.git("add", "-f", "mods/authored.txt")
        result = self.run_guard()
        self.assert_refused(result, "content-pack payload tracked or in history")
        self.assertIn("mods/authored.txt", result.stdout)

    def test_missing_ignore_rule_is_rejected(self):
        (self.repo / ".gitignore").write_text(IGNORE.replace("*.zip\n", ""))
        result = self.run_guard()
        self.assert_refused(result, "required ignore patterns missing")
        self.assertIn("'*.zip'", result.stdout)

    def test_deleted_historical_dump_is_rejected(self):
        payload = self.repo / ("a" * 32 + ".texels")
        payload.write_text("synthetic placeholder, not cartridge data")
        self.commit()
        payload.unlink()
        self.commit()
        result = self.run_guard()
        self.assert_refused(result, "dumped texture payload tracked or in history")
        self.assertIn("a" * 32 + ".texels", result.stdout)

    def large_untracked_file_is_rejected(self, name):
        payload = self.repo / name
        with payload.open("wb") as stream:
            stream.truncate(4194305)
        result = self.run_guard()
        self.assert_refused(result, "unignored untracked files over 4 MiB")
        self.assertIn(repr(name), result.stdout)

    def test_large_untracked_odd_filename_is_rejected(self):
        self.large_untracked_file_is_rejected("space quote' file.bin")

    @POSIX_ONLY
    def test_large_untracked_newline_filename_is_rejected(self):
        self.large_untracked_file_is_rejected("space quote' newline\nfile.bin")

    def test_untracked_symbolic_links_are_reported_not_followed(self):
        large = self.root / "outside.bin"
        with large.open("wb") as stream:
            stream.truncate(4194305)
        try:
            os.symlink(self.root / "deleted-capture", self.repo / "dangling")
            os.symlink(large, self.repo / "large-link")
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"symbolic links unavailable here: {error}")
        result = self.run_guard()
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("check_clean_room: PASS", result.stdout)
        self.assertIn("WARN: untracked symbolic link 'dangling'", result.stdout)
        self.assertIn("(missing target)", result.stdout)
        self.assertIn("WARN: untracked symbolic link 'large-link'", result.stdout)


if __name__ == "__main__":
    unittest.main()
