#!/usr/bin/env python3
"""Exercise the public guard in disposable repositories; no ROM data required."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GUARD = ROOT / "tools/check_clean_room.sh"
GIT = shutil.which("git")
IGNORE = "\n".join(("*.z64", "*.n64", "*.v64", "*.zip", "*.7z", "*.rar",
                     "save/", "*.ppm", "mods/", "mod-texture-dump/")) + "\n"


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
        if failure:
            bin_dir = self.root / "bin"
            bin_dir.mkdir(exist_ok=True)
            wrapper = bin_dir / "git"
            wrapper.write_text(
                "#!/usr/bin/env python3\nimport os,sys,subprocess\n"
                "mode=os.environ['FAIL_GIT_COMMAND']\n"
                "if sys.argv[1]=='cat-file' and mode=='metadata-bad' and '--batch' not in sys.argv:\n"
                " sys.stdin.buffer.read(); print('invalid metadata'); sys.exit(0)\n"
                "if sys.argv[1]=='cat-file' and '--batch' in sys.argv and mode.startswith('batch-'):\n"
                " oid=sys.stdin.buffer.readline().strip()\n"
                " if mode=='batch-short':\n"
                "  size=subprocess.check_output([os.environ['REAL_GIT'],'cat-file','-s',oid.decode()]).strip()\n"
                "  sys.stdout.buffer.write(oid+b' blob '+size+b'\\n'); sys.stdout.buffer.flush()\n"
                " sys.exit(0 if mode=='batch-short' else 73)\n"
                "if sys.argv[1] == os.environ['FAIL_GIT_COMMAND']:\n"
                " print('injected Git failure', file=sys.stderr); sys.exit(73)\n"
                "os.execv(os.environ['REAL_GIT'], [os.environ['REAL_GIT'], *sys.argv[1:]])\n")
            wrapper.chmod(0o755)
            env.update(PATH=str(bin_dir) + os.pathsep + env["PATH"],
                       FAIL_GIT_COMMAND=failure, REAL_GIT=GIT)
        return subprocess.run(["bash", str(GUARD)], cwd=self.repo, env=env,
                              text=True, capture_output=True, timeout=60)

    def test_clean(self):
        result = self.run_guard()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("check_clean_room: PASS", result.stdout)

    def test_git_failures_never_pass(self):
        for command in ("ls-files", "log", "rev-list", "cat-file"):
            with self.subTest(command=command):
                result = self.run_guard(command)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertNotIn("check_clean_room: PASS", result.stdout)

    def test_filename_is_never_shell_code(self):
        (self.repo / "$(touch injected).txt").write_text("safe fixture")
        result = self.run_guard()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.repo / "injected").exists())

    def test_missing_or_truncated_blob_output_never_passes(self):
        for mode in ("metadata-bad", "batch-short", "batch-fail"):
            with self.subTest(mode=mode):
                result = self.run_guard(mode)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertNotIn("check_clean_room: PASS", result.stdout)

    def test_all_header_orders_even_in_small_synthetic_blobs(self):
        for index, magic in enumerate(("80371240", "37804012", "40123780")):
            with self.subTest(magic=magic):
                # Only the four-byte signature, not a ROM or ROM-derived data.
                (self.repo / f"header{index}.bin").write_bytes(bytes.fromhex(magic))
                self.commit()
                result = self.run_guard()
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("ROM header in historical blob", result.stdout)
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
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_force_added_pack_is_rejected(self):
        folder = self.repo / "mods"
        folder.mkdir()
        (folder / "authored.txt").write_text("synthetic package marker")
        self.git("add", "-f", "mods/authored.txt")
        result = self.run_guard()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_missing_ignore_rule_is_rejected(self):
        (self.repo / ".gitignore").write_text(IGNORE.replace("*.zip\n", ""))
        result = self.run_guard()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_deleted_historical_dump_is_rejected(self):
        payload = self.repo / ("a" * 32 + ".texels")
        payload.write_text("synthetic placeholder, not cartridge data")
        self.commit()
        payload.unlink()
        self.commit()
        result = self.run_guard()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_large_untracked_odd_filename_is_rejected(self):
        payload = self.repo / "space quote' newline\nfile.bin"
        with payload.open("wb") as stream:
            stream.truncate(4194305)
        result = self.run_guard()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
