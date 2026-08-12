/*
 * port_env.c — implementation of the registering boolean flag accessor.
 * See port_env.h for the rationale and semantics.
 *
 * The registry is a small fixed-capacity table, appended to lazily the first
 * time each named flag is accessed. Lookups are a linear scan by name — fine for
 * the read-once pattern these gates use (and hot call sites already cache their
 * own result in a static). Not thread-safe: call from the main thread, which is
 * where these flags are read.
 */
#include "port_env.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name; /* caller's string literal (must be static/stable) */
    const char *help;
    int  parsed;   /* value has been read from the environment */
    int  was_set;  /* the environment variable was present */
    int  def;
    int  cur;
} Entry;

#define PORT_ENV_MAX 1024
static Entry s_entries[PORT_ENV_MAX];
static int   s_count = 0;

typedef struct {
    const char *name;
    const char *help;
    uint32_t def;
    uint32_t minimum;
    uint32_t maximum;
    uint32_t cur;
    int parsed;
} U32Entry;

#define PORT_ENV_U32_MAX 128
static U32Entry s_u32_entries[PORT_ENV_U32_MAX];
static int s_u32_count;

static Entry *find_entry(const char *name) {
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].name, name) == 0) {
            return &s_entries[i];
        }
    }
    return NULL;
}

static Entry *get_or_create(const char *name, const char *help) {
    Entry *e = find_entry(name);
    if (e != NULL) {
        return e;
    }
    if (s_count >= PORT_ENV_MAX) {
        return NULL; /* registry full: caller falls back to a direct parse */
    }
    e = &s_entries[s_count++];
    memset(e, 0, sizeof(*e));
    e->name = name;
    e->help = (help != NULL) ? help : "";
    return e;
}

int port_env_bool(const char *name, int default_on, const char *help) {
    const char *v;
    Entry *e = get_or_create(name, help);
    if (e == NULL) {
        v = getenv(name);
        if (v == NULL || v[0] == '\0') {
            return default_on ? 1 : 0;
        }
        return (v[0] == '0') ? 0 : 1;
    }
    if (!e->parsed) {
        v = getenv(name);
        e->was_set = (v != NULL);
        e->def = default_on ? 1 : 0;
        if (v == NULL || v[0] == '\0') {
            e->cur = default_on ? 1 : 0;
        } else {
            e->cur = (v[0] == '0') ? 0 : 1;
        }
        e->parsed = 1;
    }
    return e->cur;
}

static uint32_t parse_u32(
    const char *name, const char *value, uint32_t default_value,
    uint32_t minimum, uint32_t maximum) {
    char *end = NULL;
    unsigned long long parsed;
    if (value == NULL || value[0] == '\0') {
        return default_value;
    }
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' ||
        parsed > UINT32_MAX || parsed < minimum || parsed > maximum) {
        fprintf(stderr,
                "[ENV] %s=%s is invalid; using default=%" PRIu32 "\n",
                name, value, default_value);
        return default_value;
    }
    return (uint32_t)parsed;
}

uint32_t port_env_u32(
    const char *name, uint32_t default_value, uint32_t minimum,
    uint32_t maximum, const char *help) {
    U32Entry *entry = NULL;
    int index;
    if (name == NULL || name[0] == '\0' || minimum > maximum ||
        default_value < minimum || default_value > maximum) {
        return default_value;
    }
    for (index = 0; index < s_u32_count; index++) {
        if (strcmp(s_u32_entries[index].name, name) == 0) {
            entry = &s_u32_entries[index];
            break;
        }
    }
    if (entry == NULL) {
        if (s_u32_count >= PORT_ENV_U32_MAX) {
            return parse_u32(
                name, getenv(name), default_value, minimum, maximum);
        }
        entry = &s_u32_entries[s_u32_count++];
        memset(entry, 0, sizeof(*entry));
        entry->name = name;
        entry->help = help != NULL ? help : "";
        entry->def = default_value;
        entry->minimum = minimum;
        entry->maximum = maximum;
    }
    if (!entry->parsed) {
        entry->cur = parse_u32(
            name, getenv(name), default_value, minimum, maximum);
        entry->parsed = 1;
    }
    return entry->cur;
}
