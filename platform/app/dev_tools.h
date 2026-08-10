/* dev_tools.h — the registry every in-game diagnostic window goes through.
 *
 * A tool is an OBSERVER. Registration is also a claim: that opening this window
 * cannot move authoritative state. check_dev_tools_purity.py enumerates this
 * table and tests that claim for every entry, so the claim cannot rot.
 */
#ifndef MDKR64_DEV_TOOLS_H
#define MDKR64_DEV_TOOLS_H

#include <stdbool.h>

typedef enum MdkrDevToolId {
    MDKR_TOOL_DIAGNOSTICS = 0,
    MDKR_TOOL_CONSOLE,
    MDKR_TOOL_FREECAM,
    MDKR_TOOL_COLLISION,
    MDKR_TOOL_OBJECTS,
    MDKR_TOOL_PERFORMANCE,
    MDKR_TOOL_COUNT
} MdkrDevToolId;

typedef void (*MdkrDevToolDraw)(bool *open);

typedef struct MdkrDevTool {
    MdkrDevToolId    id;
    const char      *title;   /* window title, player-readable */
    const char      *hotkey;  /* display string, e.g. "F3" */
    MdkrDevToolDraw  draw;    /* NULL until its task lands */
} MdkrDevTool;

void DevTools_register(MdkrDevToolId id, MdkrDevToolDraw draw);
void DevTools_draw(void);            /* called once per frame from app_host */
bool DevTools_isOpen(MdkrDevToolId id);
void DevTools_setOpen(MdkrDevToolId id, bool open);
/* Prints one `[TOOLTABLE] id=... title=... hotkey=...` line per tool. */
void DevTools_dumpTable(void);

/* True while Tools.Enabled is on, i.e. while the host needs an ImGui frame it
 * would otherwise skip.
 *
 * NOT part of the registry: the render host has to decide whether to build a
 * frame BEFORE anything can be drawn into it, and the alternative was for
 * ui_overlay.cpp to read Tools.Enabled itself. That would put the gate in two
 * places, and the whole point of this file is that the gate is in one. Note
 * that this is deliberately not "is any tool open": the hotkeys that OPEN a
 * tool are dispatched from inside DevTools_draw(), so a frame that is only
 * built once something is already open can never see the key that opens it. */
bool DevTools_wantsFrame(void);

#endif /* MDKR64_DEV_TOOLS_H */
