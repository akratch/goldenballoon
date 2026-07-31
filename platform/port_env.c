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
