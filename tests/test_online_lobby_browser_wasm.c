#include "platform/online/lobby_browser_wasm.h"
#include "platform/online/lobby_fake_adapter.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static int clean_text(const char *text, int required) {
    size_t length = 0u;
    if (text == NULL) return 0;
    while (length < 512u && text[length] != '\0') length++;
    return (!required || length != 0u) && length < 512u &&
        strchr(text, '|') == NULL && strchr(text, '\n') == NULL &&
        strchr(text, '\r') == NULL;
}

static void print_json_string(const char *text) {
    const unsigned char *cursor = (const unsigned char *)(text ? text : "");
    fputc('"', stdout);
    while (*cursor != '\0') {
        unsigned char value = *cursor++;
        if (value == '"' || value == '\\') {
            fputc('\\', stdout);
            fputc((int)value, stdout);
        } else if (value == '\b') {
            fputs("\\b", stdout);
        } else if (value == '\f') {
            fputs("\\f", stdout);
        } else if (value == '\n') {
            fputs("\\n", stdout);
        } else if (value == '\r') {
            fputs("\\r", stdout);
        } else if (value == '\t') {
            fputs("\\t", stdout);
        } else if (value < 0x20u) {
            fprintf(stdout, "\\u%04x", (unsigned)value);
        } else {
            fputc((int)value, stdout);
        }
    }
    fputc('"', stdout);
}

static int dump_presenter_models(void) {
    unsigned index;
    fputs("[\n", stdout);
    for (index = 0u; index < mdkr_online_browser_count(); index++) {
        unsigned slot;
        if (!mdkr_online_browser_select(index)) {
            fprintf(stderr, "could not select browser model %u\n", index);
            return 1;
        }
        if (index != 0u) fputs(",\n", stdout);
        fputs("  {\"slug\":", stdout);
        print_json_string(mdkr_online_browser_slug());
        fputs(",\"title\":", stdout);
        print_json_string(mdkr_online_browser_title());
        fputs(",\"explanation\":", stdout);
        print_json_string(mdkr_online_browser_explanation());
        fputs(",\"status\":", stdout);
        print_json_string(mdkr_online_browser_status());
        fputs(",\"verificationPhrase\":", stdout);
        print_json_string(mdkr_online_browser_verification_phrase());
        fprintf(stdout,
                ",\"kind\":%u,\"failure\":%u,\"announcement\":%u"
                ",\"members\":%u,\"seats\":%u,\"ready\":%u"
                ",\"admission\":%s,\"localPlay\":%s,\"timeoutVisible\":%s"
                ",\"timeoutTitle\":",
                mdkr_online_browser_kind(),
                mdkr_online_browser_failure(),
                mdkr_online_browser_announcement(),
                mdkr_online_browser_member_count(),
                mdkr_online_browser_seat_count(),
                mdkr_online_browser_ready_count(),
                mdkr_online_browser_requires_admission() ? "true" : "false",
                mdkr_online_browser_local_play_available() ? "true" : "false",
                mdkr_online_browser_timeout_visible() ? "true" : "false");
        print_json_string(mdkr_online_browser_timeout_title());
        fputs(",\"timeoutCopy\":", stdout);
        print_json_string(mdkr_online_browser_timeout_explanation());
        fputs(",\"controls\":[", stdout);
        for (slot = 0u; slot < 4u; slot++) {
            if (slot != 0u) fputc(',', stdout);
            fprintf(stdout, "{\"slot\":%u,\"action\":%u,\"label\":",
                    slot, mdkr_online_browser_control_action(slot));
            print_json_string(mdkr_online_browser_control_label(slot));
            fprintf(stdout, ",\"enabled\":%s}",
                    mdkr_online_browser_control_enabled(slot)
                        ? "true" : "false");
        }
        fputs("]}", stdout);
    }
    fputs("\n]\n", stdout);
    return ferror(stdout) ? 1 : 0;
}

