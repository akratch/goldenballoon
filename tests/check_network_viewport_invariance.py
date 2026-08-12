#!/usr/bin/env python3
"""Lock authoritative gameplay to canonical participants, not local views."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def function_body(source: str, name: str) -> str:
    marker = f"{name}("
    search = 0
    while True:
        start = source.find(marker, search)
        assert start >= 0, f"missing function {name}"
        brace = source.find("{", start)
        semicolon = source.find(";", start)
        if brace >= 0 and (semicolon < 0 or brace < semicolon):
            break
        search = start + len(marker)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : index + 1]
    raise AssertionError(f"unterminated body for {name}")


def assert_racer_contract(source: str) -> None:
    names = (
        "racer_ai_challenge",
        "racer_ai_eggs",
        "func_80050A28",
        "update_car_velocity_ground",
    )
    for name in names:
        body = function_body(source, name)
        canonicalized = re.sub(
            r"mdkr_authoritative_player_count\(\s*"
            r"cam_get_viewport_layout\(\) \+ 1\s*\)",
            "CANONICAL_PLAYER_COUNT",
            body,
        )
        assert "cam_get_viewport_layout" not in canonicalized, (
            f"{name} leaked endpoint-local viewport layout into authority"
        )
        assert "CANONICAL_PLAYER_COUNT" in canonicalized, (
            f"{name} no longer uses the canonical participant seam"
        )


def assert_fish_contract(source: str) -> None:
    body = function_body(source, "obj_loop_fish")
    query = "mdkr_authoritative_player_count"
    assert query in body, "fish lifetime does not use canonical participants"
    assert body.index(query) < body.index("free_object(fishObj)"), (
        "fish canonical admission must precede its authoritative free"
    )
    assert "#ifdef NATIVE_PORT" in body and "#else" in body, (
        "native roster seam must preserve the original non-port build"
    )


def assert_launcher_handoff(source: str) -> None:
    body = function_body(source, "runEngineSession")
    install = body.index("mdkr_net_roster_runtime_install")
    boot = body.index("mdkr64_engine_boot")
    clear = body.index("mdkr_net_roster_runtime_clear", boot)
    assert install < boot < clear, (
        "launcher must install before boot and clear after blocking teardown"
    )
    assert "MDKR_INTENT_ONLINE_PRIVATE" in body
    assert "MDKR_SCENE_LOADING" in body
    assert body.index("session.requestRace") < install, (
        "online loading must become an admitted race before roster install"
    )


def assert_process_gate_enters_through_launcher(source: str) -> None:
    # Include the return type so the mention in the restart-path comment cannot
    # be mistaken for the definition by the intentionally tiny C++ scanner.
    body = function_body(source, "int runAutoplay")
    begin = body.index("session.beginOnline")
    room = body.index("session.setRoomPhase(MDKR_ROOM_LOADING)")
    apply = body.index("session.applyLaunch(launch)")
    run = body.index("runEngineSession")
    assert begin < room < apply < run, (
        "process gate must enter through launcher state/envelope before boot"
    )
    assert "MDKR_APP_TEST_ONLINE_LOCAL_MASK" in body
    assert "MDKR_APP_TEST_ONLINE_VIEWPORT_MASK" in body


def assert_shared_player_count_contract(source: str) -> None:
    body = function_body(source, "mdkr_authoritative_player_count")
    assert "localPlayerCount < 1 || localPlayerCount > 4" in body
    assert "mdkr_net_roster_runtime_canonical_player_count" in body
    assert body.index("localPlayerCount < 1") < body.index(
        "mdkr_net_roster_runtime_canonical_player_count"
    ), "local behavior must be the fallback, not a second authority"


def assert_output_camera_split(camera: str, tracks: str) -> None:
    begin = function_body(camera, "cam_output_view_begin")
    assert "state->viewport >= 0" in begin, (
        "presentation output override must refuse nesting"
    )
    viewport = function_body(camera, "viewport_main")
    assert "cam_get_output_viewport()" in viewport
    assert "cam_get_output_viewport_layout()" in viewport
    assert "if (!cam_output_projection_compatible(" in viewport, (
        "endpoint projection compatibility must be a fail-closed guard"
    )
    lens = viewport.index("cam_output_projection_compatible")
    scissor = viewport.index("widthAndHeight = fb_size()")
    assert lens < scissor, (
        "endpoint lens compatibility must fail closed before viewport output"
    )
    stack = function_body(camera, "cam_output_viewport_stack")
    assert "static Vp stack[4]" in stack, (
        "mapped display-list commands need copy-owned viewport storage"
    )
    rsp = function_body(camera, "void viewport_rsp_set")
    assert "cam_output_viewport_stack()[outputViewport]" in rsp
    assert "OS_K0_TO_PHYSICAL(outputStack)" in rsp, (
        "mapped viewport command borrowed mutable canonical storage"
    )
    # Preprocessor-exclusive loop arms contain braces for both native and
    # matching builds, which a text-only brace scanner cannot pair. Bound the
    # one function by its stable rodata footer instead.
    render_start = tracks.index("void render_scene(")
    render_end = tracks.index("/************ .rodata ************/", render_start)
    render = tracks[render_start:render_end]
    mapping = render.index("mdkr_net_roster_runtime_viewport_to_canonical")
    bracket = render.index("cam_output_view_begin")
    active = render.index("set_active_camera(gSceneCurrentPlayerID)")
    end = render.index("cam_output_view_end")
    assert mapping < bracket < active < end, (
        "canonical camera must be selected inside one output-view bracket"
    )
    assert "mdkr_net_roster_runtime_viewport_count" in render
    assert "endpoint=no-render hidden-world-passes=0" in render


def assert_endpoint_minimap_layout(game_ui: str) -> None:
    query = function_body(game_ui, "hud_presentation_viewport_layout")
    assert "mdkr_net_roster_runtime_viewport_count(0u)" in query
    assert "return gHUDNumPlayers" in query, (
        "ordinary local HUD layout must remain the non-session fallback"
    )
    general = function_body(game_ui, "hud_render_general")
    assignment = general.index(
        "presentationLayout = hud_presentation_viewport_layout()"
    )
    layout_switch = general.index("switch (presentationLayout)")
    minimap_draw = general.index("render_ortho_triangle_image_transform")
    assert assignment < layout_switch < minimap_draw, (
        "minimap must choose endpoint layout before its draw"
    )


def assert_endpoint_hud_reflow(game_ui: str) -> None:
    reflow = function_body(game_ui, "hud_endpoint_reflow_element")
    assert "canonicalBaseline" in reflow and "endpointBaseline" in reflow
    assert "*output = *source" in reflow, (
        "HUD presentation must copy canonical state before rebasing it"
    )
    render = function_body(game_ui, "hud_element_render")
    shadow = render.index("hud_endpoint_reflow_element(hud, &endpointHud)")
    redirect = render.index("hud = &endpointHud")
    asset = render.index("spriteID = hud->spriteID", redirect)
    assert shadow < redirect < asset, (
        "only the reflowed stack shadow may reach HUD asset rendering"
    )


def assert_audio_listener_split(audio: str) -> None:
    update = function_body(audio, "audspat_update_all")
    canonical = update.index(
        "mdkr_net_roster_runtime_canonical_player_count"
    )
    report = update.index("audspat_report_endpoint_listeners")
    vehicles = update.index("racer_sound_update_all")
    assert canonical < report < vehicles, (
        "voice lifetime and vehicle updates must use canonical listeners"
    )
    assert "audspat_endpoint_point_mix" in update
    assert "audspat_endpoint_line_mix" in update


def assert_no_render_chrome(thread: str) -> None:
    mode = function_body(thread, "void mode_game")
    guard = mode.index("mdkr_net_roster_runtime_viewport_count(1u) > 0u")
    general = mode.index("hud_render_general(")
    assert guard < general, "no-render endpoint emitted global HUD chrome"


def mutation_controls(racer: str, objects: str, app: str,
                      camera: str, tracks: str, game_ui: str,
                      thread: str, audio: str) -> None:
    broken_racer = racer.replace(
        "mdkr_authoritative_player_count(\n                            cam_get_viewport_layout() + 1) == 1",
        "cam_get_viewport_layout() == 0",
        1,
    )
    try:
        assert_racer_contract(broken_racer)
    except AssertionError:
        pass
    else:
        raise AssertionError("viewport-to-AI mutation control was not detected")

    broken_fish = objects.replace(
        "authorityPlayerCount = mdkr_authoritative_player_count(\n"
        "        cam_get_viewport_layout() + 1);",
        "authorityPlayerCount = cam_get_viewport_layout() + 1;",
        1,
    )
    try:
        assert_fish_contract(broken_fish)
    except AssertionError:
        pass
    else:
        raise AssertionError("viewport-to-world mutation control was not detected")

    prefix, separator, suffix = app.rpartition(
        "mdkr_net_roster_runtime_clear();"
    )
    broken_app = prefix + suffix if separator else app
    try:
        assert_launcher_handoff(broken_app)
    except (AssertionError, ValueError):
        pass
    else:
        raise AssertionError("roster lifetime mutation control was not detected")

    broken_online = app.replace(
        "session.setRoomPhase(MDKR_ROOM_LOADING)",
        "/* skipped launcher room */ true",
        1,
    )
    try:
        assert_process_gate_enters_through_launcher(broken_online)
    except (AssertionError, ValueError):
        pass
    else:
        raise AssertionError("direct-to-engine online mutation was not detected")

    broken_view = tracks.replace(
        "set_active_camera(gSceneCurrentPlayerID);",
        "set_active_camera(j);",
        1,
    )
    try:
        assert_output_camera_split(camera, broken_view)
    except (AssertionError, ValueError):
        pass
    else:
        raise AssertionError("canonical-camera selection mutation was not detected")

    broken_lens = camera.replace(
        "if (!cam_output_projection_compatible(",
        "if (FALSE && !cam_output_projection_compatible(",
        1,
    )
    try:
        assert_output_camera_split(broken_lens, tracks)
    except (AssertionError, ValueError):
        pass
    else:
        raise AssertionError("endpoint-lens fail-closed mutation was not detected")

    broken_storage = camera.replace(
        "cam_output_viewport_stack()[outputViewport]",
        "gViewportStack[outputViewport]",
        1,
    )
    try:
        assert_output_camera_split(broken_storage, tracks)
    except (AssertionError, ValueError):
        pass
    else:
        raise AssertionError(
            "mutable mapped-viewport storage mutation was not detected"
        )

    broken_minimap = game_ui.replace(
        "switch (presentationLayout)", "switch (gHUDNumPlayers)", 1
    )
    try:
        assert_endpoint_minimap_layout(broken_minimap)
    except (AssertionError, ValueError):
        pass
    else:
        raise AssertionError("canonical minimap-layout mutation was not detected")

    broken_reflow = game_ui.replace(
        "hud = &endpointHud;", "/* canonical HUD leaked to renderer */", 1
    )
    try:
        assert_endpoint_hud_reflow(broken_reflow)
    except (AssertionError, ValueError):
        pass
    else:
        raise AssertionError("canonical HUD render mutation was not detected")

    broken_audio = audio.replace(
        "numCameras = (s32)mdkr_net_roster_runtime_canonical_player_count(\n"
        "            (u8)numCameras);",
        "/* endpoint camera count leaked into voice authority */",
        1,
    )
    try:
        assert_audio_listener_split(broken_audio)
    except (AssertionError, ValueError):
        pass
    else:
        raise AssertionError("endpoint audio authority mutation was not detected")

    broken_chrome = thread.replace(
        "mdkr_net_roster_runtime_viewport_count(1u) > 0u",
        "true",
        1,
    )
    try:
        assert_no_render_chrome(broken_chrome)
    except (AssertionError, ValueError):
        pass
    else:
        raise AssertionError("no-render HUD mutation was not detected")


def main() -> None:
    racer = (ROOT / "game/src/racer.c").read_text(encoding="utf-8")
    objects = (ROOT / "game/src/object_functions.c").read_text(encoding="utf-8")
    app = (ROOT / "platform/app/main_app.cpp").read_text(encoding="utf-8")
    authority = (ROOT / "game/src/network_player_authority.c").read_text(
        encoding="utf-8"
    )
    camera = (ROOT / "game/src/camera.c").read_text(encoding="utf-8")
    tracks = (ROOT / "game/src/tracks.c").read_text(encoding="utf-8")
    game_ui = (ROOT / "game/src/game_ui.c").read_text(encoding="utf-8")
    thread = (ROOT / "game/src/thread3_main.c").read_text(encoding="utf-8")
    audio = (ROOT / "game/src/audio_spatial.c").read_text(encoding="utf-8")
    assert_racer_contract(racer)
    assert_fish_contract(objects)
    assert_launcher_handoff(app)
    assert_process_gate_enters_through_launcher(app)
    assert_shared_player_count_contract(authority)
    assert_output_camera_split(camera, tracks)
    assert_endpoint_minimap_layout(game_ui)
    assert_endpoint_hud_reflow(game_ui)
    assert_audio_listener_split(audio)
    assert_no_render_chrome(thread)
    mutation_controls(
        racer, objects, app, camera, tracks, game_ui, thread, audio)
    print("network viewport authority contract passed")


if __name__ == "__main__":
    main()
