#include "save_container.h"

#include <limits.h>
#include <string.h>

#include "sha256.h"

typedef struct JsonParser {
    const uint8_t *cursor;
    const uint8_t *end;
    unsigned depth;
} JsonParser;

typedef struct JsonWriter {
    char *output;
    size_t capacity;
    size_t length;
} JsonWriter;

enum {
    FIELD_FORMAT = 1u << 0,
    FIELD_VERSION = 1u << 1,
    FIELD_PAYLOAD_FORMAT = 1u << 2,
    FIELD_PAYLOAD = 1u << 3,
    FIELD_SHA256 = 1u << 4,
    FIELD_CREATED_AT = 1u << 5,
    FIELD_APP_VERSION = 1u << 6,
    FIELD_SOURCE = 1u << 7,
    REQUIRED_FIELDS = FIELD_FORMAT | FIELD_VERSION | FIELD_PAYLOAD_FORMAT |
                      FIELD_PAYLOAD | FIELD_SHA256
};

static void json_skip_space(JsonParser *parser) {
    while (parser->cursor < parser->end &&
           (*parser->cursor == ' ' || *parser->cursor == '\t' ||
            *parser->cursor == '\r' || *parser->cursor == '\n')) {
        parser->cursor++;
    }
}

static int hex_digit(uint8_t value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static int json_read_hex4(JsonParser *parser, uint32_t *value_out) {
    uint32_t value = 0;
    unsigned i;
    if ((size_t) (parser->end - parser->cursor) < 4) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        int digit = hex_digit(parser->cursor[i]);
        if (digit < 0) {
            return 0;
        }
        value = (value << 4) | (uint32_t) digit;
    }
    parser->cursor += 4;
    *value_out = value;
    return 1;
}

static int append_utf8(char *output, size_t capacity, size_t *length,
                       uint32_t codepoint) {
    uint8_t encoded[4];
    size_t count;
    if (codepoint <= 0x7Fu) {
        encoded[0] = (uint8_t) codepoint;
        count = 1;
    } else if (codepoint <= 0x7FFu) {
        encoded[0] = (uint8_t) (0xC0u | (codepoint >> 6));
        encoded[1] = (uint8_t) (0x80u | (codepoint & 0x3Fu));
        count = 2;
    } else if (codepoint <= 0xFFFFu) {
        encoded[0] = (uint8_t) (0xE0u | (codepoint >> 12));
        encoded[1] = (uint8_t) (0x80u | ((codepoint >> 6) & 0x3Fu));
        encoded[2] = (uint8_t) (0x80u | (codepoint & 0x3Fu));
        count = 3;
    } else if (codepoint <= 0x10FFFFu) {
        encoded[0] = (uint8_t) (0xF0u | (codepoint >> 18));
        encoded[1] = (uint8_t) (0x80u | ((codepoint >> 12) & 0x3Fu));
        encoded[2] = (uint8_t) (0x80u | ((codepoint >> 6) & 0x3Fu));
        encoded[3] = (uint8_t) (0x80u | (codepoint & 0x3Fu));
        count = 4;
    } else {
        return 0;
    }
    if (*length > capacity || count > capacity - *length) {
        return 0;
    }
    if (output != NULL) {
        memcpy(output + *length, encoded, count);
    }
    *length += count;
    return 1;
}