static void test_initial_and_inventory_contract(void) {
    unsigned action_case[
        MDKR_ONLINE_VIEW_ACTION_REPORT_PHRASE_MISMATCH + 1u] = {0};
    unsigned action_seen[
        MDKR_ONLINE_VIEW_ACTION_REPORT_PHRASE_MISMATCH + 1u] = {0};
    unsigned kind_seen[MDKR_ONLINE_VIEW_RECOVERY + 1u] = {0};
    unsigned failure_seen[MDKR_ONLINE_VIEW_FAILURE_COUNT] = {0};
    unsigned phrase_count = 0u;
    unsigned index;

    expect(mdkr_online_browser_version() == MDKR_ONLINE_BROWSER_ABI_VERSION,
           "browser ABI publishes the exact supported version");
    expect(mdkr_online_browser_count() == 43u,
           "browser ABI publishes all 43 authoritative gallery cases");
    expect(mdkr_online_browser_slug()[0] == '\0' &&
           mdkr_online_browser_title()[0] == '\0' &&
           mdkr_online_browser_kind() == 0u &&
           mdkr_online_browser_dispatch(1u, 0u) == 0u &&
           !mdkr_online_browser_complete(),
           "unselected browser ABI is inert and returns bounded empty values");

    for (index = 0u; index < mdkr_online_browser_count(); index++) {
        const MdkrOnlineFakeGallerySpec *spec =
            mdkr_online_fake_gallery_at(index);
        unsigned kind;
        unsigned failure;
        unsigned slot;
        int timeout_case;

        expect(spec != NULL && mdkr_online_browser_select(index),
               "every authoritative gallery case projects through browser ABI");
        if (spec == NULL) continue;
        kind = mdkr_online_browser_kind();
        failure = mdkr_online_browser_failure();
        timeout_case = strncmp(spec->slug, "timeout-", 8u) == 0;
        expect(strcmp(mdkr_online_browser_slug(), spec->slug) == 0 &&
               kind == (unsigned)spec->kind &&
               failure == (unsigned)spec->failure,
               "browser semantic identity matches the gallery specification");
        expect(kind >= MDKR_ONLINE_VIEW_ENTRY &&
               kind <= MDKR_ONLINE_VIEW_RECOVERY &&
               failure < MDKR_ONLINE_VIEW_FAILURE_COUNT,
               "browser semantic enums stay within the public vocabulary");
        if (kind <= MDKR_ONLINE_VIEW_RECOVERY) kind_seen[kind] = 1u;
        if (failure < MDKR_ONLINE_VIEW_FAILURE_COUNT) failure_seen[failure] = 1u;
        expect(clean_text(mdkr_online_browser_slug(), 1) &&
               clean_text(mdkr_online_browser_title(), 1) &&
               clean_text(mdkr_online_browser_explanation(), 1) &&
               clean_text(mdkr_online_browser_status(), 0),
               "browser copy is bounded and safe for the gallery wire format");
        expect(mdkr_online_browser_announcement() <=
                   MDKR_ONLINE_ANNOUNCE_ASSERTIVE &&
               mdkr_online_browser_member_count() <= MDKR_ONLINE_MAX_ENDPOINTS &&
               mdkr_online_browser_seat_count() <= MDKR_ONLINE_MAX_SEATS &&
               mdkr_online_browser_ready_count() <=
                   mdkr_online_browser_member_count(),
               "browser counts and announcement priority remain bounded");
        expect(mdkr_online_browser_requires_admission() ==
                   (unsigned)spec->requires_race_admission,
               "browser fixture cannot invent or discard local admission policy");
        expect(mdkr_online_browser_timeout_visible() == (unsigned)timeout_case,
               "timeout visibility is owned by the prepared local clock result");
        expect((!timeout_case &&
                mdkr_online_browser_timeout_title()[0] == '\0' &&
                mdkr_online_browser_timeout_explanation()[0] == '\0') ||
               (timeout_case &&
                clean_text(mdkr_online_browser_timeout_title(), 1) &&
                clean_text(mdkr_online_browser_timeout_explanation(), 1)),
               "timeout copy appears only after the bounded local expiry");
        expect(mdkr_online_browser_control_action(0u) ==
                   (unsigned)spec->primary_action,
               "browser primary action matches the authoritative gallery spec");

        for (slot = 0u; slot < 4u; slot++) {
            unsigned action = mdkr_online_browser_control_action(slot);
            const char *label = mdkr_online_browser_control_label(slot);
            unsigned enabled = mdkr_online_browser_control_enabled(slot);
            expect(action <= MDKR_ONLINE_VIEW_ACTION_REPORT_PHRASE_MISMATCH,
                   "browser control action stays inside the ABI vocabulary");
            expect((action == 0u && label[0] == '\0' && enabled == 0u) ||
                   (action != 0u && clean_text(label, 1)),
                   "browser control label and visibility remain correlated");
            if (action != 0u && enabled != 0u) {
                action_seen[action] = 1u;
                if (action_case[action] == 0u) action_case[action] = index + 1u;
            }
        }
        expect(mdkr_online_browser_control_action(4u) == 0u &&
               mdkr_online_browser_control_label(4u)[0] == '\0' &&
               mdkr_online_browser_control_enabled(4u) == 0u,
               "out-of-range browser control slots return inert values");

        if (mdkr_online_browser_verification_phrase()[0] != '\0') {
            phrase_count++;
            expect(strcmp(spec->slug, "preflight") == 0 &&
                   strcmp(mdkr_online_browser_verification_phrase(),
                          "Nimble-Pilot Jolly-Star Sunny-Falcon") == 0 &&
                   mdkr_online_browser_control_action(0u) ==
                       MDKR_ONLINE_VIEW_ACTION_CONFIRM_PHRASE &&
                   strcmp(mdkr_online_browser_control_label(0u),
                          "Words Match") == 0 &&
                   mdkr_online_browser_control_action(1u) ==
                       MDKR_ONLINE_VIEW_ACTION_REPORT_PHRASE_MISMATCH &&
                   strcmp(mdkr_online_browser_control_label(1u),
                          "Words Differ") == 0 &&
                   !mdkr_online_browser_timeout_visible(),
                   "one human-paced phrase view owns both explicit decisions");
        }

        expect(mdkr_online_browser_dispatch(0u, 0u) == 0u &&
               mdkr_online_browser_dispatch(
                   MDKR_ONLINE_VIEW_ACTION_REPORT_PHRASE_MISMATCH + 1u,
                   0u) == 0u,
               "invalid browser actions reject without dispatch");
        expect(strcmp(mdkr_online_browser_slug(), spec->slug) == 0 &&
               mdkr_online_browser_kind() == kind &&
               mdkr_online_browser_failure() == failure,
               "rejected browser actions preserve the selected projection");
    }

    for (index = MDKR_ONLINE_VIEW_ENTRY;
         index <= MDKR_ONLINE_VIEW_RECOVERY; index++) {
        expect(kind_seen[index], "gallery covers every browser view kind");
    }
    for (index = 1u; index < MDKR_ONLINE_VIEW_FAILURE_COUNT; index++) {
        expect(failure_seen[index], "gallery covers every typed browser failure");
    }
    expect(phrase_count == 1u,
           "gallery exposes exactly one verification-phrase ceremony");
    for (index = 1u;
         index <= MDKR_ONLINE_VIEW_ACTION_REPORT_PHRASE_MISMATCH; index++) {
        unsigned result;
        expect(action_seen[index] && action_case[index] != 0u,
               "gallery exposes every browser action through an enabled control");
        if (action_case[index] == 0u) continue;
        expect(mdkr_online_browser_select(action_case[index] - 1u),
               "action fixture can be selected afresh");
        result = mdkr_online_browser_dispatch(index, 0u);
        expect(result == 1u || result == 2u,
               "every browser action reaches a reducer or launcher route");
    }
}

