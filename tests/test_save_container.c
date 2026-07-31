#include "save_container.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,   \
                    #condition);                                                \
            failures++;                                                         \
        }                                                                       \
    } while (0)

static void fill_payload(uint8_t payload[MDKR_SAVE_IMAGE_SIZE]) {
    size_t i;
    for (i = 0; i < MDKR_SAVE_IMAGE_SIZE; i++) {
        payload[i] = (uint8_t) ((i * 73u + 19u) & 0xFFu);
    }
}

static char *encode_fixture(const uint8_t payload[MDKR_SAVE_IMAGE_SIZE],
                            size_t *size_out) {
    MdkrSaveContainerMetadata metadata;
    size_t required = 0;
    char *container;
    memset(&metadata, 0, sizeof(metadata));
    strcpy(metadata.created_at, "2026-07-27T12:34:56.000Z");
    strcpy(metadata.app_version, "v0.3.2 \"test\"");
    strcpy(metadata.source, "web\nlocal");
    CHECK(mdkr_save_container_encode(payload, &metadata, NULL, 0, size_out,
                                     &required) == MDKR_SAVE_CONTAINER_OK);
    container = (char *) malloc(required);
    CHECK(container != NULL);
    if (container == NULL) {
        return NULL;
    }
    CHECK(mdkr_save_container_encode(payload, &metadata, container, required,
                                     size_out, NULL) ==
          MDKR_SAVE_CONTAINER_OK);
    CHECK(strlen(container) == *size_out);
    return container;
}

static void test_raw_and_container_round_trip(void) {
    uint8_t payload[MDKR_SAVE_IMAGE_SIZE];
    uint8_t decoded[MDKR_SAVE_IMAGE_SIZE];
    MdkrSaveInputFormat format = MDKR_SAVE_INPUT_CONTAINER;
    MdkrSaveContainerMetadata metadata;
    size_t size;
    char *container;

    fill_payload(payload);
    memset(decoded, 0xA5, sizeof(decoded));
    memset(&metadata, 0xA5, sizeof(metadata));
    CHECK(mdkr_save_container_decode(payload, sizeof(payload), decoded, &format,
                                     &metadata) ==
          MDKR_SAVE_CONTAINER_OK);
    CHECK(format == MDKR_SAVE_INPUT_RAW);
    CHECK(memcmp(decoded, payload, sizeof(payload)) == 0);
    CHECK(metadata.created_at[0] == '\0');

    container = encode_fixture(payload, &size);
    if (container == NULL) return;
    memset(decoded, 0xA5, sizeof(decoded));
    memset(&metadata, 0, sizeof(metadata));
    CHECK(mdkr_save_container_decode((const uint8_t *) container, size, decoded,
                                     &format, &metadata) ==
          MDKR_SAVE_CONTAINER_OK);
    CHECK(format == MDKR_SAVE_INPUT_CONTAINER);
    CHECK(memcmp(decoded, payload, sizeof(payload)) == 0);
    CHECK(strcmp(metadata.created_at, "2026-07-27T12:34:56.000Z") == 0);
    CHECK(strcmp(metadata.app_version, "v0.3.2 \"test\"") == 0);
    CHECK(strcmp(metadata.source, "web\nlocal") == 0);
    free(container);
}