static int decode_utf8(const uint8_t *input, const uint8_t *end,
                       uint32_t *codepoint_out, size_t *size_out) {
    uint32_t codepoint;
    uint32_t minimum;
    size_t count;
    size_t i;

    if (input >= end || *input < 0x80u) {
        return 0;
    }
    if (*input >= 0xC2u && *input <= 0xDFu) {
        codepoint = *input & 0x1Fu;
        minimum = 0x80u;
        count = 2;
    } else if (*input >= 0xE0u && *input <= 0xEFu) {
        codepoint = *input & 0x0Fu;
        minimum = 0x800u;
        count = 3;
    } else if (*input >= 0xF0u && *input <= 0xF4u) {
        codepoint = *input & 0x07u;
        minimum = 0x10000u;
        count = 4;
    } else {
        return 0;
    }
    if ((size_t) (end - input) < count) {
        return 0;
    }
    for (i = 1; i < count; i++) {
        if ((input[i] & 0xC0u) != 0x80u) {
            return 0;
        }
        codepoint = (codepoint << 6) | (input[i] & 0x3Fu);
    }
    if (codepoint < minimum || codepoint > 0x10FFFFu ||
        (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
        return 0;
    }
    *codepoint_out = codepoint;
    *size_out = count;
    return 1;
}

static int json_parse_string(JsonParser *parser, char *output,
                             size_t output_capacity, size_t *length_out) {
    size_t length = 0;
    if (parser->cursor >= parser->end || *parser->cursor++ != '"') {
        return 0;
    }
    while (parser->cursor < parser->end) {
        uint8_t value = *parser->cursor++;
        uint32_t codepoint;
        if (value == '"') {
            if (output != NULL) {
                if (length >= output_capacity) {
                    return 0;
                }
                output[length] = '\0';
            }
            if (length_out != NULL) {
                *length_out = length;
            }
            return 1;
        }
        if (value < 0x20u) {
            return 0;
        }
        if (value != '\\') {
            if (value >= 0x80u) {
                size_t encoded_size;
                if (!decode_utf8(parser->cursor - 1, parser->end, &codepoint,
                                 &encoded_size) ||
                    !append_utf8(output, output_capacity, &length,
                                 codepoint)) {
                    return 0;
                }
                parser->cursor += encoded_size - 1;
                continue;
            }
            if (length >= output_capacity) {
                return 0;
            }
            if (output != NULL) {
                output[length] = (char) value;
            }
            length++;
            continue;
        }
        if (parser->cursor >= parser->end) {
            return 0;
        }
        value = *parser->cursor++;
        switch (value) {
            case '"': codepoint = '"'; break;
            case '\\': codepoint = '\\'; break;
            case '/': codepoint = '/'; break;
            case 'b': codepoint = '\b'; break;
            case 'f': codepoint = '\f'; break;
            case 'n': codepoint = '\n'; break;
            case 'r': codepoint = '\r'; break;
            case 't': codepoint = '\t'; break;
            case 'u': {
                uint32_t first;
                if (!json_read_hex4(parser, &first)) {
                    return 0;
                }
                if (first >= 0xD800u && first <= 0xDBFFu) {
                    uint32_t second;
                    if ((size_t) (parser->end - parser->cursor) < 6 ||
                        parser->cursor[0] != '\\' ||
                        parser->cursor[1] != 'u') {
                        return 0;
                    }
                    parser->cursor += 2;
                    if (!json_read_hex4(parser, &second) ||
                        second < 0xDC00u || second > 0xDFFFu) {
                        return 0;
                    }
                    codepoint = UINT32_C(0x10000) +
                                ((first - 0xD800u) << 10) +
                                (second - 0xDC00u);
                } else {
                    if (first >= 0xDC00u && first <= 0xDFFFu) {
                        return 0;
                    }
                    codepoint = first;
                }
                break;
            }
            default: return 0;
        }
        /* Schema strings are represented as bounded C strings. Reject an
         * escaped NUL instead of silently truncating a required value or
         * metadata field during its later comparison/use. Unknown values are
         * validated with output == NULL and may still contain legal JSON NULs. */
        if (codepoint == 0 && output != NULL) {
            return 0;
        }
        if (!append_utf8(output, output_capacity, &length, codepoint)) {
            return 0;
        }
    }
    return 0;
}

static int json_skip_value(JsonParser *parser);

static int json_skip_object(JsonParser *parser) {
    if (parser->depth >= 16 || parser->cursor >= parser->end ||
        *parser->cursor++ != '{') {
        return 0;
    }
    parser->depth++;
    json_skip_space(parser);
    if (parser->cursor < parser->end && *parser->cursor == '}') {
        parser->cursor++;
        parser->depth--;
        return 1;
    }
    for (;;) {
        if (!json_parse_string(parser, NULL, SIZE_MAX, NULL)) {
            return 0;
        }
        json_skip_space(parser);
        if (parser->cursor >= parser->end || *parser->cursor++ != ':') {
            return 0;
        }
        json_skip_space(parser);
        if (!json_skip_value(parser)) {
            return 0;
        }
        json_skip_space(parser);
        if (parser->cursor >= parser->end) {
            return 0;
        }
        if (*parser->cursor == '}') {
            parser->cursor++;
            parser->depth--;
            return 1;
        }
        if (*parser->cursor++ != ',') {
            return 0;
        }
        json_skip_space(parser);
    }
}

static int json_skip_array(JsonParser *parser) {
    if (parser->depth >= 16 || parser->cursor >= parser->end ||
        *parser->cursor++ != '[') {
        return 0;
    }
    parser->depth++;
    json_skip_space(parser);
    if (parser->cursor < parser->end && *parser->cursor == ']') {
        parser->cursor++;
        parser->depth--;
        return 1;
    }
    for (;;) {
        if (!json_skip_value(parser)) {
            return 0;
        }
        json_skip_space(parser);
        if (parser->cursor >= parser->end) {
            return 0;
        }
        if (*parser->cursor == ']') {
            parser->cursor++;
            parser->depth--;
            return 1;
        }
        if (*parser->cursor++ != ',') {
            return 0;
        }
        json_skip_space(parser);
    }
}

static int json_skip_number(JsonParser *parser) {
    const uint8_t *start = parser->cursor;
    if (parser->cursor < parser->end && *parser->cursor == '-') {
        parser->cursor++;
    }
    if (parser->cursor >= parser->end) {
        return 0;
    }
    if (*parser->cursor == '0') {
        parser->cursor++;
    } else if (*parser->cursor >= '1' && *parser->cursor <= '9') {
        do {
            parser->cursor++;
        } while (parser->cursor < parser->end &&
                 *parser->cursor >= '0' && *parser->cursor <= '9');
    } else {
        return 0;
    }
    if (parser->cursor < parser->end && *parser->cursor == '.') {
        parser->cursor++;
        if (parser->cursor >= parser->end || *parser->cursor < '0' ||
            *parser->cursor > '9') {
            return 0;
        }
        while (parser->cursor < parser->end && *parser->cursor >= '0' &&
               *parser->cursor <= '9') {
            parser->cursor++;
        }
    }
    if (parser->cursor < parser->end &&
        (*parser->cursor == 'e' || *parser->cursor == 'E')) {
        parser->cursor++;
        if (parser->cursor < parser->end &&
            (*parser->cursor == '+' || *parser->cursor == '-')) {
            parser->cursor++;
        }
        if (parser->cursor >= parser->end || *parser->cursor < '0' ||
            *parser->cursor > '9') {
            return 0;
        }
        while (parser->cursor < parser->end && *parser->cursor >= '0' &&
               *parser->cursor <= '9') {
            parser->cursor++;
        }
    }
    return parser->cursor != start;
}

static int json_skip_literal(JsonParser *parser, const char *literal) {
    size_t length = strlen(literal);
    if ((size_t) (parser->end - parser->cursor) < length ||
        memcmp(parser->cursor, literal, length) != 0) {
        return 0;
    }
    parser->cursor += length;
    return 1;
}

static int json_skip_value(JsonParser *parser) {
    json_skip_space(parser);
    if (parser->cursor >= parser->end) {
        return 0;
    }
    switch (*parser->cursor) {
        case '{': return json_skip_object(parser);
        case '[': return json_skip_array(parser);
        case '"': return json_parse_string(parser, NULL, SIZE_MAX, NULL);
        case 't': return json_skip_literal(parser, "true");
        case 'f': return json_skip_literal(parser, "false");
        case 'n': return json_skip_literal(parser, "null");
        default: return json_skip_number(parser);
    }
}

static int base64_value(uint8_t value) {
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '+') return 62;
    if (value == '/') return 63;
    return -1;
}

