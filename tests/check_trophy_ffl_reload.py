#!/usr/bin/env python3
"""Earn FFL gold, then display it from unchanged EEPROM in a fresh process.

Other-world golds and completed campaign prerequisites are fixture premises.
All Taj offers are already seen so a pending offer cannot turn the ground
navigation into a plane route. Championship timing/order use existing test seams.
"""
import argparse
from pathlib import Path

from check_adventure_two import CUTSCENE_ADVENTURE_TWO
from check_campaign_progression import Slot, decode
from check_first_boss_progression import save_order
from check_future_fun_land import campaign_checkpoint, drive_to_signpost
from check_trophy_series import AWARD_RE, BAD_RE, GOLD_ORDER, ROUND_RE, run_case
from harness_utils import DEFAULT_BUILD_DIR, resolve_binary


def verify_save(save):
    fields = decode(save)
    if not fields["checksum_ok"] or fields["trophies"] != 0x3ff:
        raise ValueError("invalid checksum or lost earned trophy bits")


def verify_reload(output):
    if BAD_RE.search(output) or "quarantined" in output:
        raise ValueError("native failure or quarantined save")
    if "level_load: levelId=35 numPlayers=0" not in output:
        raise ValueError("fresh process never reached Future Fun Land")
    if "trophycabinet: DISPLAY world=5 state=3 trophies=0x3ff" not in output:
        raise ValueError("fresh process did not display earned FFL gold")


def run_mode(binary, rom, adventure_two):
    eligible = save_order(str(rom))
    slot = Slot.from_save(campaign_checkpoint(rom, eligible), eligible)
    slot.trophies = 0xff  # FFL trophy deliberately absent.
    slot.taj = 7  # Offered, not beaten; no pending automatic transformation.
    if adventure_two:
        slot.cutscenes |= CUTSCENE_ADVENTURE_TWO
    else:
        slot.cutscenes &= ~CUTSCENE_ADVENTURE_TWO
    proc, produced = run_case(binary, rom, 13000, slot.image(eligible),
                              "retry", GOLD_ORDER, 5, adventure_two=adventure_two)
    if proc.returncode or BAD_RE.search(proc.stdout):
        raise RuntimeError(proc.stdout[-5000:])
    tracks = [int(m[3]) for m in ROUND_RE.finditer(proc.stdout)][:4]
    if tracks != [17, 32, 33, 15]:
        raise ValueError(f"wrong championship schedule: {tracks}")
    for track in tracks:
        mode = int(adventure_two)
        if f"adventure_mode: level={track} adventureTwo={mode} mirrored={mode}" not in proc.stdout:
            raise ValueError(f"wrong Adventure mode on track {track}")
    award = AWARD_RE.search(proc.stdout)
    if award is None or tuple(int(award[i], 16 if i in (4, 5) else 10)
                              for i in (1, 2, 3, 4, 5)) != (5, 0, 36, 0xff, 0x3ff):
        raise ValueError("missing production FFL gold upgrade")
    verify_save(produced)
    try:
        verify_save(slot.image(eligible))
    except ValueError:
        pass
    else:
        raise AssertionError("unearned FFL trophy passed the save oracle")
    # No save editing between the two processes.
    rc, output, reloaded = drive_to_signpost(
        binary, rom, "earned-gold-reload", produced, 300,
        adventure_two=adventure_two)
    if rc or BAD_RE.search(output) or "quarantined" in output:
        raise RuntimeError(output[-5000:])
    verify_reload(output)
    for marker in ("level_load: levelId=35 numPlayers=0",
                   "trophycabinet: DISPLAY world=5 state=3 trophies=0x3ff"):
        try:
            verify_reload(output.replace(marker, "removed witness"))
        except ValueError:
            pass
        else:
            raise AssertionError("missing reload witness was accepted")
    verify_save(reloaded)
    for save in (produced, reloaded):
        actual = bool(Slot.from_save(save, eligible).cutscenes & CUTSCENE_ADVENTURE_TWO)
        if actual != adventure_two:
            raise ValueError("campaign mode changed in persisted save")
    print(f"check_trophy_ffl_reload: PASS — Adventure {2 if adventure_two else 1} earned gold survives fresh-process cabinet load", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", type=Path, required=True)
    args = parser.parse_args()
    for mode in (False, True):
        run_mode(Path(resolve_binary(args.build)).resolve(), args.rom.resolve(), mode)


if __name__ == "__main__":
    main()
