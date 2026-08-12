#include "taj_mod_state_file.h"

#include "taj_mod.h"
#include "text_state_file.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
EM_JS(void, taj_mod_state_schedule_web_persist, (unsigned int generation), {
    const persist = (typeof Module.__mdkrPersist === "function")
        ? Module.__mdkrPersist({reason: "taj-mod", urgent: true})
        : new Promise((resolve, reject) => {
            FS.syncfs(false, error => error ? reject(error) : resolve());
        });
    Promise.resolve(persist).then(() => {
        if (typeof Module._taj_mod_report_persistence_success === "function") {
            Module._taj_mod_report_persistence_success(generation);
        }
    }).catch(error => {
        if (typeof Module._taj_mod_report_persistence_failure === "function") {
            Module._taj_mod_report_persistence_failure(generation);
        }
        if (typeof Module.__mdkrPersistFailed === "function") {
            Module.__mdkrPersistFailed(String(
                error && error.message ? error.message : error));
        }
    });
});

static void taj_mod_state_did_replace(void *context) {
    (void)context;
    taj_mod_state_schedule_web_persist(
        taj_mod_persistence_pending_generation());
}
#endif

const TajModStateStorage *taj_mod_state_file_storage(void) {
    static const MdkrTextStateFileSpec spec = {
        "taj_mod_state.ini",
#ifdef __EMSCRIPTEN__
        taj_mod_state_did_replace,
#else
        NULL,
#endif
        NULL
    };
    static const TajModStateStorage storage = {
        (void *)&spec, mdkr_text_state_file_read, mdkr_text_state_file_write
    };
    return &storage;
}
