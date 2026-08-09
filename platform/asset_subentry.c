/* Bounds-checked access to one entry inside an asset section. See
 * asset_subentry.h for why this exists and why its failure mode is an abort. */
#include "asset_subentry.h"

#include <stdio.h>
#include <stdlib.h>

/* The terminator every offset table in this tree ends with, read through the
 * unsigned view the descriptor uses. gAssetsMiscTable and friends store it as
 * s32 -1; the entry count is derived by walking to it, so it can only ever be
 * the value one PAST the last entry. Seeing it as a start offset means the count
 * and the table disagree. */
#define MDKR_SUBENTRY_TERMINATOR 0xFFFFFFFFu

_Static_assert(sizeof(uint32_t) == 4,
               "asset offset tables are 4-byte words in every DKR revision");

/* One formatter, so every abort from this file is greppable as one string and
 * the negative control has exactly one shape to match. The second line is for
 * whoever hits this with a ROM the build was not compiled for: the count is the
 * ROM's, the index is the build's. */
MDKR_SUBENTRY_NORETURN static void subentry_fail(const char *section_name,
                                                 long long index,
                                                 long long entry_count,
                                                 const char *where,
                                                 const char *detail) {
    fflush(stdout);
    fprintf(stderr,
            "[FATAL] asset sub-entry index out of range: section=%s index=%lld "
            "count=%lld at=%s\n",
            section_name != NULL ? section_name : "?", index, entry_count,
            where != NULL ? where : "?");
    fprintf(stderr,
            "        %s. This build asks for us.v80 sub-entry indices; a "
            "section with fewer entries than the index means the ROM revision "
            "and the build disagree.\n",
            detail != NULL ? detail : "index is not inside the section");
    fflush(stderr);
    abort();
}

void mdkr_asset_subentry_out_of_range(const char *section_name, long long index,
                                      long long entry_count,
                                      const char *where) {
    subentry_fail(section_name, index, entry_count, where,
                  "the caller's own bound rejected this index");
}

const uint8_t *mdkr_asset_subentry(const MdkrAssetSection *section,
                                   uint32_t index, uint32_t *out_size) {
    const char *name;
    uint32_t scale;
    uint32_t start;
    uint32_t end;
    uint64_t byte_start;
    uint64_t byte_size;

    if (out_size != NULL) {
        *out_size = 0;
    }
    if (section == NULL) {
        subentry_fail("?", (long long) index, -1, "mdkr_asset_subentry",
                      "no section descriptor was supplied");
    }
    name = section->name != NULL ? section->name : "?";
    /* Checked before the bound so "you asked before the section loaded" does not
     * masquerade as "your index is too large": an unloaded section has a zero
     * count and would otherwise fail the bound for every index. */
    if (section->base == NULL || section->offsets == NULL) {
        subentry_fail(name, (long long) index, (long long) section->entry_count,
                      "mdkr_asset_subentry",
                      "the section is not loaded (base or offset table is NULL)");
    }
    /* The bound, and the whole point of the file. Unsigned, so a negative index
     * converted by the caller arrives as a huge value and is rejected here
     * rather than read backwards off the section base. */
    if (index >= section->entry_count) {
        subentry_fail(name, (long long) index, (long long) section->entry_count,
                      "mdkr_asset_subentry",
                      "index is at or past the section's entry count");
    }

    scale = section->offset_scale != 0u ? section->offset_scale : 1u;
    start = section->offsets[index];
    if (start == MDKR_SUBENTRY_TERMINATOR) {
        /* Unreachable on a table whose count was derived by walking to the
         * terminator, so reaching it means the count outran the table. */
        subentry_fail(name, (long long) index, (long long) section->entry_count,
                      "mdkr_asset_subentry",
                      "the offset table terminates before this entry");
    }
    end = section->offsets[index + 1u];

    byte_start = (uint64_t) start * (uint64_t) scale;
    /* A terminator or a non-advancing successor means "length unknown", which is
     * what get_misc_asset_size() has always reported as 0. It is not an error:
     * the last usable entry of a terminated table legitimately lands here. */
    if (end != MDKR_SUBENTRY_TERMINATOR && end > start) {
        byte_size = (uint64_t) (end - start) * (uint64_t) scale;
    } else {
        byte_size = 0u;
    }
    if (byte_start > (uint64_t) UINT32_MAX || byte_size > (uint64_t) UINT32_MAX) {
        subentry_fail(name, (long long) index, (long long) section->entry_count,
                      "mdkr_asset_subentry",
                      "the entry's byte range does not fit in 32 bits");
    }
    if (out_size != NULL) {
        *out_size = (uint32_t) byte_size;
    }
    return section->base + (size_t) byte_start;
}

void mdkr_asset_subentry_env_selftest(void) {
    /* Negative control only. A synthetic one-entry section, so the abort this
     * drives is the accessor's own and needs no ROM data to be out of range. */
    static const uint32_t offsets[2] = {0u, 4u};
    static const uint8_t payload[4] = {0u, 0u, 0u, 0u};
    MdkrAssetSection section;
    const char *raw = getenv("MDKR_SUBENTRY_TEST_INDEX");
    char *stop = NULL;
    unsigned long long requested;

    if (raw == NULL || raw[0] == '\0') {
        return;
    }
    requested = strtoull(raw, &stop, 0);
    if (stop == raw) {
        fprintf(stderr,
                "[FATAL] MDKR_SUBENTRY_TEST_INDEX is not a number: '%s'\n", raw);
        fflush(stderr);
        abort();
    }
    /* Saturate rather than truncate: `MDKR_SUBENTRY_TEST_INDEX=4294967296` must
     * not silently become index 0, which is in range and would make the control
     * pass for the wrong reason. */
    if (requested > (unsigned long long) UINT32_MAX) {
        requested = (unsigned long long) UINT32_MAX;
    }
    section.name = "MDKR_SELFTEST";
    section.base = payload;
    section.entry_count = 1u;
    section.offsets = offsets;
    section.offset_scale = 1u;
    fprintf(stderr, "[SUBENTRY] selftest driving index %llu into a 1-entry section\n",
            requested);
    fflush(stderr);
    (void) mdkr_asset_subentry(&section, (uint32_t) requested, NULL);
    /* Reached only when the requested index was in range, which the gate treats
     * as a failure: the control must be non-vacuous. */
    fprintf(stderr, "[SUBENTRY] selftest index %llu resolved without aborting\n",
            requested);
    fflush(stderr);
}