static void test_phrase_transition_and_live_fail_closed(void) {
    const MdkrOnlineFakeGallerySpec *room =
        mdkr_online_fake_gallery_find("room-friends");
    unsigned room_index = 0u;
    unsigned index;

    expect(room != NULL, "room-friends fixture exists");
    for (index = 0u; index < mdkr_online_browser_count(); index++) {
        if (mdkr_online_fake_gallery_at(index) == room) room_index = index;
    }
    expect(mdkr_online_browser_select(room_index) &&
           mdkr_online_browser_dispatch(MDKR_ONLINE_VIEW_ACTION_CHECK_SETUP,
                                        0u) == 1u &&
           mdkr_online_browser_pending() == MDKR_ONLINE_FAKE_PENDING_PREFLIGHT &&
           mdkr_online_browser_kind() == MDKR_ONLINE_VIEW_PREFLIGHT &&
           mdkr_online_browser_verification_phrase()[0] == '\0',
           "setup remains pending without publishing a phrase early");
    expect(mdkr_online_browser_complete() &&
           mdkr_online_browser_pending() == MDKR_ONLINE_FAKE_PENDING_NONE &&
           strcmp(mdkr_online_browser_verification_phrase(),
                  "Nimble-Pilot Jolly-Star Sunny-Falcon") == 0 &&
           mdkr_online_browser_control_action(0u) ==
               MDKR_ONLINE_VIEW_ACTION_CONFIRM_PHRASE,
           "authenticated-setup completion publishes the confirmation view");
    expect(!mdkr_online_browser_complete() &&
           mdkr_online_browser_dispatch(
               MDKR_ONLINE_VIEW_ACTION_CONFIRM_PHRASE, 0u) == 1u &&
           mdkr_online_browser_kind() == MDKR_ONLINE_VIEW_SELECTING &&
           mdkr_online_browser_verification_phrase()[0] == '\0',
           "Words Match alone advances to selection and clears phrase output");

    expect(mdkr_online_browser_select(room_index) &&
           mdkr_online_browser_dispatch(MDKR_ONLINE_VIEW_ACTION_CHECK_SETUP,
                                        0u) == 1u &&
           mdkr_online_browser_complete() &&
           mdkr_online_browser_dispatch(
               MDKR_ONLINE_VIEW_ACTION_REPORT_PHRASE_MISMATCH, 0u) == 1u &&
           mdkr_online_browser_kind() == MDKR_ONLINE_VIEW_RECOVERY &&
           mdkr_online_browser_failure() ==
               MDKR_ONLINE_VIEW_FAILURE_VERIFICATION_MISMATCH &&
           strcmp(mdkr_online_browser_title(), "Words Did Not Match") == 0 &&
           mdkr_online_browser_control_action(0u) ==
               MDKR_ONLINE_VIEW_ACTION_RETRY,
           "Words Differ reaches the dedicated secure recovery projection");
    expect(mdkr_online_browser_dispatch(MDKR_ONLINE_VIEW_ACTION_RETRY, 0u) ==
               1u &&
           mdkr_online_browser_pending() ==
               MDKR_ONLINE_FAKE_PENDING_PREFLIGHT &&
           mdkr_online_browser_complete() &&
           strcmp(mdkr_online_browser_verification_phrase(),
                  "Golden-Otter Rapid-Moon Velvet-Kite") == 0,
           "browser mismatch retry publishes a changed phrase");

    expect(mdkr_online_browser_live_project(
               MDKR_ROOM_OPEN, MDKR_ONLINE_LOBBY, 1u, 0u,
               0u, 0u, 1u, 1u, 1u, 0u, 1u, 0u, 0u,
               0u, 0u, 0u, 0u, 0u,
               MDKR_ONLINE_JOURNEY_CREATE,
               MDKR_ONLINE_VIEW_FAILURE_NONE, 1u),
           "valid minimized service snapshot projects through the live ABI");
    expect(strcmp(mdkr_online_browser_slug(), "live-room") == 0 &&
           mdkr_online_browser_kind() == MDKR_ONLINE_VIEW_ROOM &&
           !mdkr_online_browser_requires_admission() &&
           mdkr_online_browser_verification_phrase()[0] == '\0' &&
           mdkr_online_browser_dispatch(
               MDKR_ONLINE_VIEW_ACTION_SHARE_INVITE, 0u) == 0u,
           "live service data cannot supply phrase, admission or fake effects");
    expect(mdkr_online_browser_live_project(
               MDKR_ROOM_OPEN, MDKR_ONLINE_LOBBY, 1u, 0u,
               0u, 0u, 1u, 1u, 1u, 0u, 1u, 0u, 0u,
               0u, 0u, 0u, 0u, 0u,
               MDKR_ONLINE_JOURNEY_CREATE,
               MDKR_ONLINE_VIEW_FAILURE_NONE, 2u) &&
           strcmp(mdkr_online_browser_status(), "Invitation Expired") == 0 &&
           strcmp(mdkr_online_browser_control_label(0u), "New Invitation") == 0 &&
           mdkr_online_browser_control_enabled(0u),
           "live ABI exposes an explicit expired-invite replacement path");
    expect(!mdkr_online_browser_live_project(
               MDKR_ROOM_OPEN, MDKR_ONLINE_LOBBY, 1u, 0u,
               0u, 0u, 1u, 1u, 1u, 0u, 1u, 0u, 0u,
               0u, 0u, 0u, 0u, 0u,
               MDKR_ONLINE_JOURNEY_CREATE,
               MDKR_ONLINE_VIEW_FAILURE_NONE, 3u) &&
           strcmp(mdkr_online_browser_status(), "Invitation Expired") == 0,
           "invalid invite state rejects without replacing the prior model");
    expect(!mdkr_online_browser_live_project(
               MDKR_ROOM_PREFLIGHT, MDKR_ONLINE_LOBBY, 2u, 0u,
               0u, 0u, 1u, 1u, 1u, 0u, 1u, 0u, 0u,
               0u, 0u, 0u, 0u, 0u,
               MDKR_ONLINE_JOURNEY_CREATE,
               MDKR_ONLINE_VIEW_FAILURE_VERIFICATION_MISMATCH, 1u) &&
           strcmp(mdkr_online_browser_slug(), "live-room") == 0 &&
           mdkr_online_browser_kind() == MDKR_ONLINE_VIEW_ROOM,
           "service projection cannot synthesize a local phrase mismatch");
    expect(!mdkr_online_browser_live_project(
               0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
               0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u) &&
           strcmp(mdkr_online_browser_slug(), "live-room") == 0 &&
           mdkr_online_browser_kind() == MDKR_ONLINE_VIEW_ROOM,
           "invalid live projection rejects fail-atomically");
    expect(!mdkr_online_browser_select(mdkr_online_browser_count()) &&
           strcmp(mdkr_online_browser_slug(), "live-room") == 0,
           "invalid gallery selection preserves the prior live projection");
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--dump-presenter-json") == 0) {
        return dump_presenter_models();
    }
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--dump-presenter-json]\n", argv[0]);
        return 2;
    }
    test_initial_and_inventory_contract();
    test_phrase_transition_and_live_fail_closed();
    if (failures != 0) {
        fprintf(stderr, "%d browser ABI test(s) failed\n", failures);
        return 1;
    }
    puts("online browser ABI tests passed: 43 cases, 27 actions, phrase gate");
    return 0;
}
