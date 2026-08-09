/** dev_command.c — see dev_command.h. */
#include "dev_command.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum ArgType {
    ARG_STRING = 0,
    ARG_INT,
    ARG_VIDEO_KEY
} ArgType;

typedef struct ArgSpec {
    const char *name; /* argument name used in error text, e.g. "track" */
    ArgType type;
} ArgSpec;

typedef struct VerbSpec {
    const char *name;
    MdkrDevCommandVerb verb;
    int arg_count;
    ArgSpec args[MDKR_DEV_COMMAND_MAX_ARGS];
} VerbSpec;

/*
 * The whole console surface. Every verb this module accepts is a row here;
 * adding a command means adding a row, never a special case in the parser
 * below. `set`/`get` route their key argument through ARG_VIDEO_KEY, which is
 * what makes the schema check unconditional rather than something a new verb
 * could accidentally skip.
 */
static const VerbSpec s_verbs[] = {
    { "help", MDKR_DEV_CMD_HELP, 0, { { NULL, ARG_STRING }, { NULL, ARG_STRING } } },
    { "warp", MDKR_DEV_CMD_WARP, 1, { { "track", ARG_INT }, { NULL, ARG_STRING } } },
    { "set",  MDKR_DEV_CMD_SET,  2, { { "key", ARG_VIDEO_KEY }, { "value", ARG_STRING } } },
    { "get",  MDKR_DEV_CMD_GET,  1, { { "key", ARG_VIDEO_KEY }, { NULL, ARG_STRING } } },
    { "hash", MDKR_DEV_CMD_HASH, 0, { { NULL, ARG_STRING }, { NULL, ARG_STRING } } },
    { "dump", MDKR_DEV_CMD_DUMP, 1, { { "target", ARG_STRING }, { NULL, ARG_STRING } } },
};
#define VERB_COUNT ((int) (sizeof(s_verbs) / sizeof(s_verbs[0])))

/* Verb + its widest arity, the largest number of tokens one legal command can
 * ever produce. One slot beyond a verb's own arity is enough to notice "too
 * many arguments" without needing to store an unbounded tail. */
#define TOKEN_CAP (MDKR_DEV_COMMAND_MAX_ARGS + 1)

