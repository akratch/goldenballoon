#!/usr/bin/env python3
"""Guard the production seams for the runtime-safety repair batch.

The ROM-free unit executables exercise the shared boundary helpers and, since
1.8, the guards themselves: runtime_contracts (the helpers), ainode_route (the
AI-node rebuild and route-search guards), engine_guards (weather_reset and
clear_lap_records), save_records (record bytes and the retail save
inventory) and pulse_material (the pulsing-light uploads). This gate runs all
five.

The source census complements them where a guard has no ROM-free behavioural
witness. Two kinds of check:

* REQUIRED / FORBIDDEN: the pre-1.8 exact-text fragments, kept as they were.
  Each required fragment is removed in memory as a positive control.
* GUARDS (added in 1.8): token checks of the essential guard in the function
  that needs it, in the code the native build compiles (``#ifdef NATIVE_PORT``
  branches taken, their ``#else`` dropped, ``#if 0`` code dropped and ``#if 1``
  code kept). Whitespace, line breaks, comments and braces do not matter,
  ``$ID`` stands for any one identifier, and nothing is counted: a guard is
  present in its function or not. ``ordered`` guards must first appear in the
  order given (a check that runs after the read it protects is no check), and
  an ``each`` guard's first run must come before EVERY occurrence of its second
  (each read has its own check, not one check for the function). The positive
  controls delete each required token run (each occurrence of an ``each``
  check in turn), reverse each order and plant each forbidden run, and every
  one must fail; a dead-code control disables one guard with ``#if 0`` (must
  fail) and with ``#if 1`` (must pass); a reflow control rewrites every guarded
  file with a comment and a line break between all tokens, and every guard
  must still pass.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path

from check_address_domains import active_lines
from harness_utils import resolve_binary


REPO = Path(__file__).resolve().parent.parent

REQUIRED = {
    "game/src/game.c": (
        "LevelHeader *levelHeader = gCurrentLevelHeader;",
        "weatherEnabled = levelHeader->weatherEnable > 0;",
        "skyTexture = DKR_PTR(TextureHeader, levelHeader->unkA4);",
        "free_track();",
        "weather_free();",
        "tex_free(skyTexture);",
        "mempool_free(levelHeader);",
        "gCurrentLevelHeader = NULL;",
    ),
    "game/src/tracks.c": (
        "gCurrentLevelModel = NULL;",
        "gCurrentLevelHeader2 = NULL;",
        "u8 *collisionCursor;",
        "segment->unk10 = DKR_TOK(collisionCursor);",
        "segment->collisionPlanes = DKR_TOK(collisionCursor);",
        "segment->unk34 = DKR_TOK(collisionCursor);",
        "collisionMaxBytes",
    ),
    "game/src/tracks.h": (
        "(ptr) = DKR_TOK((u8 *)(mdl) + (u32)(ptr))",
    ),
    "game/src/racer.c": (
        "s8 spA1 = FALSE;",
        "Object *intendedTarget = NULL;",
        "pathGoalValid = set_position_goal_from_path",
        "mdkr_normalize_xz(spB8, spB4, &spB8, &spB4)",
        "mdkr_normalize_xz(xVel, zVel, &xVel, &zVel)",
        "*x = obj->trans.x_position;",
        "*y = obj->trans.y_position;",
        "*z = obj->trans.z_position;",
        "return FALSE;",
    ),
    "game/src/audio.c": (
        "audConfig.numGroups = MDKR_SOUND_GROUP_COUNT;",
        "mdkr_sound_id_valid(soundId, gSoundCount)",
        "mdkr_sound_id_valid(soundID, gSoundCount)",
    ),
    "game/src/audiosfx.c": (
        "sndp_group_volume_for_key(keyMap)",
        "gSoundGroupVolume == NULL || gSoundGroupCount == 0",
    ),
    "game/src/audio_vehicle.c": (
        "mdkr_vehicle_sound_row(characterId, vehicleId, &assetRow, &soundVehicleId)",
        "mdkr_asset_rows_fit(table[ASSET_AUDIO_7], table[ASSET_AUDIO_8]",
        "asset_swap_vehicle_sound(asset, sizeof(VehicleSoundAsset))",
        "mdkr_vehicle_sound_model(gSoundRacerObj->vehicleIDPrev)",
        "case VEHICLE_CAR:",
        "racer_sound_plane(obj, buttonsPressed, buttonsHeld, ticksDelta);",
        "racer_sound_car(obj, buttonsPressed, buttonsHeld, ticksDelta);",
    ),
    "game/src/audiomgr.c": (
        "mdkr_audio_fx_span(assetAudioTable,",
        "c->fxType[0] = AL_FX_NONE;",
        "mempool_free(assetAudioTable);",
        "mempool_free(asset8);",
    ),
    # game/libultra/src/audio/{env,load}.c entries removed: the SGI-derived
    # synthesizer was deleted at the clean-room audio swap, and the safety
    # contracts those two entries asserted died with the code they guarded.
    # The clean-room engine's equivalent boundary (synthInternals.h param
    # round-trips) is covered by check_address_domains' allowance instead.
    "game/src/textures_sprites.c": (
        "mdkr_texture_allocation_size(",
        "mdkr_texture_frame_advance(",
        "mdkr_palette_reservation(",
        "gNumberOfLoadedTextures >= MAX_NUM_TEXTURES",
        "gTextureCache[ASSETCACHE_ID(slotIndex)] = id;",
        "if (cacheExtended) {\n        gNumberOfLoadedTextures++;",
    ),
    "game/src/object_functions.c": (
        "mdkr_trophy_state(settings->trophies, settings->worldId,",
        "mdkr_course_flag(levelEntry->goldenBalloon.balloonID, &flag)",
        "mdkr_course_flag(door->doorID, &doorIDFlag)",
        "mdkr_course_flag(triggerEntry->index, &flags)",
        "mdkr_model_index_resolve(",
    ),
    "game/src/objects.c": (
        "mdkr_model_load_selection(",
        "mdkr_trophy_state(settings->trophies, settings->worldId,",
        "uintptr_t objectEntryAddress = (uintptr_t) obj->level_entry;",
    ),
    "game/src/save_data.c": (
        "mdkr_extension_bit(extension, &extensionBit)",
    ),
    "game/src/menu.c": (
        "settings->trophies = mdkr_trophy_records_merge(",
        "mdkr_trophy_state(settings->trophies,",
        "i = WORLD_DINO_DOMAIN; i <= WORLD_FUTURE_FUN_LAND; i++)",
        "mdkr_trophy_state(settings->trophies, i, &trophyState)",
    ),
    "game/src/weather.c": (
        "DKR_SHL32(1U, rand_range(0, 32) + 5)",
    ),
    "platform/platform_sdl_min.c": (
        "static int8_t s_rumbleSupported[DKR_MAXPADS] = { -1, -1, -1, -1 };",
        "if (s_rumbleSupported[port] >= 0) return s_rumbleSupported[port];",
        "SDL_GameControllerHasRumble(s_gc[port]) == SDL_TRUE",
        "zero-strength compatibility probe exactly once per opened controller",
        "s_rumbleSupported[i] = -1;",
    ),
}

FORBIDDEN = {
    "game/src/audio.c": (
        "if (soundId > gSoundCount)",
        "if (soundID > gSoundCount)",
        "audConfig.numGroups = 1;",
    ),
    "game/src/racer.c": (
        "s8 spA1;\n",
        "Object *intendedTarget;\n",
    ),
    "game/src/object_functions.c": (
        "0x10000 << levelEntry->goldenBalloon.balloonID",
        "0x10000 << door->doorID",
        "0x10000 << triggerEntry->index",
    ),
    "game/src/save_data.c": (
        "1 << (fileExtensions[fileNum][0] + (BLANK_EXT_CHAR - 1))",
    ),
    "platform/platform_sdl_min.c": (
        "return SDL_GameControllerRumble(s_gc[port], 0, 0, 0) == 0;",
    ),
}


# --------------------------------------------------------------------------- #
#  Guards added in 1.8: token checks
# --------------------------------------------------------------------------- #


@dataclass(frozen=True)
class Guard:
    path: str
    function: str
    require: tuple[str, ...]
    forbid: tuple[str, ...] = ()
    ordered: bool = False
    # require[0] must come before every occurrence of require[1].
    each: bool = False
    why: str = ""


GUARDS = (
    Guard("game/src/waves.c", "func_800BBE08",
          ("header->unk74[colourID - 1]",
           "$ID == (dkrptr32)-1 ? NULL : DKR_PTR(LevelHeader_70, $ID)"),
          why="an absent water colour record (-1 token) selects no colour"),
    Guard("game/src/game.c", "level_header_size_checked",
          ("table[index + 1] > asset_table_size(ASSET_LEVEL_HEADERS)",),
          why="a header span must end inside the level-header section"),
    Guard("game/src/game.c", "level_global_init",
          ("level_header_size_checked(gTempAssetTable, gNumberOfLevelHeaders, i)",
           "asset_load(ASSET_LEVEL_HEADERS,"),
          ordered=True,
          why="each header span is checked before the header is read"),
    Guard("game/src/game.c", "level_global_init",
          ("sRetailLevelCount = asset_retail_entry_count(ASSET_LEVEL_HEADERS_TABLE, "
           "gNumberOfLevelHeaders)",
           "if (i < sRetailLevelCount && sRetailWorldCount <= gCurrentLevelHeader->world)",
           "i < gNumberOfLevelHeaders || i <= 0",
           "sLevelNameSizes = (u32 *)(gLevelNames + i)"),
          why="retail save inventory recorded; level-name table sized and bounded"),
    Guard("game/src/game.c", "level_count_for_save",
          ("*outLevelCount = sRetailLevelCount;", "*outWorldCount = sRetailWorldCount;"),
          why="save codecs see the retail inventory, not appended courses"),
    Guard("game/src/game.c", "level_name",
          ("mdkr_level_name_bounded(gLevelNames[levelId], sLevelNameSizes[levelId], language)",),
          why="a level name is selected within its own entry"),
    Guard("game/src/game.c", "level_load",
          ("level_header_size_checked(gTempAssetTable, gNumberOfLevelHeaders, levelId)",
           "asset_load(ASSET_LEVEL_HEADERS,"),
          forbid=("gTempAssetTable[levelId + 1] - offset",),
          each=True,
          why="every header load, initial and redirected, has its own span check "
              "and none sizes itself unchecked"),
    Guard("game/src/game.c", "level_load",
          ("(s32)gCurrentLevelHeader->weatherVelX * 256",
           "(s32)gCurrentLevelHeader->weatherVelY * 256",
           "(s32)gCurrentLevelHeader->weatherVelZ * 256"),
          why="weather velocities widen before scaling"),
    Guard("game/src/tracks.c", "render_level_segment",
          ("if (numberVertices <= 0 || numberTriangles <= 0) continue;", "gSPVertexDKR("),
          ordered=True,
          why="an empty batch is skipped before any draw command"),
    Guard("game/src/tracks.c", "render_level_segment",
          ("if (pulseData == NULL || texture == NULL) batchFlags &= ~RENDER_PULSING_LIGHTS;",
           "color = pulseData->outColorValue;"),
          ordered=True,
          why="pulsing lights need their data and a texture before they are read"),
    Guard("game/src/tracks.c", "render_level_segment",
          ("gCurrentLevelHeader2->unk74[levelHeaderIndex - 1]",
           "$ID == (dkrptr32)-1 ? NULL :",
           "if ($ID != NULL)"),
          why="an absent batch colour record selects the default colour"),
    Guard("game/src/textures_sprites.c", "update_colour_cycle",
          ("mdkr_light_cycle_time(arg0->unk8, updateRate, arg0->unkC)",
           "mdkr_light_fraction(65536u, arg0->unk8, cur->unk18->unk0)"),
          why="colour cycles advance and interpolate through the bounded helpers"),
    Guard("game/src/textures_sprites.c", "update_pulsating_light_data",
          ("mdkr_light_cycle_time(data->time, timeDelta, data->totalTime)",
           "mdkr_light_fraction(data->frames[nextFrameIndex].value,"),
          why="pulsating lights advance and interpolate through the bounded helpers"),
    Guard("game/src/objects.c", "allocate_object_pools",
          ("for (i = 0; i < MDKR_OBJ_BHV_CACHE_N; ++i) sObjectBehaviorCache[i] = -3;",),
          why="the behaviour cache is reset with the object pools"),
    Guard("game/src/objects.c", "mdkr_obj_behavior_of",
          ("if (hdr == NULL) { return -1; }", "sObjectBehaviorCache[hdrType] = $ID;"),
          ordered=True,
          why="a failed header load returns before it can be cached"),
    Guard("game/src/objects.c", "mdkr_objmap_swap_bodies",
          ("if (!mdkr_objmap_validate_bodies(entries, totalSize)) return FALSE;",
           "mdkr_objmap_swap_entry_body("),
          ordered=True,
          why="the whole object map is validated before any body is swapped"),
    Guard("game/src/objects.c", "track_spawn_objects",
          ("if (!mdkr_object_map_index_resolve(mapID, i, &mapID))", "objMapTable[mapID]"),
          ordered=True,
          why="an object-map reference is resolved before it indexes the table"),
    Guard("game/src/menu.c", "postrace_start",
          ("$ID = mdkr_postrace_background_index(header->world);",),
          why="the post-race background row is bounded"),
    Guard("game/src/menu.c", "postrace_free",
          ("$ID = mdkr_postrace_background_index(header->world);",),
          why="the post-race background row is bounded on release too"),
)

# 1.8 guards whose source pins moved to ROM-free behavioural units. Named here
# so the census documents where each went; main() runs every unit.
BEHAVIOUR_UNITS = {
    "mdkr_runtime_contracts_test": "shared boundary helpers",
    "mdkr_ainode_route_test": "objects.c ainode_update duplicate-ID, stale-link and cost "
                              "guards; func_8001CD28 stale-link checks and retail routing",
    "mdkr_engine_guards_test": "weather.c weather_reset negative types; thread3_main.c "
                               "clear_lap_records past the retail inventory",
    "mdkr_save_records_test": "save_data.c codecs use the retail inventory "
                              "(level_count_for_save); record bytes are deterministic",
    "mdkr_pulse_material_test": "textures_sprites.c material_set_blinking_lights loads only "
                                "the texture and samples the N64's bytes (wrapped)",
}

TOKEN_RE = re.compile(r"""
      "(?:\\.|[^"\\\n])*"
    | '(?:\\.|[^'\\\n])*'
    | \$ID
    | [A-Za-z_]\w*
    | \.?\d(?:[eEpP][+-]|[\w.])*
    | ->|\+\+|--|<<=|>>=|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^]=|\.\.\.
    | \S
