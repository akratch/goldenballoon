#include "mod_manifest.h"
#include <stdio.h>
#include <string.h>

static int failures;

static void expect(int cond, const char *what) {
    if (!cond) { printf("FAIL %s\n", what); failures++; }
    else       { printf("ok   %s\n", what); }
}

static void test_minimal_manifest_defaults(void) {
    static const char ini[] = "[pack]\nname = Sunset Skies\n";
    MdkrModManifest m;
    char err[128];
    int rc = mdkr_mod_manifest_parse(ini, sizeof(ini) - 1, &m, err, sizeof err);
    expect(rc == 0, "minimal manifest parses");
    expect(!strcmp(m.name, "Sunset Skies"), "name read");
    expect(m.priority == 100, "priority defaults to 100");
    expect(m.enabled == 1, "enabled defaults to on");
    expect(m.author[0] == '\0', "absent author is empty, not garbage");
}

static void test_missing_name_is_rejected(void) {
    static const char ini[] = "[pack]\nauthor = Somebody\n";
    MdkrModManifest m;
    char err[128];
    int rc = mdkr_mod_manifest_parse(ini, sizeof(ini) - 1, &m, err, sizeof err);
    expect(rc != 0, "manifest without a name is rejected");
    expect(strstr(err, "name") != NULL, "error names the missing key");
}

static void test_priority_out_of_range_is_rejected(void) {
    static const char ini[] = "[pack]\nname = X\npriority = 100000\n";
    MdkrModManifest m;
    char err[128];
    expect(mdkr_mod_manifest_parse(ini, sizeof(ini) - 1, &m, err, sizeof err) != 0,
           "priority above 9999 is rejected");
}

static void test_name_longer_than_field_is_rejected_not_truncated(void) {
    char ini[256];
    snprintf(ini, sizeof ini, "[pack]\nname = %0*d\n", 100, 0);
    MdkrModManifest m;
    char err[128];
    expect(mdkr_mod_manifest_parse(ini, strlen(ini), &m, err, sizeof err) != 0,
           "over-long name is rejected rather than silently truncated");
}

int main(void) {
    test_minimal_manifest_defaults();
    test_missing_name_is_rejected();
    test_priority_out_of_range_is_rejected();
    test_name_longer_than_field_is_rejected_not_truncated();
    printf(failures ? "FAILURES: %d\n" : "all manifest assertions passed\n", failures);
    return failures ? 1 : 0;
}
