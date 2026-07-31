# Input, menu interactivity and windowed play

> **This work has landed.** The document was written ahead of the M4 wave and
> is kept because it is still the clearest description of the input path and
> the binding conventions. Everything under "Goal" and "Acceptance" was
> achieved; read those sections as a description of what the build does, not
> as outstanding work.

## Goal
Boot to the main menu in a visible SDL window, navigate menus with keyboard and/or
SDL game controller, start a race, and reach in-race rendering. `--headless-frames`
keeps working for CI-style checks; interactive mode is the default when no
`--headless-frames` flag is given.

## Input path (game side — do not restructure)
- `game/src/joypad.c` wraps libultra SI: `osContInit` (boot), then per-frame
  `osContStartReadData`/`osContGetReadData` filling `OSContPad {s16 stick_x, stick_y;
  u16 button; u8 errno}` for 4 controllers.
- The shim (`platform/stubs_dkr.c`) currently returns neutral pads. Replace with a
  platform input state owned by `platform/platform_sdl_min.c`, updated in the SDL
  event pump each frame BEFORE the retrace message is synthesized (ordering matters:
  game reads pads after retrace wake).

## Default bindings (mirror mgb64 conventions, see mgb64 src/platform input files)
Keyboard (P1):
- Arrows = analog stick (full deflection ±80; add gradual ramp only if menus feel
  too fast — DKR menus read stick as digital)
- X = A, Z = B, C = Z-trigger, Enter = Start
- A/D = L/R shoulder, Q/E/W/S + IJKL = C-buttons (IJKL primary)
- Esc = quit (host-level, not mapped to the game)
SDL_GameController (any connected, P1 first): left stick = stick, A=A, B/X=B,
LT/RT... follow mgb64's mapping table; load lib/sdl_gamecontrollerdb.
- N64 button bit constants: use the game's own defs (game/include — grep
  A_BUTTON/B_BUTTON/Z_TRIG/START_BUTTON etc.), NOT SDL scancodes in game code.

## Save data
- EEPROM file backing already exists (save/eeprom.bin). Verify init_save_data's
  first-boot path writes defaults and menus read language/settings sanely.
- Controller Pak stays absent (ghosts disabled) — confirm menu tolerates.

## Frame pacing (interactive)
- DKR runs 30fps gameplay with 60Hz VI retrace. The shim's retrace synthesis is the
  pacer: target real-time 60Hz retrace messages (SDL_GL_SetSwapInterval(1) vsync
  where available; fall back to a clock-based limiter). Do not let headless mode
  sleep — it should run as fast as possible.

## Acceptance — all met

1. `./build/mdkr64` opens a window, boot flow reaches the main menu, and keyboard
   input navigates: highlight moves, A advances, B backs out.
2. Start Adventure/Tracks flow far enough to load into a race level; report how far
   in-race gets (rendering + racer control), crashes documented precisely.
3. `--headless-frames 600 --dump-frames` still exits 0; spot-check dumps show the
   menu progressing (input can be scripted via a simple `--input-script file` of
   frame:button pairs if useful for automation — optional, only if cheap).