static int base64_decode_512(const char *encoded, size_t encoded_length,
                             uint8_t output[MDKR_SAVE_IMAGE_SIZE]) {
    uint8_t decoded[MDKR_SAVE_IMAGE_SIZE];
    size_t input_offset;
    size_t output_offset = 0;
    if (encoded_length != 684 || encoded[683] != '=' ||
        encoded[682] == '=') {
        return 0;
    }
    for (input_offset = 0; input_offset < encoded_length; input_offset += 4) {
        int a = base64_value((uint8_t) encoded[input_offset]);
        int b = base64_value((uint8_t) encoded[input_offset + 1]);
        int c = encoded[input_offset + 2] == '='
                    ? -2
                    : base64_value((uint8_t) encoded[input_offset + 2]);
        int d = encoded[input_offset + 3] == '='
                    ? -2
                    : base64_value((uint8_t) encoded[input_offset + 3]);
        uint32_t value;
        if (a < 0 || b < 0 || c == -1 || d == -1) {
            return 0;
        }
        if ((c == -2 || d == -2) && input_offset + 4 != encoded_length) {
            return 0;
        }
        if (c == -2 && d != -2) {
            return 0;
        }
        if ((c == -2 && (b & 0x0F) != 0) ||
            (d == -2 && c >= 0 && (c & 0x03) != 0)) {
            return 0; /* Reject non-canonical encodings with nonzero pad bits. */
        }
        value = (uint32_t) a << 18 | (uint32_t) b << 12;
        if (c >= 0) value |= (uint32_t) c << 6;
        if (d >= 0) value |= (uint32_t) d;
        if (output_offset >= sizeof(decoded)) return 0;
        decoded[output_offset++] = (uint8_t) (value >> 16);
        if (c >= 0) {
            if (output_offset >= sizeof(decoded)) return 0;
            decoded[output_offset++] = (uint8_t) (value >> 8);
        }
        if (d >= 0) {
            if (output_offset >= sizeof(decoded)) return 0;
            decoded[output_offset++] = (uint8_t) value;
        }
    }
    if (output_offset != sizeof(decoded)) {
        return 0;
    }
    memcpy(output, decoded, sizeof(decoded));
    return 1;
}