""", re.X)
LITERAL_OR_COMMENT_RE = re.compile(r"""
      (?P<string>"(?:\\.|[^"\\\n])*")
    | (?P<char>'(?:\\.|[^'\\\n])*')
    | (?P<block>/\*.*?\*/)
    | (?P<line>//[^\n]*)
""", re.X | re.S)
WILDCARD = "$ID"


def _blank_comments(text: str) -> str:
    """Drop comments (keeping their line breaks) and empty every string
    literal, so the line-based preprocessor walk cannot mistake literal text
    for code or a comment."""

    def replace(match: re.Match[str]) -> str:
        if match.group("string") is not None:
            return '""'
        if match.group("char") is not None:
            return match.group("char")
        return " " + "\n" * match.group(0).count("\n")

    return LITERAL_OR_COMMENT_RE.sub(replace, text)


@lru_cache(maxsize=64)
def native_tokens(text: str) -> tuple[str, ...]:
    """Tokens of the code the native build compiles."""

    lines = active_lines(_blank_comments(text))
    return tuple(TOKEN_RE.findall("\n".join(line for _, line in lines)))


def unbraced(tokens) -> list[str]:
    """Braces dropped: ``if (x) continue;`` and ``if (x) { continue; }`` are
    the same guard. (Function bodies are still found by their braces.)"""

    return [token for token in tokens if token not in ("{", "}")]


def pattern(snippet: str) -> list[str]:
    return unbraced(TOKEN_RE.findall(snippet))


def find_run(tokens: list[str], run: list[str], start: int = 0) -> int:
    """Index of the first occurrence of ``run`` at or after ``start``; -1 if none."""

    if not run:
        return start
    last = len(tokens) - len(run)
    for index in range(start, last + 1):
        for offset, want in enumerate(run):
            got = tokens[index + offset]
            if want == WILDCARD:
                if not (got[0].isalpha() or got[0] == "_"):
                    break
            elif got != want:
                break
        else:
            return index
    return -1


def function_body(tokens: tuple[str, ...], name: str) -> list[str] | None:
    """The tokens between the braces of ``name``'s definition.

    A definition is ``name ( ... ) {``; a call or a prototype is never
    followed directly by a brace. Brace depth outside the function is not
    trusted: a build condition this walk cannot decide keeps both branches."""

    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token == name and index + 1 < len(tokens) and tokens[index + 1] == "(":
            close = index + 1
            parens = 0
            while close < len(tokens):
                parens += tokens[close] == "("
                parens -= tokens[close] == ")"
                if parens == 0:
                    break
                close += 1
            if close + 1 < len(tokens) and tokens[close + 1] == "{":
                body_start = close + 2
                braces = 1
                cursor = body_start
                while cursor < len(tokens) and braces:
                    braces += tokens[cursor] == "{"
                    braces -= tokens[cursor] == "}"
                    cursor += 1
                return list(tokens[body_start:cursor - 1])
            index = close
        index += 1
    return None


def guard_failures(guard: Guard, body: list[str] | None) -> list[str]:
    label = f"{guard.path}:{guard.function}"
    if body is None:
        return [f"{label}: function not found in the native build's code"]
    failures: list[str] = []
    positions = []
    for snippet in guard.require:
        position = find_run(body, pattern(snippet))
        positions.append(position)
        if position < 0:
            failures.append(f"{label}: missing guard {snippet!r} ({guard.why})")
    if guard.ordered and all(p >= 0 for p in positions) and positions != sorted(positions):
        failures.append(f"{label}: guard out of order, need {list(guard.require)!r} ({guard.why})")
    if guard.each and all(p >= 0 for p in positions):
        check, read = pattern(guard.require[0]), pattern(guard.require[1])
        start, at, number = 0, find_run(body, read), 1
        while at >= 0:
            found = find_run(body, check, start)
            if found < 0 or found > at:
                failures.append(f"{label}: {guard.require[1]!r} #{number} has no "
                                f"{guard.require[0]!r} of its own before it ({guard.why})")
                break
            start = at + len(read)
            at, number = find_run(body, read, start), number + 1
    for snippet in guard.forbid:
        if find_run(body, pattern(snippet)) >= 0:
            failures.append(f"{label}: unguarded form present {snippet!r} ({guard.why})")
    return failures


def guard_bodies(sources: dict[str, str]) -> dict[tuple[str, str], list[str] | None]:
    tokens = {path: native_tokens(sources[path]) for path in {g.path for g in GUARDS}}
    bodies = {}
    for guard in GUARDS:
        body = function_body(tokens[guard.path], guard.function)
        bodies[(guard.path, guard.function)] = None if body is None else unbraced(body)
    return bodies


def validate_guards(bodies: dict[tuple[str, str], list[str] | None]) -> list[str]:
    failures: list[str] = []
    for guard in GUARDS:
        failures += guard_failures(guard, bodies[(guard.path, guard.function)])
    return failures


def run_guard_controls(sources: dict[str, str]) -> list[str]:
    """Every guard must fail without its tokens, out of order, or with its
    forbidden form planted -- and must still pass after a full reflow."""

    failures: list[str] = []
    bodies = guard_bodies(sources)
    for guard in GUARDS:
        body = bodies[(guard.path, guard.function)]
        if body is None:
            continue
        for snippet in guard.require:
            if not guard_failures(guard, without(body, pattern(snippet))):
                failures.append(f"control: {guard.path}:{guard.function} passed without {snippet!r}")
        if guard.ordered:
            first = pattern(guard.require[0])
            at = find_run(body, first)
            if at >= 0:
                moved = body[:at] + body[at + len(first):] + first
                if not guard_failures(guard, moved):
                    failures.append(f"control: {guard.path}:{guard.function} passed out of order")
        if guard.each:
            check = pattern(guard.require[0])
            at = find_run(body, check)
            occurrence = 1
            while at >= 0:
                if not guard_failures(guard, body[:at] + body[at + len(check):]):
                    failures.append(f"control: {guard.path}:{guard.function} passed "
                                    f"without check #{occurrence} of {guard.require[0]!r}")
                at = find_run(body, check, at + len(check))
                occurrence += 1
        for snippet in guard.forbid:
            if not guard_failures(guard, body + pattern(snippet)):
                failures.append(f"control: {guard.path}:{guard.function} passed with {snippet!r}")

    reflowed = dict(sources)
    for path in {g.path for g in GUARDS}:
        reflowed[path] = reflow(sources[path])
    for failure in validate_guards(guard_bodies(reflowed)):
        failures.append(f"reflow control: {failure}")
    failures.extend(dead_code_controls(sources))
    return failures


# The redirected level-header load's own span check, on its own line.
DEAD_CODE_LINE = "size = level_header_size_checked(gTempAssetTable, gNumberOfLevelHeaders, levelId);"


def dead_code_controls(sources: dict[str, str]) -> list[str]:
    """A guard left in the file but compiled out must read as missing."""

    text = sources["game/src/game.c"]
    lines = text.split("\n")
    where = [index for index, line in enumerate(lines) if line.strip() == DEAD_CODE_LINE]
    if len(where) < 2:
        return [f"dead-code control: expected the initial and redirected "
                f"{DEAD_CODE_LINE!r} in game.c, found {len(where)}"]
    failures: list[str] = []
    for condition, must_fail in (("0", True), ("1", False)):
        mutated = list(lines)
        mutated[where[-1]:where[-1] + 1] = [f"#if {condition}", lines[where[-1]], "#endif"]
        found = validate_guards(guard_bodies({**sources, "game/src/game.c": "\n".join(mutated)}))
        if bool(found) != must_fail:
            failures.append(f"dead-code control: the redirected span check under "
                            f"#if {condition} {'passed' if must_fail else 'failed'}: {found}")
    return failures


def without(tokens: list[str], run: list[str]) -> list[str]:
    """``tokens`` with every occurrence of ``run`` cut out."""

    out = list(tokens)
    at = find_run(out, run)
    while at >= 0:
        del out[at:at + len(run)]
        at = find_run(out, run, at)
    return out


def reflow(text: str) -> str:
    """The same code with a comment and a line break between every token
    (preprocessor lines kept whole, since they are line-based)."""

    out: list[str] = []
    for line in _blank_comments(text).split("\n"):
        if line.lstrip().startswith("#"):
            out.append(line)
        else:
            out.append("  /* reflowed */\n\t".join(TOKEN_RE.findall(line)))
    return "\n".join(out)



def load_sources() -> dict[str, str]:
    paths = set(REQUIRED) | set(FORBIDDEN) | {guard.path for guard in GUARDS}
    return {
        relative: (REPO / relative).read_text(encoding="utf-8", errors="replace")
        for relative in sorted(paths)
    }


def validate(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for relative, fragments in REQUIRED.items():
        text = sources[relative]
        for fragment in fragments:
            if fragment not in text:
                failures.append(f"{relative}: missing contract fragment {fragment!r}")
    for relative, fragments in FORBIDDEN.items():
        text = sources[relative]
        for fragment in fragments:
            if fragment in text:
                failures.append(f"{relative}: obsolete unsafe fragment remains {fragment!r}")

    game = sources["game/src/game.c"]
    order = (
        "LevelHeader *levelHeader = gCurrentLevelHeader;",
        "free_track();",
        "weather_free();",
        "tex_free(skyTexture);",
        "mempool_free(levelHeader);",
    )
    positions = [game.find(fragment) for fragment in order]
    if any(position < 0 for position in positions) or positions != sorted(positions):
        failures.append("game/src/game.c: level teardown ownership order changed")

    racer = sources["game/src/racer.c"]
    if racer.count("pathGoalValid = set_position_goal_from_path") != 2:
        failures.append("game/src/racer.c: expected both recovery paths to validate path goals")
    if racer.count("mdkr_normalize_xz(") != 2:
        failures.append("game/src/racer.c: expected both recovery paths to normalize safely")

    audio_sfx = sources["game/src/audiosfx.c"]
    if audio_sfx.count("sndp_group_volume_for_key(keyMap)") != 5:
        failures.append("game/src/audiosfx.c: not every group-volume read uses the bounded helper")
    if audio_sfx.count("groupID >= gSoundGroupCount") != 2:
        failures.append("game/src/audiosfx.c: group getter/setter bounds checks changed")
    failures += validate_guards(guard_bodies(sources))
    return failures


def run_positive_controls(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for relative, fragments in REQUIRED.items():
        for fragment in fragments:
            mutated = dict(sources)
            mutated[relative] = mutated[relative].replace(fragment, "")
            if not validate(mutated):
                failures.append(
                    f"positive control did not reject removal of {relative}: {fragment!r}"
                )
    mutated = dict(sources)
    mutated["game/src/audiosfx.c"] = mutated["game/src/audiosfx.c"].replace(
        "groupID >= gSoundGroupCount", "", 1
    )
    if not validate(mutated):
        failures.append("positive control did not reject a missing group API bounds check")
    failures.extend(run_guard_controls(sources))
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", required=True, help="native build or mdkr64 binary")
    parser.add_argument("--rom", help="accepted for the suite's common invocation shape")
    args = parser.parse_args()

    sources = load_sources()
    failures = validate(sources)
    failures.extend(run_positive_controls(sources))

    binary = Path(resolve_binary(args.build))
    for name, covers in BEHAVIOUR_UNITS.items():
        unit = binary.parent / name
        if not unit.is_file():
            failures.append(f"missing behavioural unit executable {unit} ({covers})")
            continue
        result = subprocess.run(
            [str(unit)],
            cwd=REPO,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if result.returncode != 0:
            failures.append(
                f"{name} failed ({result.returncode}; {covers}):\n{result.stdout}"
            )

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print(f"runtime safety: census, {len(GUARDS)} token guards, controls and "
          f"{len(BEHAVIOUR_UNITS)} behavioural units passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
