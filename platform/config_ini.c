/**
 * config_ini.c — see config_ini.h.
 */
#include "config_ini.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static void config_ini_trim(char *s) {
    size_t len;
    char *start = s;

    while (*start != '\0' && isspace((unsigned char) *start)) {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    len = strlen(s);
    while (len > 0 && isspace((unsigned char) s[len - 1])) {
        s[--len] = '\0';
    }
}

int config_ini_parse(const char *text, ConfigIniEntry *out, int max, int *out_count) {
    char section[CONFIG_INI_KEY_MAX] = "";
    char line[CONFIG_INI_KEY_MAX + CONFIG_INI_VALUE_MAX + 4];
    const char *p = text;
    int count = 0;

    if (out_count != NULL) {
        *out_count = 0;
    }
    if (text == NULL || out == NULL || max <= 0) {
        return 0;
    }

    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t n = (eol != NULL) ? (size_t)(eol - p) : strlen(p);
        char *eq;

        if (n >= sizeof(line)) {
            return 0;
        }
        memcpy(line, p, n);
        line[n] = '\0';
        p = (eol != NULL) ? eol + 1 : p + n;

        config_ini_trim(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (line[0] == '[') {
            char *close = strchr(line, ']');
            if (close != NULL) {
                size_t section_length;
                *close = '\0';
                config_ini_trim(line + 1);
                section_length = strlen(line + 1);
                if (section_length >= sizeof(section)) {
                    return 0;
                }
                memcpy(section, line + 1, section_length + 1);
            }
            continue;
        }

        eq = strchr(line, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        config_ini_trim(line);
        config_ini_trim(eq + 1);

        if (count >= max) {
            return 0;
        }
        {
            size_t section_length = strlen(section);
            size_t key_length = strlen(line);
            size_t value_length = strlen(eq + 1);
            size_t flattened_length =
                section_length + (section_length != 0 ? 1u : 0u) +
                key_length;
            if (line[0] == '\0' ||
                flattened_length >= sizeof(out[count].key) ||
                value_length >= sizeof(out[count].value)) {
                return 0;
            }
        }
        if (section[0] != '\0') {
            size_t section_length = strlen(section);
            size_t key_length = strlen(line);
            memcpy(out[count].key, section, section_length);
            out[count].key[section_length] = '.';
            memcpy(
                out[count].key + section_length + 1,
                line,
                key_length + 1);
        } else {
            size_t key_length = strlen(line);
            memcpy(out[count].key, line, key_length + 1);
        }
        {
            size_t value_length = strlen(eq + 1);
            memcpy(out[count].value, eq + 1, value_length + 1);
        }
        count++;
    }

    if (out_count != NULL) {
        *out_count = count;
    }
    return 1;
}

int config_ini_serialize(const ConfigIniEntry *entries, int count,
                         char *out, size_t capacity) {
    char section[CONFIG_INI_KEY_MAX] = "";
    size_t used = 0;

    if (out == NULL || capacity == 0 || count < 0 ||
        (entries == NULL && count > 0)) {
        return 0;
    }
    out[0] = '\0';

    /*
     * INI has no portable "leave this section and return to root" marker.
     * Emit all root keys first, then sectioned keys. This preserves every key
     * and value even when the flattened input interleaves them.
     */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < count; i++) {
            const char *dot = strchr(entries[i].key, '.');
            char this_section[CONFIG_INI_KEY_MAX] = "";
            const char *leaf = entries[i].key;
            int written;

            if ((pass == 0) != (dot == NULL)) {
                continue;
            }
            if (dot != NULL) {
                size_t sl = (size_t) (dot - entries[i].key);
                if (sl >= sizeof(this_section)) {
                    return 0;
                }
                memcpy(this_section, entries[i].key, sl);
                this_section[sl] = '\0';
                leaf = dot + 1;
            }

            if (strcmp(this_section, section) != 0) {
                size_t section_length = strlen(this_section);
                memcpy(section, this_section, section_length + 1);
                written = snprintf(out + used, capacity - used,
                                   "%s[%s]\n", used > 0 ? "\n" : "", section);
                if (written < 0 || (size_t) written >= capacity - used) {
                    return 0;
                }
                used += (size_t) written;
            }

            written = snprintf(out + used, capacity - used, "%s=%s\n",
                               leaf, entries[i].value);
            if (written < 0 || (size_t) written >= capacity - used) {
                return 0;
            }
            used += (size_t) written;
        }
    }
    return 1;
}
