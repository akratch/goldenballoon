/**
 * config_ini.h — minimal INI reader/writer.
 *
 * Game-agnostic by design (modelled on mgb64 src/platform/config_pc.c, itself
 * modelled on the Perfect Dark port's config.c). Keys are flattened to
 * "Section.Key". Unknown keys round-trip unchanged so a config written by a
 * newer build survives being loaded and re-saved by an older one.
 */
#ifndef MDKR64_CONFIG_INI_H
#define MDKR64_CONFIG_INI_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_INI_KEY_MAX   128
#define CONFIG_INI_VALUE_MAX 1024

typedef struct ConfigIniEntry {
    char key[CONFIG_INI_KEY_MAX];
    char value[CONFIG_INI_VALUE_MAX];
} ConfigIniEntry;

/* Returns 1 on success, 0 on bad arguments, over-capacity keys/values/lines,
 * or if more than `max` entries were present. Input is never truncated. */
int config_ini_parse(const char *text, ConfigIniEntry *out, int max, int *out_count);

/* Returns 1 on success, 0 if `capacity` was insufficient or if any key or value
 * lies outside the grammar config_ini_parse can read back unchanged (leading or
 * trailing whitespace, an embedded newline, '=' in a key, or a key that would
 * parse as a section/comment). Everything this writes reloads as itself. */
int config_ini_serialize(const ConfigIniEntry *entries, int count,
                         char *out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_CONFIG_INI_H */
