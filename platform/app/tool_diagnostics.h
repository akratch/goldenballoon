// tool_diagnostics.h — the live-state window (F3) and the bug-report block.
//
// A tool is an OBSERVER (see dev_tools.h). This one reads published records
// only: the presentation scheduler's tick, the presentation snapshot's camera,
// the resolved video config, and the trace rows the engine has already written
// to the diagnostic log. It never samples a live game structure and never
// writes one, which is the claim check_dev_tools_purity.py tests.
#ifndef MDKR64_TOOL_DIAGNOSTICS_H
#define MDKR64_TOOL_DIAGNOSTICS_H

#include <string>
#include <vector>

// MdkrDevToolDraw-compatible; registered into MDKR_TOOL_DIAGNOSTICS.
void ToolDiagnostics_draw(bool *open);

// The plain-text block the window's copy action produces: everything a bug
// report needs about this build, this ROM, this machine and this moment, in an
// order someone reading an issue can scan. Also used by the console's own
// output, so the two can never describe the same run differently.
std::string ToolDiagnostics_copyBlock();

// The most recent `max` lines of the diagnostic log ring that contain `marker`,
// oldest first, each line from `marker` to its end.
//
// The log ring is the source on purpose. [EVENTHASH] and [SIMHASH] rows are
// written by the engine's own instruments; reading the rows back instead of
// re-deriving the numbers means a tool can never quietly disagree with the
// stream a gate would compare. Returns an empty list when the instrument that
// writes `marker` is switched off, which is the ordinary case.
std::vector<std::string> ToolDiagnostics_recentLogLines(const char *marker,
                                                        int max);

#endif  // MDKR64_TOOL_DIAGNOSTICS_H
