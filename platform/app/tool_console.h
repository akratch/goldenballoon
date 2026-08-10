// tool_console.h — the command console (F4).
//
// The console does NOT parse. platform/dev_command.c owns tokenising,
// resolution and validation, and is tested without a window
// (tests/test_dev_command.c); this file only renders results and performs the
// side effects a validated command asks for. That split is what keeps `set`
// from becoming an arbitrary-write primitive: a key that reaches this file has
// already been resolved through the video_config schema and nothing else can
// get here.
#ifndef MDKR64_TOOL_CONSOLE_H
#define MDKR64_TOOL_CONSOLE_H

// MdkrDevToolDraw-compatible; registered into MDKR_TOOL_CONSOLE.
void ToolConsole_draw(bool *open);

#endif  // MDKR64_TOOL_CONSOLE_H
