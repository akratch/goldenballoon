#!/usr/bin/env python3
"""Selected-build routing and early artifact/dependency admission; no app I/O."""

from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import run_checks  # noqa: E402


class NativeBrowserArtifacts(unittest.TestCase):
    def setUp(self) -> None:
        self.native = Path("selected build") / "mdkr64"
        self.release = Path("different release") / "mdkr64"
        self.asan = Path("different asan") / "mdkr64"
        self.rom = Path("private fixture.z64")
        self.wasm = Path("linked wasm") / "mdkr64_web.wasm"

    def checks(self):
        return [check for check in run_checks.CHECKS
                if check.name in run_checks.NATIVE_BROWSER_DRIVERS]

    def command(self, check):
        return run_checks.command_for(
            check, self.native, self.release, self.asan,
            self.rom, Path("rom directory"), self.wasm, True)

    def test_all_three_use_selected_build_and_staged_shell(self):
        checks = self.checks()
        self.assertEqual({check.name for check in checks},
                         {"party_native_e2e", "party_lan_e2e",
                          "online_live_transport_e2e"})
        for check in checks:
            with self.subTest(check=check.name):
                self.assertEqual(check.role, "browser_local")
                self.assertTrue(run_checks.is_serial(check))
                command = self.command(check)
                self.assertEqual(command.count("--build"), 1)
                self.assertEqual(command[command.index("--build") + 1],
                                 str(self.native.parent))
                self.assertEqual(command[command.index("--shell-dir") + 1],
                                 str(run_checks.ROOT / "dist" / "web"))
                self.assertNotIn(str(self.release), command)
                self.assertNotIn(str(self.asan), command)
                suffix = ".exe" if os.name == "nt" else ""
                self.assertEqual(run_checks.native_browser_driver(check, self.native),
                                 self.native.parent /
                                 (run_checks.NATIVE_BROWSER_DRIVERS[check.name] + suffix))

    def test_missing_driver_is_reported_before_any_later_preflight_io(self):
        for check in self.checks():
            with self.subTest(check=check.name):
                driver = run_checks.native_browser_driver(check, self.native)
                with patch.object(Path, "is_file", lambda path: path != driver), \
                        patch.object(Path, "read_text", side_effect=AssertionError("late I/O")), \
                        patch.object(run_checks, "report_sdl_flavor",
                                     side_effect=AssertionError("loader execution")):
                    with self.assertRaises(RuntimeError) as error:
                        run_checks.preflight([check], self.native, self.release,
                                             self.asan, self.rom, self.wasm)
                self.assertIn(check.name + " native driver", str(error.exception))
                self.assertIn(str(driver), str(error.exception))

    def test_driver_suffix_for_both_host_families(self):
        # Replace only the runner's module binding, not global os.name: pathlib
        # must keep using the actual host's concrete Path implementation.
        for platform, suffix in (("posix", ""), ("nt", ".exe")):
            with patch.object(run_checks, "os", SimpleNamespace(name=platform)):
                for check in self.checks():
                    self.assertEqual(
                        run_checks.native_browser_driver(check, self.native),
                        self.native.parent /
                        (run_checks.NATIVE_BROWSER_DRIVERS[check.name] + suffix))

    def test_gallery_uses_selected_binary_and_staged_shell(self):
        check = next(check for check in run_checks.CHECKS
                     if check.name == "browser_online_room_gallery")
        self.assertEqual(check.role, "browser_local")
        self.assertTrue(run_checks.is_serial(check))
        self.assertIsNone(run_checks.native_browser_driver(check, self.native))
        self.assertEqual(run_checks.yield_wrapper(check), [])
        for name in ("mdkr64", "mdkr64.exe", "custom game.exe"):
            with self.subTest(binary=name):
                self.native = Path("selected build") / name
                command = self.command(check)
                self.assertEqual(command.count("--build"), 1)
                self.assertEqual(command[command.index("--build") + 1], str(self.native))
                self.assertEqual(command.count("--shell-dir"), 1)
                self.assertEqual(command[command.index("--shell-dir") + 1],
                                 str(run_checks.ROOT / "dist" / "web"))
                self.assertNotIn(str(self.release), command)
                self.assertNotIn(str(self.asan), command)

    def test_missing_gallery_binary_is_reported_before_later_preflight_io(self):
        check = next(check for check in run_checks.CHECKS
                     if check.name == "browser_online_room_gallery")
        with patch.object(Path, "is_file", lambda path: path != self.native), \
                patch.object(Path, "read_text", side_effect=AssertionError("late I/O")), \
                patch.object(run_checks, "report_sdl_flavor",
                             side_effect=AssertionError("loader execution")):
            with self.assertRaises(RuntimeError) as error:
                run_checks.preflight([check], self.native, self.release,
                                     self.asan, self.rom, self.wasm)
        self.assertIn("browser_online_room_gallery native gallery binary",
                      str(error.exception))
        self.assertIn(str(self.native), str(error.exception))

    def test_other_browser_local_checks_keep_their_own_arguments(self):
        checks = [check for check in run_checks.CHECKS
                  if check.role == "browser_local" and
                  check.name not in run_checks.NATIVE_BROWSER_DRIVERS and
                  check.name not in run_checks.NATIVE_BROWSER_BINARIES]
        self.assertTrue(checks)
        for check in checks:
            with self.subTest(check=check.name):
                self.assertIsNone(run_checks.native_browser_driver(check, self.native))
                self.assertEqual(self.command(check).count("--build"),
                                 list(check.args).count("--build"))

    def test_new_source_contracts_remain_ctest_owned(self):
        for name in ("check_ai_difficulty_ui.py", "check_match_transport_tls_io.py",
                     "check_online_resolver_budget.py"):
            self.assertIn(name, run_checks.CTEST_COMPANION_SCRIPTS)
            self.assertIn(name, run_checks.cmake_registered_test_scripts())

    def test_worker_dependency_inventory_matches_the_seven_local_service_gates(self):
        expected = {"party_capacity", "party_experience_canary_smoke",
                    "party_service_chaos", "party_native_e2e",
                    "online_live_transport_e2e", "browser_online_two_person",
                    "party_firewall_negative"}
        self.assertEqual(run_checks.PARTY_WORKER_CHECKS, expected)
        self.assertEqual({check.name for check in run_checks.CHECKS
                          if check.name in expected}, expected)
        self.assertEqual(run_checks.PARTY_WRANGLER,
                         run_checks.ROOT / "services/party/node_modules/wrangler/bin/wrangler.js")
        self.assertNotIn("party_lan_e2e", run_checks.PARTY_WORKER_CHECKS)

    def test_missing_worker_dependency_refuses_before_later_io_or_subprocess(self):
        checks = [check for check in run_checks.CHECKS
                  if check.name in run_checks.PARTY_WORKER_CHECKS]
        # Check each standalone selection and the combined selection; the
        # latter reports a shared missing dependency only once, with consumers.
        for selection in [[check] for check in checks] + [checks]:
            with self.subTest(checks=[check.name for check in selection]), \
                    patch.object(Path, "is_file", lambda path: path != run_checks.PARTY_WRANGLER), \
                    patch.object(Path, "read_text", side_effect=AssertionError("late I/O")), \
                    patch.object(run_checks.subprocess, "run",
                                 side_effect=AssertionError("subprocess execution")), \
                    patch.object(run_checks, "report_sdl_flavor",
                                 side_effect=AssertionError("loader execution")):
                with self.assertRaises(RuntimeError) as caught:
                    run_checks.preflight(selection, self.native, self.release,
                                         self.asan, self.rom, self.wasm)
                message = str(caught.exception)
                self.assertEqual(message.count(str(run_checks.PARTY_WRANGLER)), 1)
                self.assertIn("package-lock.json", message)
                self.assertIn("npm ci", message)
                for check in selection:
                    self.assertIn(check.name, message)

    def test_worker_admission_is_selection_scoped_and_does_not_block_lan(self):
        for name in ("party_lan_e2e", "browser_online_room_gallery", "party_capacity"):
            # With its dependency present the Worker check must proceed to the
            # normal stage guard; non-Worker checks do so even when it is absent.
            worker_selected = name in run_checks.PARTY_WORKER_CHECKS
            check = next(check for check in run_checks.CHECKS if check.name == name)
            with self.subTest(check=name), \
                    patch.object(Path, "is_file",
                                 lambda path: worker_selected or path != run_checks.PARTY_WRANGLER), \
                    patch.object(Path, "read_text", side_effect=AssertionError("stage reached")) as stage, \
                    patch.object(run_checks.subprocess, "run",
                                 side_effect=AssertionError("subprocess execution")):
                with self.assertRaisesRegex(AssertionError, "stage reached"):
                    run_checks.preflight([check], self.native, self.release,
                                         self.asan, self.rom, self.wasm)
                stage.assert_called_once_with(encoding="utf-8")

    def test_missing_driver_and_worker_are_reported_together(self):
        check = next(check for check in run_checks.CHECKS if check.name == "party_native_e2e")
        driver = run_checks.native_browser_driver(check, self.native)
        with patch.object(Path, "is_file",
                          lambda path: path not in {driver, run_checks.PARTY_WRANGLER}), \
                patch.object(Path, "read_text", side_effect=AssertionError("late I/O")):
            with self.assertRaises(RuntimeError) as caught:
                run_checks.preflight([check], self.native, self.release,
                                     self.asan, self.rom, self.wasm)
        self.assertIn(str(driver), str(caught.exception))
        self.assertIn(str(run_checks.PARTY_WRANGLER), str(caught.exception))