static void base64_encode_512(const uint8_t input[MDKR_SAVE_IMAGE_SIZE],
                              char output[685]) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t in_offset = 0;
    size_t out_offset = 0;
    while (in_offset + 3 <= MDKR_SAVE_IMAGE_SIZE) {
        uint32_t value = (uint32_t) input[in_offset] << 16 |
                         (uint32_t) input[in_offset + 1] << 8 |
                         input[in_offset + 2];
        output[out_offset++] = alphabet[(value >> 18) & 63u];
        output[out_offset++] = alphabet[(value >> 12) & 63u];
        output[out_offset++] = alphabet[(value >> 6) & 63u];
        output[out_offset++] = alphabet[value & 63u];
        in_offset += 3;
    }
    if (in_offset < MDKR_SAVE_IMAGE_SIZE) {
        uint32_t value = (uint32_t) input[in_offset] << 16;
        output[out_offset++] = alphabet[(value >> 18) & 63u];
        if (in_offset + 1 < MDKR_SAVE_IMAGE_SIZE) {
            value |= (uint32_t) input[in_offset + 1] << 8;
            output[out_offset++] = alphabet[(value >> 12) & 63u];
            output[out_offset++] = alphabet[(value >> 6) & 63u];
            output[out_offset++] = '=';
        } else {
            output[out_offset++] = alphabet[(value >> 12) & 63u];
            output[out_offset++] = '=';
            output[out_offset++] = '=';
        }
    }
    output[out_offset] = '\0';
}

static unsigned field_for_key(const char *key) {
    if (strcmp(key, "format") == 0) return FIELD_FORMAT;
    if (strcmp(key, "version") == 0) return FIELD_VERSION;
    if (strcmp(key, "payloadFormat") == 0) return FIELD_PAYLOAD_FORMAT;
    if (strcmp(key, "payload") == 0) return FIELD_PAYLOAD;
    if (strcmp(key, "sha256") == 0) return FIELD_SHA256;
    if (strcmp(key, "createdAt") == 0) return FIELD_CREATED_AT;
    if (strcmp(key, "appVersion") == 0) return FIELD_APP_VERSION;
    if (strcmp(key, "source") == 0) return FIELD_SOURCE;
    return 0;
}

static int bounded_utf8_string(const char *text, size_t capacity) {
    const uint8_t *cursor = (const uint8_t *) text;
    const uint8_t *end;
    const char *terminator;

    if (text == NULL) {
        return 0;
    }
    terminator = (const char *) memchr(text, '\0', capacity);
    if (terminator == NULL) {
        return 0;
    }
    end = (const uint8_t *) terminator;
    while (cursor < end) {
        if (*cursor < 0x80u) {
            if (*cursor < 0x20u && *cursor != '\b' && *cursor != '\f' &&
                *cursor != '\n' && *cursor != '\r' && *cursor != '\t') {
                return 0;
            }
            cursor++;
        } else {
            uint32_t codepoint;
            size_t encoded_size;
            if (!decode_utf8(cursor, end, &codepoint, &encoded_size)) {
                return 0;
            }
            (void) codepoint;
            cursor += encoded_size;
        }
    }
    return 1;
}