static void test_atomic_failures(void) {
    uint8_t payload[MDKR_SAVE_IMAGE_SIZE];
    uint8_t decoded[MDKR_SAVE_IMAGE_SIZE];
    uint8_t sentinel[MDKR_SAVE_IMAGE_SIZE];
    char small[32];
    char small_before[32];
    size_t size;
    size_t required;
    char *container;
    char *digest;
    char *version;

    fill_payload(payload);
    container = encode_fixture(payload, &size);
    if (container == NULL) return;
    memset(decoded, 0xC3, sizeof(decoded));
    memcpy(sentinel, decoded, sizeof(decoded));
    CHECK(mdkr_save_container_decode((const uint8_t *) container, size - 2,
                                     decoded, NULL, NULL) ==
          MDKR_SAVE_CONTAINER_ERR_FORMAT);
    CHECK(memcmp(decoded, sentinel, sizeof(decoded)) == 0);

    digest = strstr(container, "\"sha256\":\"");
    CHECK(digest != NULL);
    if (digest != NULL) {
        digest += strlen("\"sha256\":\"");
        digest[0] = digest[0] == '0' ? '1' : '0';
        CHECK(mdkr_save_container_decode((const uint8_t *) container, size,
                                         decoded, NULL, NULL) ==
              MDKR_SAVE_CONTAINER_ERR_DIGEST);
        CHECK(memcmp(decoded, sentinel, sizeof(decoded)) == 0);
        digest[0] = digest[0] == '0' ? '1' : '0';
    }
    version = strstr(container, "\"version\":1");
    CHECK(version != NULL);
    if (version != NULL) {
        version[strlen("\"version\":")] = '2';
        CHECK(mdkr_save_container_decode((const uint8_t *) container, size,
                                         decoded, NULL, NULL) ==
              MDKR_SAVE_CONTAINER_ERR_VERSION);
        CHECK(memcmp(decoded, sentinel, sizeof(decoded)) == 0);
    }

    memset(small, 0x7B, sizeof(small));
    memcpy(small_before, small, sizeof(small));
    CHECK(mdkr_save_container_encode(payload, NULL, small, sizeof(small), NULL,
                                     &required) ==
          MDKR_SAVE_CONTAINER_ERR_CAPACITY);
    CHECK(required > sizeof(small));
    CHECK(memcmp(small, small_before, sizeof(small)) == 0);
    free(container);
}

static void test_unknown_fields_and_schema_strictness(void) {
    uint8_t payload[MDKR_SAVE_IMAGE_SIZE];
    uint8_t decoded[MDKR_SAVE_IMAGE_SIZE];
    size_t size;
    char *container;
    char *extended;
    const char addition[] =
        ",\"futureFeature\":{\"a-very-long-key-name\":[true,false,null,"
        "{\"nested\":\"ok\"}]}}\n";
    const char long_key_addition[] =
        ",\"this-unknown-key-is-deliberately-longer-than-the-parser-key-buffer-"
        "and-must-still-be-ignored\":{\"snowman\":\"\xE2\x98\x83\"}}\n";

    fill_payload(payload);
    container = encode_fixture(payload, &size);
    if (container == NULL) return;
    CHECK(size >= 2 && container[size - 2] == '}');
    extended = (char *) malloc(size + sizeof(addition));
    CHECK(extended != NULL);
    if (extended != NULL) {
        memcpy(extended, container, size - 2);
        memcpy(extended + size - 2, addition, sizeof(addition));
        CHECK(mdkr_save_container_decode(
                  (const uint8_t *) extended,
                  size - 2 + sizeof(addition) - 1, decoded, NULL, NULL) ==
              MDKR_SAVE_CONTAINER_OK);
        CHECK(memcmp(decoded, payload, sizeof(payload)) == 0);
        free(extended);
    }

    extended = (char *) malloc(size + sizeof(long_key_addition));
    CHECK(extended != NULL);
    if (extended != NULL) {
        memcpy(extended, container, size - 2);
        memcpy(extended + size - 2, long_key_addition,
               sizeof(long_key_addition));
        CHECK(mdkr_save_container_decode(
                  (const uint8_t *) extended,
                  size - 2 + sizeof(long_key_addition) - 1, decoded, NULL,
                  NULL) == MDKR_SAVE_CONTAINER_OK);
        CHECK(memcmp(decoded, payload, sizeof(payload)) == 0);
        free(extended);
    }

    {
        const char duplicate[] =
            "{\"format\":\"mdkr64-save\",\"format\":\"mdkr64-save\","
            "\"version\":1,\"payloadFormat\":\"dkr-eeprom-4k-be-v1\","
            "\"payload\":\"x\",\"sha256\":\"x\"}";
        CHECK(mdkr_save_container_decode((const uint8_t *) duplicate,
                                         strlen(duplicate), decoded, NULL,
                                         NULL) ==
              MDKR_SAVE_CONTAINER_ERR_FORMAT);
    }
    CHECK(mdkr_save_container_decode((const uint8_t *) "{}", 2, decoded, NULL,
                                     NULL) ==
          MDKR_SAVE_CONTAINER_ERR_FORMAT);
    CHECK(mdkr_save_container_decode((const uint8_t *) "", 0, decoded, NULL,
                                     NULL) ==
          MDKR_SAVE_CONTAINER_ERR_SIZE);
    free(container);
}

