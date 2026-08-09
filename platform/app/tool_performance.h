// tool_performance.h — the performance window (F8).
//
// It adds no timing instrumentation. Every number it shows is one the pacing
// work already produces and tests/check_pacing_quality.py already reads:
// present_sched.c's [PRESENTPERF-HIST] / [PRESENTPERF] / [PRESENTPERF-LATENCY]
// rows and the [PRESENTSCHED-SUMMARY] line, plus the exported scalars
// (present_sched_present_policy_name/_rate/_kind, g_frameCounter,
// g_surfaceFrameCounter, g_simTickCounter). The histogram rows are read back
// out of the diagnostic log ring rather than recomputed, for the reason
// tool_diagnostics.h states: reading the rows means this window can never
// quietly disagree with the stream a gate would compare.
//
// THE CAVEAT IT MUST CARRY. Under MDKR_SYNTH_FIELDS the pacer synthesises a
// fixed field count per frame and the loop runs as fast as the machine allows,
// so the presentation and GPU numbers describe the harness, not the game.
// Reading them under synthetic fields has already misled measurement work on
// this project, so the window says so on screen whenever that variable is set —
// beside the numbers, not in a footnote.
#ifndef MDKR64_TOOL_PERFORMANCE_H
#define MDKR64_TOOL_PERFORMANCE_H

// MdkrDevToolDraw-compatible; registered into MDKR_TOOL_PERFORMANCE.
void ToolPerformance_draw(bool *open);

#endif  // MDKR64_TOOL_PERFORMANCE_H