MdkrSaveContainerResult mdkr_save_container_decode(
    const uint8_t *input, size_t input_size,
    uint8_t payload_out[MDKR_SAVE_IMAGE_SIZE], MdkrSaveInputFormat *format_out,
    MdkrSaveContainerMetadata *metadata_out) {
    JsonParser parser;
    MdkrSaveContainerMetadata metadata;
    uint8_t payload[MDKR_SAVE_IMAGE_SIZE];
    char format[32] = "";
    char payload_format[32] = "";
    char payload_base64[685] = "";
    char expected_digest[65] = "";
    unsigned fields = 0;

    if (input == NULL || payload_out == NULL) {
        return MDKR_SAVE_CONTAINER_ERR_ARGUMENT;
    }
    if (input_size == MDKR_SAVE_IMAGE_SIZE) {
        memcpy(payload_out, input, MDKR_SAVE_IMAGE_SIZE);
        if (format_out != NULL) *format_out = MDKR_SAVE_INPUT_RAW;
        if (metadata_out != NULL) memset(metadata_out, 0, sizeof(*metadata_out));
        return MDKR_SAVE_CONTAINER_OK;
    }
    if (input_size == 0 || input_size > MDKR_SAVE_CONTAINER_MAX_INPUT) {
        return MDKR_SAVE_CONTAINER_ERR_SIZE;
    }
    memset(&metadata, 0, sizeof(metadata));
    parser.cursor = input;
    parser.end = input + input_size;
    parser.depth = 0;
    json_skip_space(&parser);
    if (parser.cursor >= parser.end || *parser.cursor++ != '{') {
        return MDKR_SAVE_CONTAINER_ERR_FORMAT;
    }
    json_skip_space(&parser);
    if (parser.cursor < parser.end && *parser.cursor == '}') {
        return MDKR_SAVE_CONTAINER_ERR_FORMAT;
    }
    for (;;) {
        JsonParser key_parser;
        char key[64];
        unsigned field;
        size_t key_length = 0;
        size_t value_length = 0;
        key_parser = parser;
        if (!json_parse_string(&parser, NULL, SIZE_MAX, &key_length)) {
            return MDKR_SAVE_CONTAINER_ERR_FORMAT;
        }
        if (key_length < sizeof(key)) {
            if (!json_parse_string(&key_parser, key, sizeof(key), NULL)) {
                return MDKR_SAVE_CONTAINER_ERR_FORMAT;
            }
            field = field_for_key(key);
        } else {
            field = 0;
        }
        json_skip_space(&parser);
        if (parser.cursor >= parser.end || *parser.cursor++ != ':') {
            return MDKR_SAVE_CONTAINER_ERR_FORMAT;
        }
        json_skip_space(&parser);
        if (field != 0 && (fields & field) != 0) {
            return MDKR_SAVE_CONTAINER_ERR_FORMAT;
        }
        switch (field) {
            case FIELD_FORMAT:
                if (!json_parse_string(&parser, format, sizeof(format),
                                       &value_length)) {
                    return MDKR_SAVE_CONTAINER_ERR_FORMAT;
                }
                break;
            case FIELD_VERSION: {
                const uint8_t *start = parser.cursor;
                if (!json_skip_number(&parser) ||
                    (size_t) (parser.cursor - start) != 1 || *start != '1') {
                    return MDKR_SAVE_CONTAINER_ERR_VERSION;
                }
                break;
            }
            case FIELD_PAYLOAD_FORMAT:
                if (!json_parse_string(&parser, payload_format,
                                       sizeof(payload_format), &value_length)) {
                    return MDKR_SAVE_CONTAINER_ERR_FORMAT;
                }
                break;
            case FIELD_PAYLOAD:
                if (!json_parse_string(&parser, payload_base64,
                                       sizeof(payload_base64), &value_length) ||
                    value_length != 684) {
                    return MDKR_SAVE_CONTAINER_ERR_PAYLOAD;
                }
                break;
            case FIELD_SHA256:
                if (!json_parse_string(&parser, expected_digest,
                                       sizeof(expected_digest), &value_length) ||
                    value_length != 64) {
                    return MDKR_SAVE_CONTAINER_ERR_DIGEST;
                }
                break;
            case FIELD_CREATED_AT:
                if (!json_parse_string(&parser, metadata.created_at,
                                       sizeof(metadata.created_at),
                                       &value_length)) {
                    return MDKR_SAVE_CONTAINER_ERR_FORMAT;
                }
                break;
            case FIELD_APP_VERSION:
                if (!json_parse_string(&parser, metadata.app_version,
                                       sizeof(metadata.app_version),
                                       &value_length)) {
                    return MDKR_SAVE_CONTAINER_ERR_FORMAT;
                }
                break;
            case FIELD_SOURCE:
                if (!json_parse_string(&parser, metadata.source,
                                       sizeof(metadata.source), &value_length)) {
                    return MDKR_SAVE_CONTAINER_ERR_FORMAT;
                }
                break;
            default:
                if (!json_skip_value(&parser)) {
                    return MDKR_SAVE_CONTAINER_ERR_FORMAT;
                }
                break;
        }
        fields |= field;
        json_skip_space(&parser);
        if (parser.cursor >= parser.end) {
            return MDKR_SAVE_CONTAINER_ERR_FORMAT;
        }
        if (*parser.cursor == '}') {
            parser.cursor++;
            break;
        }
        if (*parser.cursor++ != ',') {
            return MDKR_SAVE_CONTAINER_ERR_FORMAT;
        }
        json_skip_space(&parser);
    }
    json_skip_space(&parser);
    if (parser.cursor != parser.end ||
        (fields & REQUIRED_FIELDS) != REQUIRED_FIELDS) {
        return MDKR_SAVE_CONTAINER_ERR_FORMAT;
    }
    if (strcmp(format, MDKR_SAVE_CONTAINER_FORMAT) != 0 ||
        strcmp(payload_format, MDKR_SAVE_CONTAINER_PAYLOAD_FORMAT) != 0) {
        return MDKR_SAVE_CONTAINER_ERR_VERSION;
    }
    if (!base64_decode_512(payload_base64, 684, payload)) {
        return MDKR_SAVE_CONTAINER_ERR_PAYLOAD;
    }
    {
        char actual_digest[MDKR_SHA256_HEX_SIZE];
        uint8_t mismatch = 0;
        size_t i;
        mdkr_sha256_hex(payload, sizeof(payload), actual_digest);
        for (i = 0; i < 64; i++) {
            mismatch |= (uint8_t) actual_digest[i] ^
                        (uint8_t) expected_digest[i];
        }
        if (mismatch != 0) {
            return MDKR_SAVE_CONTAINER_ERR_DIGEST;
        }
    }
    memcpy(payload_out, payload, sizeof(payload));
    if (format_out != NULL) *format_out = MDKR_SAVE_INPUT_CONTAINER;
    if (metadata_out != NULL) *metadata_out = metadata;
    return MDKR_SAVE_CONTAINER_OK;
}

