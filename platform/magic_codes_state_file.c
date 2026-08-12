#include "magic_codes_state_file.h"

#include "text_state_file.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
EM_JS(void, magic_codes_schedule_web_persist, (unsigned int generation), {
    const persist = (typeof Module.__mdkrPersist === "function")
        ? Module.__mdkrPersist({reason: "magic-codes", urgent: true})
        : new Promise((resolve, reject) => {
            FS.syncfs(false, error => error ? reject(error) : resolve());
        });
    Promise.resolve(persist).then(() => {
        if (typeof Module._magic_codes_report_persistence_success === "function") {
            Module._magic_codes_report_persistence_success(generation);
        }
    }).catch(error => {
        if (typeof Module._magic_codes_report_persistence_failure === "function") {
            Module._magic_codes_report_persistence_failure(generation);
        }
        if (typeof Module.__mdkrPersistFailed === "function") {
            Module.__mdkrPersistFailed(String(
                error && error.message ? error.message : error));
        }
    });
});

static void magic_codes_state_did_replace(void *context) {
    (void)context;
    magic_codes_schedule_web_persist(
        magic_codes_persistence_pending_generation());
}
#endif

const MagicCodesStateStorage *magic_codes_state_file_storage(void) {
    static const MdkrTextStateFileSpec spec = {
        "magic_codes_state.ini",
#ifdef __EMSCRIPTEN__
        magic_codes_state_did_replace,
#else
        NULL,
#endif
        NULL
    };
    static const MagicCodesStateStorage storage = {
        (void *)&spec, mdkr_text_state_file_read, mdkr_text_state_file_write
    };
    return &storage;
}
