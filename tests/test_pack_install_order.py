#!/usr/bin/env python3
"""Source-order guard only; actual pending-install UI input needs native QA."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "platform/app/ui_launcher.cpp"
CALL = "servicePackInstall(state_.quitRequested || action.type == LauncherActionType::Quit);"
DRAW = "LauncherAction Launcher::draw(AppHost &host) {"


def valid_order(source):
    if source.count(DRAW) != 1:
        return False
    draw = source.split(DRAW, 1)[1].split("\nvoid Launcher::setBootError", 1)[0]
    content = source.split("void drawContentPanel(LauncherState &s, LauncherAction &out) {", 1)
    if len(content) != 2:
        return False
    # The Content function occurs before the root draw. No publication service
    # belongs in any panel helper between them.
    if "servicePackInstall(" in content[1].split(DRAW, 1)[0]:
        return False
    if draw.count("servicePackInstall(") != 2 or draw.count(CALL) != 2:
        return False
    if draw.count("return action;") != 2:
        return False
    first_return = draw.index("return action;")
    first_call = draw.index(CALL)
    second_call = draw.rindex(CALL)
    return (draw.index("drawLobbyTakeover(state_, action);") < first_call < first_return
            < draw.index("drawActivePanel(active_, state_, action);")
            < second_call < draw.rindex("return action;"))


class PackInstallOrderTests(unittest.TestCase):
    def test_production_order(self):
        self.assertTrue(valid_order(SOURCE.read_text()))

    def test_early_publication_is_rejected(self):
        source = SOURCE.read_text()
        before, call, after = source.rpartition(CALL)
        mutated = (before + after).replace(DRAW, DRAW + "\n" + call, 1)
        self.assertFalse(valid_order(mutated))

    def test_panel_publication_is_rejected(self):
        source = SOURCE.read_text().replace(
            "void drawContentPanel(LauncherState &s, LauncherAction &out) {",
            "void drawContentPanel(LauncherState &s, LauncherAction &out) {\nservicePackInstall(false);", 1)
        self.assertFalse(valid_order(source))

    def test_quit_and_takeover_paths_cannot_be_dropped(self):
        source = SOURCE.read_text()
        self.assertFalse(valid_order(source.replace(CALL, "servicePackInstall(false);", 1)))
        self.assertFalse(valid_order(source.replace(CALL, "", 1)))


if __name__ == "__main__":
    unittest.main()
