/* Atomic UTF-8 text sidecars in the application's save directory. */
#ifndef MDKR64_TEXT_STATE_FILE_H
#define MDKR64_TEXT_STATE_FILE_H

#include <stddef.h>

typedef int (*MdkrTextStateRead)(void *context, char *text, size_t capacity,
                                 size_t *length);
typedef int (*MdkrTextStateWrite)(void *context, const char *text,
                                  size_t length);

typedef struct MdkrTextStateStorage {
    void *context;
    MdkrTextStateRead read;
    MdkrTextStateWrite write;
} MdkrTextStateStorage;

typedef void (*MdkrTextStateDidReplace)(void *context);

typedef struct MdkrTextStateFileSpec {
    const char *filename;
    MdkrTextStateDidReplace did_replace;
    void *did_replace_context;
} MdkrTextStateFileSpec;

int mdkr_text_state_file_read(void *context, char *text, size_t capacity,
                              size_t *length);
int mdkr_text_state_file_write(void *context, const char *text, size_t length);

#endif