static int ci_equal(const char *a, const char *b) {
    if (a == NULL || b == NULL) {
        return 0;
    }
    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char) *a) != tolower((unsigned char) *b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static const VerbSpec *find_verb(const char *name) {
    for (int i = 0; i < VERB_COUNT; i++) {
        if (ci_equal(name, s_verbs[i].name)) {
            return &s_verbs[i];
        }
    }
    return NULL;
}

static void set_error(MdkrDevCommandResult *out, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(out->error, sizeof(out->error), format, args);
    va_end(args);
}

/*
 * Whitespace-separated tokens with double-quoted-string support. Every
 * character written into `tokens` passes through the same capacity check, so
 * a token can never overflow its MDKR_DEV_COMMAND_ARG_MAX buffer regardless
 * of how `mdkr_dev_command_parse` bounded `length` upstream — belt and
 * suspenders rather than one one single line-length check standing alone.
 *
 * Returns 0 and fills out->error on an unterminated quote or too many
 * tokens; returns 1 (including for zero tokens — an empty or whitespace-only
 * line) otherwise.
 */
static int tokenize(const char *line, size_t length,
                    char tokens[TOKEN_CAP][MDKR_DEV_COMMAND_ARG_MAX],
                    int *out_count, MdkrDevCommandResult *out) {
    size_t i = 0;
    int count = 0;

    while (i < length) {
        while (i < length && isspace((unsigned char) line[i])) {
            i++;
        }
        if (i >= length) {
            break;
        }
        if (count >= TOKEN_CAP) {
            set_error(out, "too many arguments");
            return 0;
        }

        char *dst = tokens[count];
        size_t dst_len = 0;
        if (line[i] == '"') {
            i++; /* consume opening quote */
            int closed = 0;
            while (i < length) {
                if (line[i] == '"') {
                    closed = 1;
                    i++;
                    break;
                }
                if (dst_len >= MDKR_DEV_COMMAND_ARG_MAX - 1) {
                    set_error(out, "argument too long");
                    return 0;
                }
                dst[dst_len++] = line[i];
                i++;
            }
            if (!closed) {
                set_error(out, "unterminated quote");
                return 0;
            }
        } else {
            while (i < length && !isspace((unsigned char) line[i])) {
                if (dst_len >= MDKR_DEV_COMMAND_ARG_MAX - 1) {
                    set_error(out, "argument too long");
                    return 0;
                }
                dst[dst_len++] = line[i];
                i++;
            }
        }
        dst[dst_len] = '\0';
        count++;
    }

    *out_count = count;
    return 1;
}

void mdkr_dev_command_parse(const char *line, size_t length,
                            MdkrDevCommandResult *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->verb = MDKR_DEV_CMD_COUNT;
    out->key = MDKR_VIDEO_KEY_COUNT;

    if (line == NULL) {
        set_error(out, "no command");
        return;
    }
    /* Reject purely on the declared length, before touching a single byte of
     * `line` — an oversized caller buffer can never be over-read here. */
    if (length > MDKR_DEV_COMMAND_LINE_MAX) {
        set_error(out, "command line exceeds the %d-byte limit",
                  MDKR_DEV_COMMAND_LINE_MAX);
        return;
    }
    /* `length` is authoritative, not a NUL terminator: an embedded NUL inside
     * the declared range must not be allowed to silently truncate the
     * command the caller thinks it is submitting. */
    for (size_t i = 0; i < length; i++) {
        if (line[i] == '\0') {
            set_error(out, "command line contains an embedded NUL byte");
            return;
        }
    }

    char tokens[TOKEN_CAP][MDKR_DEV_COMMAND_ARG_MAX];
    int token_count = 0;
    if (!tokenize(line, length, tokens, &token_count, out)) {
        return;
    }
    if (token_count == 0) {
        set_error(out, "empty command");
        return;
    }

    const VerbSpec *spec = find_verb(tokens[0]);
    if (spec == NULL) {
        set_error(out, "unknown command '%s'", tokens[0]);
        return;
    }

    int actual_args = token_count - 1;
    if (actual_args < spec->arg_count) {
        set_error(out, "%s: missing argument '%s'", spec->name,
                  spec->args[actual_args].name);
        return;
    }
    if (actual_args > spec->arg_count) {
        set_error(out, "%s: too many arguments (expected %d)", spec->name,
                  spec->arg_count);
        return;
    }

    for (int i = 0; i < spec->arg_count; i++) {
        const char *token = tokens[1 + i];
        const ArgSpec *arg_spec = &spec->args[i];
        size_t token_len = strlen(token); /* < MDKR_DEV_COMMAND_ARG_MAX, guaranteed by tokenize() */
        memcpy(out->args[i].text, token, token_len + 1);

        switch (arg_spec->type) {
        case ARG_INT: {
            char *end = NULL;
            errno = 0;
            long long value = strtoll(token, &end, 10);
            if (end == token || *end != '\0' || errno == ERANGE) {
                set_error(out, "%s: argument '%s' ('%s') is not an integer",
                          spec->name, arg_spec->name, token);
                return;
            }
            out->args[i].int_value = value;
            break;
        }
        case ARG_VIDEO_KEY: {
            /*
             * THE property that makes this module safe to reach from an
             * in-game console: only a name that mdkr_video_key_from_name()
             * — the same lookup the ini loader and --video-set use — already
             * recognises can ever become a resolved key. Everything else is
             * rejected here, before any caller sees a key at all.
             */
            MdkrVideoKey key = mdkr_video_key_from_name(token);
            if (key == MDKR_VIDEO_KEY_COUNT) {
                set_error(out, "%s: unknown key '%s'", spec->name, token);
                return;
            }
            out->key = key;
            break;
        }
        case ARG_STRING:
        default:
            break;
        }
    }

    out->ok = 1;
    out->verb = spec->verb;
    out->arg_count = spec->arg_count;
}

/* Edit distance is computed over a capped prefix so a pathological (but
 * still line-length-bounded) input can never grow the working rows; no verb
 * name is anywhere near this long. */
#define SUGGEST_MAX_LEN 31
/* A distance above this is treated as coincidence rather than a typo — see
 * mdkr_dev_command_suggest(). */
#define SUGGEST_THRESHOLD 3

static size_t capped_length(const char *s, size_t cap) {
    size_t n = 0;
    while (s[n] != '\0' && n < cap) {
        n++;
    }
    return n;
}

static int edit_distance(const char *a, const char *b) {
    size_t la = capped_length(a, SUGGEST_MAX_LEN);
    size_t lb = capped_length(b, SUGGEST_MAX_LEN);
    int prev[SUGGEST_MAX_LEN + 1];
    int curr[SUGGEST_MAX_LEN + 1];

    for (size_t j = 0; j <= lb; j++) {
        prev[j] = (int) j;
    }
    for (size_t i = 1; i <= la; i++) {
        curr[0] = (int) i;
        for (size_t j = 1; j <= lb; j++) {
            int cost = (tolower((unsigned char) a[i - 1]) ==
                        tolower((unsigned char) b[j - 1])) ? 0 : 1;
            int deletion = prev[j] + 1;
            int insertion = curr[j - 1] + 1;
            int substitution = prev[j - 1] + cost;
            int best = deletion < insertion ? deletion : insertion;
            if (substitution < best) {
                best = substitution;
            }
            curr[j] = best;
        }
        memcpy(prev, curr, sizeof(int) * (lb + 1));
    }
    return prev[lb];
}

const char *mdkr_dev_command_suggest(const char *text) {
    if (text == NULL || text[0] == '\0') {
        return NULL;
    }

    const char *best_name = NULL;
    int best_distance = -1;
    for (int i = 0; i < VERB_COUNT; i++) {
        int distance = edit_distance(text, s_verbs[i].name);
        if (best_name == NULL || distance < best_distance) {
            best_name = s_verbs[i].name;
            best_distance = distance;
        }
    }
    if (best_name == NULL || best_distance > SUGGEST_THRESHOLD) {
        return NULL;
    }
    return best_name;
}
