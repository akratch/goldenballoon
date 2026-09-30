#ifndef MDKR_BOUNDED_JSON_H
#define MDKR_BOUNDED_JSON_H
/* Shared allocation-free JSON reader. Caller bounds input bytes; skipped
 * containers have a maximum nesting depth of 16. Schema strings reject NUL.
 * Extracted unchanged from the save container's tested parser. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct JsonParser {
    const uint8_t *cursor;
    const uint8_t *end;
    unsigned depth;
} JsonParser;

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
#endif
