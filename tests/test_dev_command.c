/*
 * test_dev_command.c — pure parser tests, no window, no game.
 *
 * The single most important assertion in this file is the very first one:
 * `set` must refuse any key that is not a real row in the video_config
 * schema. Everything else here is ordinary parser hygiene; that one is the
 * reason the module exists, so it is written and read first.
 */
#include "dev_command.h"
#include "video_config.h"

#include <string.h>
#include <stdio.h>

static int failures;

static void expect(const char *message, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", message);
        failures++;
    }
}

int main(void) {
    MdkrDevCommandResult result;

    /* --- THE security property: set cannot become an arbitrary-write
     * primitive. A key that is not in the schema must never parse. --- */
    mdkr_dev_command_parse("set Not.A.Key 1", strlen("set Not.A.Key 1"), &result);
    expect("set refuses a key outside the schema", !result.ok);
    expect("rejection names the offending key",
           strstr(result.error, "Not.A.Key") != NULL);
    expect("an unschema'd key never resolves to a real MdkrVideoKey",
           result.key == MDKR_VIDEO_KEY_COUNT);

    /* --- help --- */
    mdkr_dev_command_parse("help", strlen("help"), &result);
    expect("help parses", result.ok);
    expect("help verb is help", result.verb == MDKR_DEV_CMD_HELP);
    expect("help takes zero arguments", result.arg_count == 0);

    /* --- warp --- */
    mdkr_dev_command_parse("warp 12", strlen("warp 12"), &result);
    expect("warp with an integer argument parses", result.ok);
    expect("warp verb is warp", result.verb == MDKR_DEV_CMD_WARP);
    expect("warp argument count is 1", result.arg_count == 1);
    expect("warp integer argument resolves to 12",
           result.args[0].int_value == 12);
    expect("warp integer argument text is preserved",
           strcmp(result.args[0].text, "12") == 0);

    mdkr_dev_command_parse("warp abc", strlen("warp abc"), &result);
    expect("warp with a non-integer argument is rejected", !result.ok);
    expect("rejection names the offending argument",
           strstr(result.error, "abc") != NULL);

    mdkr_dev_command_parse("warp", strlen("warp"), &result);
    expect("warp with no argument is rejected", !result.ok);
    expect("rejection names the missing argument",
           strstr(result.error, "track") != NULL);

    /* --- set / get resolve real keys --- */
    mdkr_dev_command_parse("set Video.RenderScale 2.0",
                           strlen("set Video.RenderScale 2.0"), &result);
    expect("set with a real key parses", result.ok);
    expect("set resolves the real MdkrVideoKey",
           result.key == MDKR_VIDEO_RENDER_SCALE);
    expect("set preserves the value text",
           strcmp(result.args[1].text, "2.0") == 0);

    mdkr_dev_command_parse("get Video.RenderScale",
                           strlen("get Video.RenderScale"), &result);
    expect("get with a real key parses", result.ok);
    expect("get resolves the real MdkrVideoKey",
           result.key == MDKR_VIDEO_RENDER_SCALE);

    /* --- unknown verb + suggestion --- */
    mdkr_dev_command_parse("warpp 12", strlen("warpp 12"), &result);
    expect("a near-miss verb is rejected", !result.ok);
    {
        const char *suggestion = mdkr_dev_command_suggest("warpp");
        expect("suggest finds the nearest verb",
               suggestion != NULL && strcmp(suggestion, "warp") == 0);
    }
    expect("suggest returns NULL for empty input",
           mdkr_dev_command_suggest("") == NULL);
    expect("suggest returns NULL for NULL input",
           mdkr_dev_command_suggest(NULL) == NULL);

    /* --- oversized input: rejected, never overflows --- */
    {
        char oversized[4096];
        memset(oversized, 'a', sizeof(oversized));
        memcpy(oversized, "warp ", 5); /* keep a plausible shape */
        mdkr_dev_command_parse(oversized, sizeof(oversized), &result);
        expect("a 4 KiB input line is rejected rather than overflowing",
               !result.ok);
    }

    /* --- embedded NUL byte: rejected via explicit length, not strlen --- */
    {
        char with_nul[8] = { 'w', 'a', 'r', 'p', ' ', '\0', '1', '2' };
        mdkr_dev_command_parse(with_nul, sizeof(with_nul), &result);
        expect("an embedded NUL byte mid-line is rejected", !result.ok);
    }

    /* --- quoted arguments --- */
    {
        const char *line = "set Video.Aspect \"4:3\"";
        mdkr_dev_command_parse(line, strlen(line), &result);
        expect("a quoted set parses", result.ok);
        expect("set resolves Video.Aspect", result.key == MDKR_VIDEO_ASPECT);
        expect("the quoted argument has its quotes stripped",
               strcmp(result.args[1].text, "4:3") == 0);
    }

    /* --- unterminated quote --- */
    {
        const char *line = "set Video.Aspect \"4:3";
        mdkr_dev_command_parse(line, strlen(line), &result);
        expect("an unterminated quote is rejected cleanly", !result.ok);
    }

    /* --- empty / whitespace-only input --- */
    mdkr_dev_command_parse("", 0, &result);
    expect("empty input is rejected cleanly", !result.ok);

    mdkr_dev_command_parse("   ", strlen("   "), &result);
    expect("whitespace-only input is rejected cleanly", !result.ok);

    mdkr_dev_command_parse(NULL, 0, &result);
    expect("a NULL line is rejected cleanly, not a crash", !result.ok);

    /* --- hash --- */
    mdkr_dev_command_parse("hash", strlen("hash"), &result);
    expect("hash parses", result.ok);
    expect("hash verb is hash", result.verb == MDKR_DEV_CMD_HASH);
    expect("hash takes zero arguments", result.arg_count == 0);

    mdkr_dev_command_parse("hash now", strlen("hash now"), &result);
    expect("hash with an extra argument is rejected (arity)", !result.ok);

    /* --- dump events --- */
    mdkr_dev_command_parse("dump events", strlen("dump events"), &result);
    expect("dump events parses", result.ok);
    expect("dump verb is dump", result.verb == MDKR_DEV_CMD_DUMP);
    expect("dump argument count is 1", result.arg_count == 1);
    expect("dump argument text is events",
           strcmp(result.args[0].text, "events") == 0);

    mdkr_dev_command_parse("dump", strlen("dump"), &result);
    expect("dump with no target is rejected (arity)", !result.ok);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("dev command parser passed\n");
    return 0;
}
