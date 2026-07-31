#include "gfx_webgpu_fault.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MDKR_WEBGPU_FAULT_SPEC_MAX 127u

static const char *const s_point_names[] = {
#define GFX_WEBGPU_FAULT_NAME(symbol, name) name,
    GFX_WEBGPU_FAULT_POINT_LIST(GFX_WEBGPU_FAULT_NAME)
#undef GFX_WEBGPU_FAULT_NAME
};

_Static_assert(
    sizeof(s_point_names) / sizeof(s_point_names[0]) ==
        GFX_WEBGPU_FAULT_COUNT,
    "fault-point enum/name table drift");

static unsigned long s_visits[GFX_WEBGPU_FAULT_COUNT];
static enum GfxWebgpuFaultPoint s_selected = GFX_WEBGPU_FAULT_COUNT;
static unsigned long s_occurrence = 1;
static bool s_configured;
static bool s_valid = true;
static bool s_trace;
static bool s_override_set;
static char s_override[MDKR_WEBGPU_FAULT_SPEC_MAX + 1u];
static char s_error[192];

size_t gfx_webgpu_fault_point_count(void) {
    return GFX_WEBGPU_FAULT_COUNT;
}

const char *gfx_webgpu_fault_point_name(size_t index) {
    return index < GFX_WEBGPU_FAULT_COUNT ? s_point_names[index] : NULL;
}

const char *gfx_webgpu_fault_error(void) {
    return s_error[0] != '\0' ? s_error : NULL;
}

static bool parse_bool_env(const char *name) {
    const char *value = getenv(name);
    return value != NULL && value[0] != '\0' &&
           strcmp(value, "0") != 0 && strcmp(value, "false") != 0 &&
           strcmp(value, "off") != 0;
}

static void set_error(const char *format, const char *value) {
    (void)snprintf(s_error, sizeof(s_error), format, value);
    s_valid = false;
}

bool gfx_webgpu_fault_configure(void) {
    const char *source;
    char spec[MDKR_WEBGPU_FAULT_SPEC_MAX + 1u];
    char *at;

    if (s_configured) {
        return s_valid;
    }
    s_configured = true;
    s_valid = true;
    s_error[0] = '\0';
    s_selected = GFX_WEBGPU_FAULT_COUNT;
    s_occurrence = 1;
    memset(s_visits, 0, sizeof(s_visits));
    s_trace = parse_bool_env("MDKR_WEBGPU_FAULT_TRACE");

    source = s_override_set ? s_override : getenv("MDKR_WEBGPU_FAULT");
    if (source == NULL || source[0] == '\0' || strcmp(source, "none") == 0) {
        return true;
    }
    if (strlen(source) > MDKR_WEBGPU_FAULT_SPEC_MAX) {
        set_error("MDKR_WEBGPU_FAULT is too long: '%s'", source);
        return false;
    }
    memcpy(spec, source, strlen(source) + 1u);
    at = strrchr(spec, '@');
    if (at != NULL) {
        char *end;
        unsigned long value;
        *at++ = '\0';
        if (*at == '\0') {
            set_error("missing occurrence in MDKR_WEBGPU_FAULT='%s'", source);
            return false;
        }
        if (strcmp(at, "all") == 0) {
            s_occurrence = 0;
        } else {
            errno = 0;
            value = strtoul(at, &end, 10);
            if (errno == ERANGE || value == 0 || value == ULONG_MAX ||
                end == at || *end != '\0') {
                set_error(
                    "invalid occurrence in MDKR_WEBGPU_FAULT='%s'", source);
                return false;
            }
            s_occurrence = value;
        }
    }
    if (spec[0] == '\0') {
        set_error("missing point in MDKR_WEBGPU_FAULT='%s'", source);
        return false;
    }
    for (size_t i = 0; i < GFX_WEBGPU_FAULT_COUNT; i++) {
        if (strcmp(spec, s_point_names[i]) == 0) {
            s_selected = (enum GfxWebgpuFaultPoint)i;
            return true;
        }
    }
    set_error("unknown MDKR_WEBGPU_FAULT point: '%s'", spec);
    return false;
}

bool gfx_webgpu_fault_hit(enum GfxWebgpuFaultPoint point) {
    if (!s_configured && !gfx_webgpu_fault_configure()) {
        return false;
    }
    if (point < 0 || point >= GFX_WEBGPU_FAULT_COUNT) {
        return false;
    }
    unsigned long occurrence = ++s_visits[point];
    if (s_trace) {
        fprintf(stderr, "[webgpu-fault] visit %s@%lu\n",
                s_point_names[point], occurrence);
    }
    if (s_valid && point == s_selected &&
        (s_occurrence == 0 || occurrence == s_occurrence)) {
        fprintf(stderr, "[webgpu-fault] injected %s@%lu\n",
                s_point_names[point], occurrence);
        fflush(stderr);
        return true;
    }
    return false;
}

bool gfx_webgpu_fault_selected(enum GfxWebgpuFaultPoint point) {
    if (!s_configured && !gfx_webgpu_fault_configure()) {
        return false;
    }
    return s_valid && point >= 0 && point < GFX_WEBGPU_FAULT_COUNT &&
           point == s_selected;
}

void gfx_webgpu_fault_set_spec_for_test(const char *spec) {
    if (spec == NULL) {
        s_override_set = false;
        s_override[0] = '\0';
    } else {
        size_t length = strlen(spec);
        if (length > MDKR_WEBGPU_FAULT_SPEC_MAX) {
            length = MDKR_WEBGPU_FAULT_SPEC_MAX;
        }
        memcpy(s_override, spec, length);
        s_override[length] = '\0';
        s_override_set = true;
    }
    gfx_webgpu_fault_reset_for_test();
}

void gfx_webgpu_fault_reset_for_test(void) {
    s_configured = false;
    s_valid = true;
    s_trace = false;
    s_selected = GFX_WEBGPU_FAULT_COUNT;
    s_occurrence = 1;
    s_error[0] = '\0';
    memset(s_visits, 0, sizeof(s_visits));
}
