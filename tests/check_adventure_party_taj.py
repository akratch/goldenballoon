#!/usr/bin/env python3
"""AP-11 focused route: the Taj transaction and the shared-scene envelope.

What this proves (Enhancements.AdventureParty ON, a party in the central hub)
---------------------------------------------------------------------------
* NON-HOST summon: seat 1 (player two) summons Taj by driving into him -- any
  occupied seat may trigger the interaction (TRIGGER_TRANSITION authority), not
  just player one. The shared dialogue latches once (ACTIVE_LOBBY ->
  SHARED_DIALOGUE).
* HOST dialogue authority: player one owns the menu regardless of who summoned
  (taj_menu_loop reads input_pressed(PLAYER_ONE)); the vehicle choice is traced
  as a host DIALOGUE_CHOICE.
* WHOLE-PARTY transform: retail transform_player_vehicle rebuilds the roster as
  EXACTLY ONE racer, which would destroy a party. The party path rebuilds ALL N
  racers atomically with the NEW shared vehicle and the SAME seat->character
  identities, and restores the split layout. The transaction's own oracle is
  `aparty_transform path=party n=N live=N` -- live is the post-rebuild gNumRacers,
  so a collapse reads `live=1` (the central failure this gate guards). Exactly
  ONE transform is emitted (the roster is rebuilt once, not repeatedly).
* R10 latch lifecycle: the DIALOGUE latch RELEASES on completion (SHARED_DIALOGUE
  -> ACTIVE_LOBBY at the SAME level generation -- the transform is within-lobby,
  never a reload), so a door works again in the SAME lobby visit. No transition
  fires between the latch and the release: retail freezes all racer input during
  the Taj scene (disable_racer_input), and the arbiter additionally rejects any
  door press while the session is not ACTIVE_LOBBY, so a door cannot bypass the
  SHARED_DIALOGUE state. Doors working FROM ACTIVE_LOBBY is proven by
  check_adventure_party_transition; this gate proves the state is restored to it.
* Identity persistence: post-transform the seat->port->racer binding holds
  (the [RACERINPUT] witness: port == player == racer) and the roster characters
  are unchanged.

* R27 challenge refusal: the in-hub Taj CHALLENGE rows are not classified for a
  party yet (a challenge would spawn a lone Taj-vs-host race and strand the other
  seats). A host CHALLENGE-row selection during the party SHARED_DIALOGUE is
  fail-closed refused (`aparty_taj_challenge: ... refused=1`), no challenge race
  starts (run with MDKR_TAJ_PROBE=1 so `[TAJ] phase=accept` would appear if
  init_racer_for_challenge ran -- it must NOT), no extra racer spawns, the party
  is intact, and a vehicle TRANSFORM still works afterward (the same whole-party
  transform oracle, live==3). The refusal keys on a live party SESSION, never on
  player count, so retail JOINTVENTURE 2P challenges are untouched (the off arm).

Everything is read from aparty_ traces the running binary emits (AP-05 schema)
plus the aparty_transform / aparty_taj_challenge diagnostics; the facts come from
the pure reducer and the game adapters, not from "the process did not crash".

Driving
-------
Seat 1 summons via MDKR_AP_SEAT_ROUTE="1=-197,193" (Taj's hub position -- driving
into him opens the dialogue on contact); the host navigates the menu via the
input script (taj_menu_loop reads PLAYER_ONE input). Only seat 1 drives: a second
moving racer near a lobby door raises a door textbox that blocks npc_dialogue_loop
and (an engine quirk) diverges shell-vs-headless, so the deterministic route keeps
exactly one mover -- the summoner -- and the menu is host input.

Positive controls
-----------------
* Collapse the post-transform roster to one (rewrite live=N -> live=1 in the
  aparty_transform line): the whole-party assertion must FAIL.
* Strip the SHARED_DIALOGUE latch lines: the latch assertion must FAIL.

Off arm
-------
Retail JOINTVENTURE 2P Taj is untouched: tests/check_taj_p2_adventure.py is run
and quoted here (it uses the retail lead-swap machinery a party never engages).

Save fixture: the started Adventure One slot-0 save from
check_adventure_party_admission (the host's FILE_SELECT confirm resumes it).
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary, save_env
from check_adventure_party_admission import eeprom_image

ROOT = Path(__file__).resolve().parent.parent

TAJ_SEAT_ROUTE = "1=-197,193"  # seat 1 drives into Taj (BHV_PARK_WARDEN) to summon
HOVERCRAFT = 1  # the vehicle the host selects (lobby default is CAR=0 -> transforms)

# aparty action / verdict enum values (adventure_party_policy.h), emitted raw.
ACTION_DIALOGUE_CHOICE = 0
ACTION_TRIGGER = 2
VERDICT_LATCHED = 0

TRANSFORM_RE = re.compile(
    r"aparty_transform: path=(\w+) vehicle=(\d+) n=(\d+) live=(\d+)")
SESSION_RE = re.compile(
    r"aparty_session: state=(\w+) sgen=(\d+) lgen=(\d+) host=(\d+)")
INTER_RE = re.compile(
    r"aparty_interaction: seat=(\d+) action=(-?\d+) verdict=(-?\d+)")
ROSTER_RE = re.compile(r"aparty_roster: n=(\d+) mask=0x([0-9a-fA-F]+)((?: c\d+=\d+)*)")
LAYOUT_RE = re.compile(r"aparty_layout: viewports=(\d+) layout=(\d+)")
TRANS_RE = re.compile(r"aparty_transition: seat=(\d+) tick=(\d+)")
SOUND_RE = re.compile(r"aparty_taj_sound: seat=(\d+) reinit=1")
VISIBILITY_RE = re.compile(
    r"aparty_taj_visibility: seat=(\d+) character=(\d+) vehicle=(\d+) "
    r"transparency=(\d+) hidden=(\d+)")
# R27: the in-hub Taj CHALLENGE row is fail-closed refused for a party session.
CHALLENGE_RE = re.compile(r"aparty_taj_challenge: seat=(-?\d+) refused=(\d+) choice=(\d+)")
# [TAJ] phase=accept fires ONLY from init_racer_for_challenge (a challenge race
# actually starting). With MDKR_TAJ_PROBE=1 its ABSENCE proves no race started.
TAJ_ACCEPT_RE = re.compile(r"\[TAJ\] phase=accept\b")
# TAJ_FLAGS_CAR_CHAL_UNLOCKED (game/include/structs.h) -- exposes the CHALLENGES
# row in Taj's hub ROOT menu so the R27 refusal path is reachable.
TAJ_FLAGS_CAR_CHAL_UNLOCKED = 0x01
RACERINPUT_RE = re.compile(
    r"\[RACERINPUT\] tick=(\d+) player=(-?\d+) racer=(\d+) port=(\d+)")
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion failed")


def run_arm(binary, rom, fixture, seat_route, frames=6000, verbose=False,
            eeprom=None, extra_env=None, capture_dir=None, texture_pack=None):
    with tempfile.TemporaryDirectory(prefix="mdkr_ap_taj_") as tmp:
        root = Path(tmp)
        if texture_pack is not None:
            (root / "mods").mkdir()
            shutil.copyfile(texture_pack, root / "mods" / "taj-fixture.zip")
        save_dir = root / "save"
        save_dir.mkdir()
        (save_dir / "eeprom.bin").write_bytes(eeprom if eeprom is not None
                                              else eeprom_image())
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(("MDKR", "GE007_"))}
        env.update(LC_ALL="C", MDKR_AUDIO="0", MDKR_TRACE="1",
                   MDKR_RACER_INPUT_TRACE="1", MDKR_AP_SEAT_ROUTE=seat_route,
                   MDKR_APP_PREFS_DIR=str(root), MDKR64_HIDDEN="1")
        if extra_env:
            env.update(extra_env)
        save_env(env, str(save_dir))
        env["MDKR_VIDEO_CONFIG_PATH"] = str(root / "mdkr64.ini")
        # This arm's own pack folder (absent unless texture_pack was given).
        env["MDKR_MODS_DIR"] = str(root / "mods")
        command = [
            binary, "--headless-frames", str(frames),
            "--input-script", str(ROOT / fixture), "--rom", rom,
            "--window-size", "320x240",
            "--video-set", "Enhancements.AdventureParty=1",
        ]
        if capture_dir is not None:
            capture_dir = Path(capture_dir).resolve()
            capture_dir.mkdir(parents=True, exist_ok=False)
            command += ["--dump-frames", str(capture_dir)]
        if verbose:
            print(f"$ {' '.join(command)}  route={seat_route}", flush=True)
        proc = subprocess.run(command, cwd=root, env=env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=max(240, frames // 8), check=False)
        if capture_dir is not None:
            (capture_dir / "run.log").write_text(proc.stdout or "", encoding="utf-8")
        if proc.returncode != 0:
            raise RuntimeError(f"Taj arm {fixture} exited {proc.returncode}\n"
                               + (proc.stdout or "")[-5000:])
        if texture_pack is not None and not re.search(
                r"\[MODS\] [1-9][0-9]* high-resolution textures? used this run",
                proc.stdout or ""):
            raise RuntimeError("Taj pack arm did not demonstrate replacement texture use")
    return proc.stdout or ""


def find_line(lines, regex, start=0, pred=None):
    for i in range(start, len(lines)):
        m = regex.search(lines[i])
        if m and (pred is None or pred(m)):
            return i, m
    return -1, None


def assert_transform_scene(out, players, label, expected_vehicle=HOVERCRAFT):
    """Returns (failures, transform_line_index). transform_line_index is -1 when
    the scene never reached a clean single party transform."""
    f = []
    if bad := BAD_RE.search(out):
        f.append(f"{label}: fatal marker {bad.group(0)!r}")
        return f, -1
    lines = out.splitlines()

    # --- Non-host summon latches the shared dialogue --------------------------
    si, sm = find_line(lines, INTER_RE, pred=lambda m: int(m.group(2)) == ACTION_TRIGGER
                       and int(m.group(3)) == VERDICT_LATCHED)
    if si < 0:
        f.append(f"{label}: no summon interaction (action={ACTION_TRIGGER} latched)")
        return f, -1
    if int(sm.group(1)) == 0:
        f.append(f"{label}: host summoned; expected a NON-host seat (saw seat {sm.group(1)})")
    li, lm = find_line(lines, SESSION_RE, start=si,
                       pred=lambda m: m.group(1) == "SHARED_DIALOGUE")
    if li < 0:
        f.append(f"{label}: summon did not latch SHARED_DIALOGUE")
        return f, -1
    dialogue_lgen = int(lm.group(3))

    # --- Host owns the dialogue choice ---------------------------------------
    ci, cm = find_line(lines, INTER_RE, start=li,
                       pred=lambda m: int(m.group(2)) == ACTION_DIALOGUE_CHOICE
                       and int(m.group(3)) == VERDICT_LATCHED)
    if ci < 0:
        f.append(f"{label}: no host DIALOGUE_CHOICE (action={ACTION_DIALOGUE_CHOICE})")
    elif int(cm.group(1)) != 0:
        f.append(f"{label}: dialogue choice by seat {cm.group(1)}, expected host seat 0")

    # --- Whole-party transform: exactly one, live == n == N ------------------
    transforms = [(i, m) for i, m in enumerate(TRANSFORM_RE.search(l) for l in lines) if m]
    party = [(i, m) for i, m in transforms if m.group(1) == "party"]
    if len(party) != 1:
        f.append(f"{label}: expected exactly ONE party transform (roster rebuilt once), "
                 f"saw {[m.group(0) for _i, m in transforms]}")
        return f, -1
    ti, tm = party[0]
    veh, n, live = int(tm.group(2)), int(tm.group(3)), int(tm.group(4))
    if n != players or live != players:
        f.append(f"{label}: transform rebuilt live={live} of n={n}; expected the "
                 f"WHOLE party ({players}) -- a collapse reads live=1")
    if veh != expected_vehicle:
        f.append(f"{label}: transformed to vehicle {veh}, expected {expected_vehicle}")

    # roster + layout republished by the transform (after the transform line)
    ri, rm = find_line(lines, ROSTER_RE, start=ti)
    if ri < 0 or int(rm.group(1)) != players or int(rm.group(2), 16) != (1 << players) - 1:
        f.append(f"{label}: transform did not republish the full {players}-seat roster")
    else:
        pre_ri, pre_rm = find_line(lines, ROSTER_RE)
        if pre_rm and pre_rm.group(3) != rm.group(3):
            f.append(f"{label}: characters changed across transform "
                     f"({pre_rm.group(3).strip()} -> {rm.group(3).strip()})")
    yi, ym = find_line(lines, LAYOUT_RE, start=ti,
                       pred=lambda m: int(m.group(1)) == players)
    if yi < 0:
        f.append(f"{label}: transform did not restore the {players}-viewport split layout")

    # Engine sound restored for EVERY seat: the transform freed+rebuilt all N
    # racers with vehicleSound=0, so every seat must be re-initialised at Taj's
    # END_DIALOGUE (retail re-inits only the focus -- seats 1..N-1 would drive
    # silently). One aparty_taj_sound reinit line per seat, after the transform.
    sound_seats = {int(m.group(1)) for m in
                   (SOUND_RE.search(l) for l in lines[ti:]) if m}
    visible = {}
    for line in lines[ti:]:
        match = VISIBILITY_RE.search(line)
        if match:
            seat, character, vehicle, transparency, hidden = map(int, match.groups())
            visible[seat] = (character, vehicle, transparency, hidden)
    if set(visible) != set(range(players)):
        f.append(f"{label}: missing per-seat post-dialogue visibility: {visible}")
    if any(value[1:] != (expected_vehicle, 255, 0) for value in visible.values()):
        f.append(f"{label}: transformed racer remained hidden/transparent: {visible}")
    if rm is not None:
        expected_characters = {int(seat): int(character) for seat, character
                               in re.findall(r"c(\d+)=(\d+)", rm.group(3))}
        if any(expected_characters.get(seat) != value[0]
               for seat, value in visible.items()):
            f.append(f"{label}: visibility witness character differs from roster")
    if sound_seats != set(range(players)):
        f.append(f"{label}: engine sound re-init after transform covered seats "
                 f"{sorted(sound_seats)}, expected all of {sorted(range(players))} "
                 f"(seats 1..N-1 would drive silently)")

    # --- R10 release: SHARED_DIALOGUE -> ACTIVE_LOBBY, same lgen, after xform --
    rel_i, rel_m = find_line(lines, SESSION_RE, start=ti,
                             pred=lambda m: m.group(1) == "ACTIVE_LOBBY"
                             and int(m.group(3)) == dialogue_lgen)
    if rel_i < 0:
        f.append(f"{label}: dialogue never released to ACTIVE_LOBBY at lgen {dialogue_lgen} "
                 f"(R10 latch release) after the transform")
    else:
        # No door transition between the latch and the release: a door cannot
        # bypass SHARED_DIALOGUE (movement is frozen and the arbiter rejects it).
        di, _ = find_line(lines, TRANS_RE, start=li)
        if 0 <= di < rel_i:
            f.append(f"{label}: a door transition fired DURING the shared dialogue "
                     f"(before the release) -- the door bypassed SHARED_DIALOGUE")

    return f, ti


def assert_identity(lines, players, transform_line_idx, label):
    """Post-transform: distinct racerIndex 0..N-1 drive, port == player == racer."""
    f = []
    seats = set()
    mismatched = 0
    for i in range(transform_line_idx, len(lines)):
        m = RACERINPUT_RE.search(lines[i])
        if not m:
            continue
        player, racer, port = int(m.group(2)), int(m.group(3)), int(m.group(4))
        if player < 0:  # a finished/door racer flips to PLAYER_COMPUTER; skip
            continue
        seats.add(racer)
        if not (player == racer == port):
            mismatched += 1
    want = set(range(players))
    if not want.issubset(seats):
        f.append(f"{label}: post-transform only racerIndex {sorted(seats)} drove; "
                 f"expected all of {sorted(want)} (a seat lost its racer)")
    if mismatched:
        f.append(f"{label}: {mismatched} post-transform [RACERINPUT] rows had "
                 f"port != player != racer (binding broke across the transform)")
    return f


def assert_challenge_refusal(out, players, label):
    """R27: a host CHALLENGE-row selection during a party SHARED_DIALOGUE is
    fail-closed refused -- no TAJ_MODE_RACE starts, no extra racer spawns, the
    party is intact, and a vehicle transform still works afterward.

    Run with MDKR_TAJ_PROBE=1 so ``[TAJ] phase=accept`` (init_racer_for_challenge)
    would appear IF a challenge race started; its absence is the direct
    no-race oracle.
    """
    f = []
    if bad := BAD_RE.search(out):
        f.append(f"{label}: fatal marker {bad.group(0)!r}")
        return f
    lines = out.splitlines()

    # --- The challenge row was refused, and it is OBSERVABLE ------------------
    chal = [(i, m) for i, m in enumerate(CHALLENGE_RE.search(l) for l in lines) if m]
    refused = [(i, m) for i, m in chal if int(m.group(2)) == 1]
    if not refused:
        f.append(f"{label}: no aparty_taj_challenge refused=1 -- the challenge row "
                 f"was not fail-closed refused")
        refuse_idx = 1 << 30
    else:
        refuse_idx = refused[0][0]

    # --- No challenge race ever started (no extra racer spawn) ----------------
    if TAJ_ACCEPT_RE.search(out):
        f.append(f"{label}: '[TAJ] phase=accept' present -- a challenge race was "
                 f"accepted (init_racer_for_challenge ran) despite the party refusal")

    # --- The party is intact and a vehicle transform STILL WORKS afterward ----
    # Reuse the whole-party transform oracle: it proves the summon latched, the
    # host owns the choice, and the WHOLE party (live==N, never a collapse)
    # transforms and republishes the roster/layout -- i.e. the refused challenge
    # left the session healthy and the transform path unaffected.
    tf, ti = assert_transform_scene(out, players, f"{label} post-refusal transform")
    f += tf
    if ti >= 0 and refuse_idx > ti:
        f.append(f"{label}: the refusal did not precede the transform -- the "
                 f"challenge was selected AFTER the transform, not before it")
    return f


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--texture-pack", type=Path,
                        help="ZIP copied into each isolated party arm; actual use required")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = str(Path(resolve_binary(args.build)).resolve())
    rom = str(Path(args.rom).resolve())
    taj3 = "tests/input_scripts/adventure_party_3p_taj.txt"
    taj2 = "tests/input_scripts/adventure_party_2p_taj.txt"
    plane2 = "tests/input_scripts/adventure_party_2p_taj_plane.txt"
    plane3 = "tests/input_scripts/adventure_party_3p_taj_plane.txt"
    chal3 = "tests/input_scripts/adventure_party_3p_taj_challenge.txt"
    off = ROOT / "tests" / "check_taj_p2_adventure.py"
    required = [binary, rom, *(str(ROOT / p) for p in (taj3, taj2, plane2, plane3, chal3)), str(off)]
    if args.texture_pack is not None:
        required.append(str(args.texture_pack))
    missing = [p for p in required if not os.path.exists(p)]
    if missing:
        for p in missing:
            print(f"check_adventure_party_taj: FAIL -- missing {p}", file=sys.stderr)
        return 1

    failures = []

    # --- 3P main scene: non-host summon + host-owned whole-party transform ----
    out3 = run_arm(binary, rom, taj3, TAJ_SEAT_ROUTE, verbose=args.verbose,
                   texture_pack=args.texture_pack)
    f3, ti3 = assert_transform_scene(out3, 3, "3P transform")
    failures += f3
    if ti3 >= 0:
        failures += assert_identity(out3.splitlines(), 3, ti3, "3P identity")

    # --- 2P arm: the same transform at two seats -----------------------------
    out2 = run_arm(binary, rom, taj2, TAJ_SEAT_ROUTE, verbose=args.verbose,
                   texture_pack=args.texture_pack)
    f2, ti2 = assert_transform_scene(out2, 2, "2P transform")
    failures += f2
    if ti2 >= 0:
        failures += assert_identity(out2.splitlines(), 2, ti2, "2P identity")
    for players, fixture in ((2, plane2), (3, plane3)):
        label = f"{players}P plane"
        output = run_arm(binary, rom, fixture, TAJ_SEAT_ROUTE, verbose=args.verbose,
                         texture_pack=args.texture_pack)
        found, transform_index = assert_transform_scene(output, players, label,
                                                       expected_vehicle=2)
        failures += found
        if transform_index >= 0:
            failures += assert_identity(output.splitlines(), players, transform_index, label)

    # --- R27 challenge refusal: a host CHALLENGE-row selection during a 3P
    #     party dialogue is refused (no TAJ_MODE_RACE, no extra racer, party
    #     intact, transform still works). Run with MDKR_TAJ_PROBE=1 so
    #     '[TAJ] phase=accept' would appear IF a race started. ---
    chal_eeprom = eeprom_image(TAJ_FLAGS_CAR_CHAL_UNLOCKED)
    out_chal = run_arm(binary, rom, chal3, TAJ_SEAT_ROUTE, verbose=args.verbose,
                       eeprom=chal_eeprom, extra_env={"MDKR_TAJ_PROBE": "1"},
                       texture_pack=args.texture_pack)
    failures += assert_challenge_refusal(out_chal, 3, "3P challenge-refusal")

    # --- Positive control 3: strip the refusal diagnostic -> the refusal
    #     assertion must FAIL (otherwise the gate cannot tell a refused challenge
    #     from an unguarded one). ---
    chal_stripped = "\n".join(l for l in out_chal.splitlines()
                              if "aparty_taj_challenge" not in l)
    pc3 = assert_challenge_refusal(chal_stripped, 3, "PC-strip-challenge")
    if not pc3:
        failures.append("positive control: output with the aparty_taj_challenge "
                        "refusal line stripped PASSED the R27 refusal assertion")

    # --- Positive control 4: inject a '[TAJ] phase=accept' -> the no-race
    #     oracle must FAIL (a challenge race that actually started must not pass). ---
    chal_accepted = out_chal + "\n[TAJ] phase=accept vehicle=0 flags=0x1 balloons=0\n"
    pc4 = assert_challenge_refusal(chal_accepted, 3, "PC-inject-accept")
    if not pc4:
        failures.append("positive control: output with a '[TAJ] phase=accept' "
                        "injected PASSED the R27 no-race assertion")

    # --- Positive control 1: collapse the post-transform roster to one -------
    collapsed = re.sub(r"(aparty_transform: path=party vehicle=\d+ n=\d+ live=)\d+",
                       r"\g<1>1", out3, count=1)
    pc1, _ = assert_transform_scene(collapsed, 3, "PC-collapse")
    if not pc1:
        failures.append("positive control: a collapsed roster (live=1) PASSED the "
                        "whole-party transform assertion")

    # --- Positive control 2: strip the SHARED_DIALOGUE latch lines -----------
    stripped = "\n".join(l for l in out3.splitlines()
                         if "state=SHARED_DIALOGUE" not in l)
    pc2, _ = assert_transform_scene(stripped, 3, "PC-strip-latch")
    if not pc2:
        failures.append("positive control: output with the SHARED_DIALOGUE latch lines "
                        "stripped PASSED the latch assertion")

    # Recreate the original non-focus fade-in failure in otherwise valid output.
    invisible = re.sub(r"(aparty_taj_visibility: seat=0 .*?transparency=)\d+",
                       r"\g<1>0", out3)
    if not assert_transform_scene(invisible, 3, "PC-invisible")[0]:
        failures.append("positive control: transparent host PASSED visibility assertion")
    missing_visibility = "\n".join(line for line in out3.splitlines()
                                   if "aparty_taj_visibility:" not in line)
    if not assert_transform_scene(missing_visibility, 3, "PC-no-visibility")[0]:
        failures.append("positive control: missing visibility evidence PASSED")

    # --- OFF arm: retail JOINTVENTURE 2P Taj is untouched --------------------
    off_proc = subprocess.run(
        [sys.executable, str(off), "--build", args.build, "--rom", args.rom],
        cwd=str(ROOT), text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=900, check=False)
    off_out = (off_proc.stdout or "").strip()
    off_pass = "check_taj_p2_adventure: PASS" in off_out
    if off_proc.returncode != 0 or not off_pass:
        failures.append(f"OFF arm: check_taj_p2_adventure did not pass -- {off_out[-300:]}")

    if failures:
        print("check_adventure_party_taj: FAIL", file=sys.stderr)
        for x in failures:
            print(f"  - {x}", file=sys.stderr)
        return 1
    off_quote = next((l for l in off_out.splitlines()
                      if l.startswith("check_taj_p2_adventure: PASS")), "")
    print("check_adventure_party_taj: PASS -- a NON-host summons Taj (shared dialogue "
          "latched once), the host owns the vehicle choice, the WHOLE party (2P/3P) "
          "transforms transactionally to the new vehicle with the same seat->character "
          "identities and split layout (live==N, never a collapse to 1), and the "
          "dialogue releases to ACTIVE_LOBBY in the same generation (R10) with no door "
          "firing during it; a host CHALLENGE-row selection during the party dialogue "
          "is fail-closed refused (R27: aparty_taj_challenge refused, no '[TAJ] "
          "phase=accept', no extra racer, party intact, transform still works after); "
          "all seats restored opaque/unhidden; six positive controls fired")
    print("  Party matrix: 2P/3P car->hovercraft and car->plane; " +
          ("replacement textures used in every party arm" if args.texture_pack else
           "isolated no-pack baseline"))
    print(f"  OFF arm: {off_quote}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
