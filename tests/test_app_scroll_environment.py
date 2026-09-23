#!/usr/bin/env python3
"""ROM/window-free controls for the scroll harness's launch boundary."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import check_app_scroll


class ScrollEnvironmentTests(unittest.TestCase):
    def test_missing_empty_and_relative_pins_never_launch(self):
        for environment in ({}, {"MDKR_SAVE_DIR": ""}, {"MDKR_SAVE_DIR": "relative"}):
            with self.subTest(environment=environment), \
                 patch.dict("os.environ", {"MDKR_SAVE_DIR": "/inherited-not-a-fixture"}), \
                 patch.object(check_app_scroll.subprocess, "run") as launch:
                with self.assertRaisesRegex(ValueError, "explicit absolute MDKR_SAVE_DIR"):
                    check_app_scroll.run(Path("unused-engine"), environment, ())
                launch.assert_not_called()

    def test_private_fixture_survives_scrub_without_inherited_controls(self):
        with tempfile.TemporaryDirectory(prefix="mdkr-scroll-env-") as temporary:
            root = Path(temporary).resolve()
            environment = check_app_scroll.common(root, root / "video.ini")
            completed = subprocess.CompletedProcess([], 0, "fixture marker")
            with patch.dict("os.environ", {"MDKR_SAVE_DIR": "/wrong",
                                           "MDKR_UNRELATED": "poison",
                                           "GE007_UNRELATED": "poison"}), \
                 patch.object(check_app_scroll.subprocess, "run", return_value=completed) as launch:
                self.assertEqual(check_app_scroll.run(Path("unused-engine"), environment,
                                                     ("fixture marker",)), "fixture marker")
            child = launch.call_args.kwargs["env"]
            self.assertEqual(child["MDKR_SAVE_DIR"], str(root / "saves"))
            self.assertEqual(child["MDKR_APP_PREFS_DIR"], str(root / "prefs"))
            self.assertEqual(child["MDKR_VIDEO_CONFIG_PATH"], str(root / "video.ini"))
            self.assertNotIn("MDKR_UNRELATED", child)
            self.assertNotIn("GE007_UNRELATED", child)
            self.assertTrue((root / "saves").is_dir())


if __name__ == "__main__":
    unittest.main()
