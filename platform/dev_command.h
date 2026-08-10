/**
 * dev_command.h — pure console command parser: tokenise, resolve, validate.
 *
 * The trap this module exists to close: a console reachable from inside a
 * running game is an arbitrary-write primitive the moment `set <key> <value>`
 * accepts a key it hasn't checked. This module's only reason to exist is to
 * sit between the raw line a player or a test types and the config system,
 * and refuse to resolve any key that isn't literally a row in the
 * video_config schema — see mdkr_video_key_from_name(), the SAME lookup the
 * ini loader and --video-set already use, so a key this module accepts is a
 * key every other entry point already accepts too, and never anything more.
 *
 * It is as dependency-free as controller_mapping.h on purpose: no ImGui, no
 * window, no execution of what it parses, so tests/test_dev_command.c can
 * exercise every rejection path without a game or a renderer running. A
 * later task's console window is the only thing that turns a validated
 * MdkrDevCommandResult into an actual warp or set.
 */
#ifndef MDKR64_DEV_COMMAND_H
#define MDKR64_DEV_COMMAND_H

#include "video_config.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest input line accepted, in bytes. Anything longer is rejected before a
 * single byte of it is copied anywhere. */
#define MDKR_DEV_COMMAND_LINE_MAX 1024
/*
 * Longest single token/argument, sized to hold an entire accepted line so a
 * legal line can never overflow a token buffer — the line-length check above
 * is the only capacity rejection this module needs to make.
 */
#define MDKR_DEV_COMMAND_ARG_MAX (MDKR_DEV_COMMAND_LINE_MAX + 1)
/* No verb in the table needs more than this many arguments; `set` is the
 * widest, at key + value. */
#define MDKR_DEV_COMMAND_MAX_ARGS 2
#define MDKR_DEV_COMMAND_ERROR_MAX 160

typedef enum MdkrDevCommandVerb {
    MDKR_DEV_CMD_HELP = 0,
    MDKR_DEV_CMD_WARP,
    MDKR_DEV_CMD_SET,
    MDKR_DEV_CMD_GET,
    MDKR_DEV_CMD_HASH,
    MDKR_DEV_CMD_DUMP,
    /* Also the "nothing in the verb table matched" sentinel — same
     * convention as mdkr_video_key_from_name() returning
     * MDKR_VIDEO_KEY_COUNT for a name it does not recognise. */
    MDKR_DEV_CMD_COUNT
} MdkrDevCommandVerb;

typedef struct MdkrDevCommandArg {
    char text[MDKR_DEV_COMMAND_ARG_MAX]; /* raw text, quotes stripped */
    long long int_value;                 /* valid iff this argument is an integer */
} MdkrDevCommandArg;

typedef struct MdkrDevCommandResult {
    int ok;                    /* 1 = parsed and validated; read the rest only then */
    MdkrDevCommandVerb verb;   /* MDKR_DEV_CMD_COUNT when the verb itself was unknown */
    int arg_count;
    MdkrDevCommandArg args[MDKR_DEV_COMMAND_MAX_ARGS];
    MdkrVideoKey key;           /* resolved for set/get, else MDKR_VIDEO_KEY_COUNT */
    char error[MDKR_DEV_COMMAND_ERROR_MAX]; /* one-line human-readable reason, "" when ok */
} MdkrDevCommandResult;

/*
 * Tokenises, resolves and validates one console line: whitespace-separated
 * tokens with double-quoted-string support, verb lookup, arity, argument
 * type, and — the property that matters — key resolution restricted to
 * exactly what video_config's schema already recognises. Never allocates,
 * never executes anything the line asks for, never reads game state.
 *
 * `length` is the number of bytes at `line` to consider. The line is NOT
 * assumed to be NUL-terminated: a caller reading from a fixed-size input
 * buffer passes its exact byte count, and an embedded NUL inside that range
 * is rejected rather than silently truncating the command at it.
 *
 * Always writes `*out` completely; check `out->ok` before reading anything
 * else in it.
 */
void mdkr_dev_command_parse(const char *line, size_t length,
                            MdkrDevCommandResult *out);

/*
 * The verb-table entry nearest `text` by edit distance, for suggesting a typo
 * fix ("warpp" -> "warp"). Returns NULL for NULL/empty input, or when nothing
 * in the table is a plausible match. The returned pointer is a literal from
 * the verb table — never allocated, never to be freed or written through.
 */
const char *mdkr_dev_command_suggest(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_DEV_COMMAND_H */