static void test_utf8_and_embedded_nul_rejection(void) {
    uint8_t payload[MDKR_SAVE_IMAGE_SIZE];
    uint8_t decoded[MDKR_SAVE_IMAGE_SIZE];
    MdkrSaveContainerMetadata metadata;
    size_t size;
    size_t required;
    char *container;
    char *source;
    const uint8_t malformed_utf8[] = {
        '{', '"', 0xC0, 0xAF, '"', ':', '0', '}'
    };

    fill_payload(payload);
    CHECK(mdkr_save_container_decode(malformed_utf8,
                                     sizeof(malformed_utf8), decoded, NULL,
                                     NULL) ==
          MDKR_SAVE_CONTAINER_ERR_FORMAT);

    container = encode_fixture(payload, &size);
    if (container == NULL) return;
    source = strstr(container, "\"source\":\"web");
    CHECK(source != NULL);
    if (source != NULL) {
        source = strchr(source + strlen("\"source\":"), '"');
        CHECK(source != NULL);
        if (source != NULL) {
            source[1] = (char) 0xED;
            source[2] = (char) 0xA0;
            source[3] = (char) 0x80; /* UTF-8 encoded surrogate. */
            CHECK(mdkr_save_container_decode(
                      (const uint8_t *) container, size, decoded, NULL,
                      NULL) == MDKR_SAVE_CONTAINER_ERR_FORMAT);
        }
    }
    free(container);

    memset(&metadata, 0, sizeof(metadata));
    memset(metadata.source, 'x', sizeof(metadata.source));
    CHECK(mdkr_save_container_encode(payload, &metadata, NULL, 0, &size,
                                     &required) ==
          MDKR_SAVE_CONTAINER_ERR_FORMAT);
    memset(&metadata, 0, sizeof(metadata));
    metadata.source[0] = (char) 0xC0;
    metadata.source[1] = (char) 0xAF;
    CHECK(mdkr_save_container_encode(payload, &metadata, NULL, 0, &size,
                                     &required) ==
          MDKR_SAVE_CONTAINER_ERR_FORMAT);

    {
        const char embedded_nul_key[] =
            "{\"format\\u0000ignored\":\"mdkr64-save\",\"version\":1,"
            "\"payloadFormat\":\"dkr-eeprom-4k-be-v1\",\"payload\":\"x\","
            "\"sha256\":\"x\"}";
        CHECK(mdkr_save_container_decode(
                  (const uint8_t *) embedded_nul_key,
                  strlen(embedded_nul_key), decoded, NULL, NULL) ==
              MDKR_SAVE_CONTAINER_ERR_FORMAT);
    }
}

static uint32_t fuzz_state = UINT32_C(0x5A17C0DE);

static uint32_t fuzz_random(void) {
    uint32_t value = fuzz_state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    fuzz_state = value;
    return value;
}

static void test_mutation_property(void) {
    uint8_t input[2048];
    uint8_t output[MDKR_SAVE_IMAGE_SIZE];
    uint8_t before[MDKR_SAVE_IMAGE_SIZE];
    unsigned iteration;
    for (iteration = 0; iteration < 10000; iteration++) {
        size_t size = fuzz_random() % sizeof(input);
        size_t i;
        MdkrSaveContainerResult result;
        for (i = 0; i < size; i++) input[i] = (uint8_t) fuzz_random();
        memset(output, 0x6D, sizeof(output));
        memcpy(before, output, sizeof(before));
        result = mdkr_save_container_decode(input, size, output, NULL, NULL);
        if (result != MDKR_SAVE_CONTAINER_OK) {
            CHECK(memcmp(output, before, sizeof(output)) == 0);
        }
    }
}

int main(void) {
    test_raw_and_container_round_trip();
    test_atomic_failures();
    test_unknown_fields_and_schema_strictness();
    test_utf8_and_embedded_nul_rejection();
    test_mutation_property();
    if (failures != 0) {
        fprintf(stderr, "save container: %d failure(s)\n", failures);
        return 1;
    }
    puts("save container: all tests passed");
    return 0;
}
