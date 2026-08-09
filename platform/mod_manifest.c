/**
 * mod_manifest.c — see mod_manifest.h.
 */
#include "mod_manifest.h"
#include "config_ini.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* config_ini_parse() takes no length: it walks its input with strchr()/
 * strlen() and trusts a NUL terminator. `ini_text`/`len` here explicitly do
 * not promise one, so the reader can never be handed the caller's buffer
 * directly. Copy the bounded `len` bytes into a NUL-terminated local buffer
 * first, and only then call anything that assumes termination. A pack.ini is
 * a handful of short "key = value" lines; anything past this cap is refused
 * outright rather than parsed on a silently truncated tail. */
#define MDKR_MOD_MANIFEST_TEXT_MAX 8192

/* Generous relative to a real pack.ini (five keys, one section). Sized so
 * that stray keys in an ignored, unrelated section cannot themselves turn
 * into a spurious parse failure. */
#define MDKR_MOD_MANIFEST_MAX_ENTRIES 128

static void set_error(char *err, size_t err_size, const char *message) {
    if (err == NULL || err_size == 0) {
        return;
    }
    snprintf(err, err_size, "%s", message);
}

static const char *find_value(const ConfigIniEntry *entries, int count, const char *key) {
    int i;

    for (i = 0; i < count; i++) {
        if (strcmp(entries[i].key, key) == 0) {
            return entries[i].value;
        }
    }
    return NULL;
}

/* Copies `value` into `dest` (capacity `dest_size`) with an explicit length
 * check. Returns 0 on success. Refuses — never truncates — when `value` does
 * not fit alongside its NUL terminator; a NULL `value` (key absent) always
 * fits and leaves `dest` as the caller zeroed it. */
static int copy_field(char *dest, size_t dest_size, const char *value,
                      const char *field_name, char *err, size_t err_size) {
    size_t length;

    if (value == NULL) {
        return 0;
    }
    length = strlen(value);
    if (length >= dest_size) {
        char message[160];
        snprintf(message, sizeof message,
                 "%s is longer than the %zu characters allowed",
                 field_name, dest_size - 1);
        set_error(err, err_size, message);
        return -1;
    }
    memcpy(dest, value, length + 1);
    return 0;
}

int mdkr_mod_manifest_parse(const char *ini_text, size_t len,
                            MdkrModManifest *out, char *err, size_t err_size) {
    char text[MDKR_MOD_MANIFEST_TEXT_MAX];
    ConfigIniEntry entries[MDKR_MOD_MANIFEST_MAX_ENTRIES];
    int entry_count = 0;
    const char *value;

    if (out == NULL) {
        set_error(err, err_size, "no output manifest supplied");
        return -1;
    }
    /* Requirement: absent optional fields must read back as empty strings,
     * never uninitialised stack, even on a path that returns early below. */
    memset(out, 0, sizeof(*out));

    if (ini_text == NULL && len != 0) {
        set_error(err, err_size, "no manifest text supplied");
        return -1;
    }
    if (len >= sizeof(text)) {
        set_error(err, err_size, "manifest text is too large to parse");
        return -1;
    }
    if (len > 0) {
        memcpy(text, ini_text, len);
    }
    text[len] = '\0';

    if (!config_ini_parse(text, entries, MDKR_MOD_MANIFEST_MAX_ENTRIES, &entry_count)) {
        set_error(err, err_size, "manifest is not valid INI");
        return -1;
    }

    /* Defaults land before any key is read, so an absent key and a
     * present-but-empty key produce the same manifest. */
    out->priority = 100;
    out->enabled = 1;

    /* Only "[pack]" is read; a key from any other section flattens to a key
     * this lookup never asks for, so unknown sections are ignored for free. */
    value = find_value(entries, entry_count, "pack.name");
    if (value == NULL || value[0] == '\0') {
        set_error(err, err_size, "manifest is missing required key 'name'");
        return -1;
    }
    if (copy_field(out->name, sizeof(out->name), value, "name", err, err_size) != 0) {
        return -1;
    }

    value = find_value(entries, entry_count, "pack.author");
    if (copy_field(out->author, sizeof(out->author), value, "author", err, err_size) != 0) {
        return -1;
    }

    value = find_value(entries, entry_count, "pack.version");
    if (copy_field(out->version, sizeof(out->version), value, "version", err, err_size) != 0) {
        return -1;
    }

    value = find_value(entries, entry_count, "pack.priority");
    if (value != NULL && value[0] != '\0') {
        char *end = NULL;
        long parsed = strtol(value, &end, 10);
        if (end == value || *end != '\0' || parsed < 0 || parsed > 9999) {
            set_error(err, err_size, "priority must be an integer from 0 to 9999");
            return -1;
        }
        out->priority = (int) parsed;
    }

    value = find_value(entries, entry_count, "pack.enabled");
    if (value != NULL && value[0] != '\0') {
        if (strcmp(value, "1") == 0) {
            out->enabled = 1;
        } else if (strcmp(value, "0") == 0) {
            out->enabled = 0;
        } else {
            set_error(err, err_size, "enabled must be 0 or 1");
            return -1;
        }
    }

    return 0;
}