class PreviousReleaseReferenceRouting(unittest.TestCase):
    """--reference-build reaches only the gate that needs it; only it may skip."""

    def setUp(self) -> None:
        self.gate = next(check for check in run_checks.CHECKS
                         if check.name == "previous_release_parity")
        self.reference = Path("previous release") / "mdkr64"

    def command(self, check, reference):
        return run_checks.command_for(
            check, Path("native") / "mdkr64", Path("release") / "mdkr64",
            Path("asan") / "mdkr64", Path("rom.z64"), Path("roms"),
            Path("web.wasm"), True, reference)

    def test_the_reference_is_forwarded_once_to_the_parity_gate(self):
        command = self.command(self.gate, self.reference)
        self.assertEqual(command.count("--reference-build"), 1)
        self.assertEqual(command[command.index("--reference-build") + 1],
                         str(self.reference))
        self.assertEqual(command[command.index("--build") + 1],
                         str(Path("release") / "mdkr64"))
        self.assertIn("--roms", command)

    def test_without_a_reference_the_gate_is_invoked_without_one(self):
        self.assertNotIn("--reference-build", self.command(self.gate, None))

    def test_no_other_check_receives_the_reference(self):
        for check in run_checks.CHECKS:
            if check is self.gate or check.role == "ctest":
                continue
            with self.subTest(check=check.name):
                self.assertNotIn("--reference-build",
                                 self.command(check, self.reference))

    def test_only_the_named_gate_may_skip(self):
        self.assertTrue(run_checks.was_skipped(self.gate, run_checks.SKIP_EXIT))
        self.assertFalse(run_checks.was_skipped(self.gate, 0))
        self.assertFalse(run_checks.was_skipped(self.gate, 1))
        other = next(check for check in run_checks.CHECKS if check.name == "state_hash")
        self.assertFalse(run_checks.was_skipped(other, run_checks.SKIP_EXIT))


if __name__ == "__main__":
    unittest.main()
