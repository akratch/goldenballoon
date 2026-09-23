#!/usr/bin/env python3
"""Compile the production sequence-switch helper with a muted audio boundary.

This checks retry bookkeeping, not native menu input or audible playback.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MutedMusicTests(unittest.TestCase):
    def test_muted_switch_retries_after_unmute(self):
        source = (ROOT / "game/src/menu.c").read_text()
        begin = source.index("static void charselect_music_sequence_set(s32 sequence) {")
        end = source.index("\nstatic s32 charselect_music_observe", begin)
        helper = source[begin:end]
        fixture = r'''
#include <assert.h>
typedef int s32;
#define SEQUENCE_CHOOSE_YOUR_RACER 26
#define NUM_CHARACTERS 10
#define MDKR_TRACE(...) ((void)0)
static int volume = 256, loaded = 26, requests;
static int sCharselectMusicSequence = 26;
static struct { int channelIndex, unk1, unk2; }
    gMenuSelectedCharacter, D_801263B8, gMenuCurrentCharacter;
static int gCharacterVolumes[10][2];
static int music_volume_config(void) { return volume; }
static void music_change_on(void) {}
static void music_change_off(void) {}
static void music_channel_reset_all(void) {}
static void music_channel_off(int channel) { (void)channel; }
static void music_play(int sequence) {
    requests++;
    if (volume) loaded = sequence;
}
'''
        fixture += helper + r'''
int main(void) {
    volume = 0;
    charselect_music_sequence_set(27);
    assert(loaded == 26);
    assert(sCharselectMusicSequence == 26);
    volume = 256;
    charselect_music_sequence_set(27);
    assert(loaded == 27 && sCharselectMusicSequence == 27);
    volume = 0;
    charselect_music_sequence_set(26);
    assert(loaded == 27 && sCharselectMusicSequence == 27);
    volume = 256;
    charselect_music_sequence_set(26);
    assert(loaded == 26 && sCharselectMusicSequence == 26);
    int before = requests;
    charselect_music_sequence_set(26);
    assert(requests == before);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="mdkr-taj-muted-") as temp:
            root = Path(temp)
            (root / "fixture.c").write_text(fixture)
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall",
                            "-Wextra", "-Wno-unused-function", str(root / "fixture.c"),
                            "-o", str(root / "fixture")], check=True, capture_output=True)
            result = subprocess.run([str(root / "fixture")], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