static void writer_bytes(JsonWriter *writer, const char *bytes, size_t count) {
    if (writer->output != NULL && writer->length < writer->capacity) {
        size_t available = writer->capacity - writer->length;
        size_t copy = count < available ? count : available;
        memcpy(writer->output + writer->length, bytes, copy);
    }
    writer->length += count;
}

static void writer_literal(JsonWriter *writer, const char *text) {
    writer_bytes(writer, text, strlen(text));
}

static void writer_json_string(JsonWriter *writer, const char *text) {
    static const char hex[] = "0123456789abcdef";
    writer_literal(writer, "\"");
    while (*text != '\0') {
        unsigned char value = (unsigned char) *text++;
        switch (value) {
            case '"': writer_literal(writer, "\\\""); break;
            case '\\': writer_literal(writer, "\\\\"); break;
            case '\b': writer_literal(writer, "\\b"); break;
            case '\f': writer_literal(writer, "\\f"); break;
            case '\n': writer_literal(writer, "\\n"); break;
            case '\r': writer_literal(writer, "\\r"); break;
            case '\t': writer_literal(writer, "\\t"); break;
            default:
                if (value < 0x20u) {
                    char escaped[6] = {
                        '\\', 'u', '0', '0', hex[value >> 4],
                        hex[value & 0x0Fu]
                    };
                    writer_bytes(writer, escaped, sizeof(escaped));
                } else {
                    writer_bytes(writer, (const char *) &value, 1);
                }
                break;
        }
    }
    writer_literal(writer, "\"");
}

static void render_container(JsonWriter *writer, const char encoded[685],
                             const char digest[MDKR_SHA256_HEX_SIZE],
                             const MdkrSaveContainerMetadata *metadata) {
    writer_literal(writer, "{\"format\":\"" MDKR_SAVE_CONTAINER_FORMAT
                           "\",\"version\":1,\"payloadFormat\":\""
                           MDKR_SAVE_CONTAINER_PAYLOAD_FORMAT
                           "\",\"payload\":\"");
    writer_literal(writer, encoded);
    writer_literal(writer, "\",\"sha256\":\"");
    writer_literal(writer, digest);
    writer_literal(writer, "\",\"createdAt\":");
    writer_json_string(writer, metadata->created_at);
    writer_literal(writer, ",\"appVersion\":");
    writer_json_string(writer, metadata->app_version);
    writer_literal(writer, ",\"source\":");
    writer_json_string(writer, metadata->source);
    writer_literal(writer, "}\n");
}

MdkrSaveContainerResult mdkr_save_container_encode(
    const uint8_t payload[MDKR_SAVE_IMAGE_SIZE],
    const MdkrSaveContainerMetadata *metadata, char *output,
    size_t output_capacity, size_t *output_size, size_t *required_capacity) {
    MdkrSaveContainerMetadata empty_metadata;
    JsonWriter writer;
    char encoded[685];
    char digest[MDKR_SHA256_HEX_SIZE];
    size_t required;

    if (payload == NULL) {
        return MDKR_SAVE_CONTAINER_ERR_ARGUMENT;
    }
    if (metadata == NULL) {
        memset(&empty_metadata, 0, sizeof(empty_metadata));
        metadata = &empty_metadata;
    }
    if (!bounded_utf8_string(metadata->created_at,
                             sizeof(metadata->created_at)) ||
        !bounded_utf8_string(metadata->app_version,
                             sizeof(metadata->app_version)) ||
        !bounded_utf8_string(metadata->source, sizeof(metadata->source))) {
        return MDKR_SAVE_CONTAINER_ERR_FORMAT;
    }
    if (memchr(metadata->created_at, '\0', sizeof(metadata->created_at)) ==
            NULL ||
        memchr(metadata->app_version, '\0', sizeof(metadata->app_version)) ==
            NULL ||
        memchr(metadata->source, '\0', sizeof(metadata->source)) == NULL) {
        return MDKR_SAVE_CONTAINER_ERR_ARGUMENT;
    }
    base64_encode_512(payload, encoded);
    mdkr_sha256_hex(payload, MDKR_SAVE_IMAGE_SIZE, digest);
    writer.output = NULL;
    writer.capacity = 0;
    writer.length = 0;
    render_container(&writer, encoded, digest, metadata);
    required = writer.length + 1;
    if (output_size != NULL) *output_size = writer.length;
    if (required_capacity != NULL) *required_capacity = required;
    if (output == NULL) {
        return MDKR_SAVE_CONTAINER_OK;
    }
    if (output_capacity < required) {
        return MDKR_SAVE_CONTAINER_ERR_CAPACITY;
    }
    writer.output = output;
    writer.capacity = output_capacity;
    writer.length = 0;
    render_container(&writer, encoded, digest, metadata);
    output[writer.length] = '\0';
    return MDKR_SAVE_CONTAINER_OK;
}

const char *mdkr_save_container_result_string(MdkrSaveContainerResult result) {
    switch (result) {
        case MDKR_SAVE_CONTAINER_OK: return "ok";
        case MDKR_SAVE_CONTAINER_ERR_ARGUMENT: return "invalid argument";
        case MDKR_SAVE_CONTAINER_ERR_SIZE: return "unsupported file size";
        case MDKR_SAVE_CONTAINER_ERR_FORMAT: return "malformed container";
        case MDKR_SAVE_CONTAINER_ERR_VERSION:
            return "unsupported container version or payload format";
        case MDKR_SAVE_CONTAINER_ERR_PAYLOAD: return "invalid payload";
        case MDKR_SAVE_CONTAINER_ERR_DIGEST: return "payload digest mismatch";
        case MDKR_SAVE_CONTAINER_ERR_CAPACITY: return "output buffer too small";
        default: return "unknown container error";
    }
}
