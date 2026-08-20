/*
 * MdkrLanPartyRoom: the in-process local room. It reproduces the semantics of
 * services/party/src/party-room.ts + room-model.ts natively -- capability and
 * fallback-code minting, seat leases, invite TTL + rotation, the pairing
 * signal relay, and the Task 6 host_command_result identity echo -- with no
 * server, no network and no libdatachannel dependency. The one departure the
 * transport forces is faithful, not new: redemption travels the SAME
 * WebSocket the phone already opened (there is no separate HTTP redeem hop on
 * a LAN), so this file is where the authentication boundary lives -- an
 * upgraded socket is anonymous and does nothing until it redeems.
 */
#include "party/lan_party_room.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <utility>
#include <vector>

#if !(defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
      defined(_WIN32))
#include <sys/random.h> /* getentropy */
#endif

namespace {

/* ---- Secure randomness default ----------------------------------------- */

void defaultRandomBytes(uint8_t *out, size_t length) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    arc4random_buf(out, length);
#else
    size_t filled = 0u;
#if !defined(_WIN32)
    while (filled < length) {
        const size_t chunk = std::min<size_t>(256u, length - filled);
        if (getentropy(out + filled, chunk) != 0) break;
        filled += chunk;
    }
    if (filled < length) {
        if (std::FILE *file = std::fopen("/dev/urandom", "rb")) {
            filled += std::fread(out + filled, 1u, length - filled, file);
            std::fclose(file);
        }
    }
#endif
    if (filled < length) {
        std::random_device device;
        for (; filled < length; filled++) {
            out[filled] = static_cast<uint8_t>(device() & 0xffu);
        }
    }
#endif
}

/* ---- base64url, constant time, codes ----------------------------------- */

std::string base64Url(const uint8_t *bytes, size_t length) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string result;
    result.reserve((length * 4u + 2u) / 3u);
    uint32_t accumulator = 0u;
    unsigned bits = 0u;
    for (size_t index = 0u; index < length; index++) {
        accumulator = (accumulator << 8u) | bytes[index];
        bits += 8u;
        while (bits >= 6u) {
            bits -= 6u;
            result.push_back(kAlphabet[(accumulator >> bits) & 63u]);
        }
    }
    if (bits != 0u) {
        result.push_back(kAlphabet[(accumulator << (6u - bits)) & 63u]);
    }
    return result;
}

/* Rejects strings of unequal length in constant time relative to the shorter
 * comparison; mirrors security.ts constantTimeEqual. */
bool constantTimeEqual(const std::string &left, const std::string &right) {
    if (left.size() != right.size() || left.size() > 256u) return false;
    unsigned difference = 0u;
    for (size_t index = 0u; index < left.size(); index++) {
        difference |= static_cast<unsigned>(
            static_cast<unsigned char>(left[index]) ^
            static_cast<unsigned char>(right[index]));
    }
    return difference == 0u;
}

bool isBase64Url(const std::string &value, size_t length) {
    if (value.size() != length) return false;
    for (char byte : value) {
        const bool ok = (byte >= 'A' && byte <= 'Z') ||
                        (byte >= 'a' && byte <= 'z') ||
                        (byte >= '0' && byte <= '9') || byte == '-' || byte == '_';
        if (!ok) return false;
    }
    return true;
}

bool isSixDigits(const std::string &value) {
    if (value.size() != 6u) return false;
    for (char byte : value) {
        if (byte < '0' || byte > '9') return false;
    }
    return true;
}

/* ---- Tiny strict JSON reader ------------------------------------------- */

namespace json {

struct Value {
    enum class Type { Null, Bool, Int, Double, Str, Arr, Obj };
    Type type = Type::Null;
    bool boolean = false;
    long long integer = 0;
    double number = 0.0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    bool isObject() const { return type == Type::Obj; }
    bool isString() const { return type == Type::Str; }
    bool isInt() const { return type == Type::Int; }
    const Value *find(const std::string &key) const {
        for (const auto &entry : obj) {
            if (entry.first == key) return &entry.second;
        }
        return nullptr;
    }
};

class Reader {
public:
    bool parse(const std::string &text, Value &out) {
        cursor_ = text.data();
        end_ = text.data() + text.size();
        depth_ = 0;
        skipSpace();
        if (!readValue(out)) return false;
        skipSpace();
        return cursor_ == end_;
    }

private:
    const char *cursor_ = nullptr;
    const char *end_ = nullptr;
    int depth_ = 0;

    void skipSpace() {
        while (cursor_ < end_ && (*cursor_ == ' ' || *cursor_ == '\t' ||
                                  *cursor_ == '\n' || *cursor_ == '\r')) {
            cursor_++;
        }
    }

    bool literal(const char *text) {
        const size_t length = std::char_traits<char>::length(text);
        if (static_cast<size_t>(end_ - cursor_) < length) return false;
        if (std::char_traits<char>::compare(cursor_, text, length) != 0) return false;
        cursor_ += length;
        return true;
    }

    bool readValue(Value &out) {
        if (depth_ > 16) return false;
        skipSpace();
        if (cursor_ >= end_) return false;
        const char lead = *cursor_;
        if (lead == '{') return readObject(out);
        if (lead == '[') return readArray(out);
        if (lead == '"') {
            out.type = Value::Type::Str;
            return readString(out.str);
        }
        if (lead == 't') {
            if (!literal("true")) return false;
            out.type = Value::Type::Bool;
            out.boolean = true;
            return true;
        }
        if (lead == 'f') {
            if (!literal("false")) return false;
            out.type = Value::Type::Bool;
            out.boolean = false;
            return true;
        }
        if (lead == 'n') {
            if (!literal("null")) return false;
            out.type = Value::Type::Null;
            return true;
        }
        return readNumber(out);
    }

    bool readObject(Value &out) {
        depth_++;
        cursor_++; /* '{' */
        out.type = Value::Type::Obj;
        skipSpace();
        if (cursor_ < end_ && *cursor_ == '}') {
            cursor_++;
            depth_--;
            return true;
        }
        for (;;) {
            skipSpace();
            if (cursor_ >= end_ || *cursor_ != '"') return false;
            std::string key;
            if (!readString(key)) return false;
            for (const auto &entry : out.obj) {
                if (entry.first == key) return false; /* no duplicate keys */
            }
            skipSpace();
            if (cursor_ >= end_ || *cursor_ != ':') return false;
            cursor_++;
            Value child;
            if (!readValue(child)) return false;
            out.obj.emplace_back(std::move(key), std::move(child));
            skipSpace();
            if (cursor_ >= end_) return false;
            if (*cursor_ == ',') {
                cursor_++;
                continue;
            }
            if (*cursor_ == '}') {
                cursor_++;
                depth_--;
                return true;
            }
            return false;
        }
    }

    bool readArray(Value &out) {
        depth_++;
        cursor_++; /* '[' */
        out.type = Value::Type::Arr;
        skipSpace();
        if (cursor_ < end_ && *cursor_ == ']') {
            cursor_++;
            depth_--;
            return true;
        }
        for (;;) {
            Value child;
            if (!readValue(child)) return false;
            out.arr.push_back(std::move(child));
            skipSpace();
            if (cursor_ >= end_) return false;
            if (*cursor_ == ',') {
                cursor_++;
                continue;
            }
            if (*cursor_ == ']') {
                cursor_++;
                depth_--;
                return true;
            }
            return false;
        }
    }

    bool readString(std::string &out) {
        if (cursor_ >= end_ || *cursor_ != '"') return false;
        cursor_++;
        out.clear();
        while (cursor_ < end_) {
            const unsigned char byte = static_cast<unsigned char>(*cursor_++);
            if (byte == '"') return true;
            if (byte == '\\') {
                if (cursor_ >= end_) return false;
                const char escape = *cursor_++;
                switch (escape) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        unsigned code = 0u;
                        if (!readHex4(code)) return false;
                        if (code >= 0xd800u && code <= 0xdbffu) {
                            if (cursor_ + 1 >= end_ || cursor_[0] != '\\' ||
                                cursor_[1] != 'u') {
                                return false;
                            }
                            cursor_ += 2;
                            unsigned low = 0u;
                            if (!readHex4(low) || low < 0xdc00u || low > 0xdfffu) {
                                return false;
                            }
                            code = 0x10000u + ((code - 0xd800u) << 10u) +
                                   (low - 0xdc00u);
                        } else if (code >= 0xdc00u && code <= 0xdfffu) {
                            return false; /* lone low surrogate */
                        }
                        appendUtf8(out, code);
                        break;
                    }
                    default:
                        return false;
                }
                continue;
            }
            if (byte < 0x20u) return false; /* control chars must be escaped */
            out.push_back(static_cast<char>(byte));
        }
        return false;
    }

    bool readHex4(unsigned &out) {
        if (end_ - cursor_ < 4) return false;
        out = 0u;
        for (int index = 0; index < 4; index++) {
            const char digit = *cursor_++;
            out <<= 4u;
            if (digit >= '0' && digit <= '9') out |= static_cast<unsigned>(digit - '0');
            else if (digit >= 'a' && digit <= 'f') out |= static_cast<unsigned>(digit - 'a' + 10);
            else if (digit >= 'A' && digit <= 'F') out |= static_cast<unsigned>(digit - 'A' + 10);
            else return false;
        }
        return true;
    }

    static void appendUtf8(std::string &out, unsigned code) {
        if (code <= 0x7fu) {
            out.push_back(static_cast<char>(code));
        } else if (code <= 0x7ffu) {
            out.push_back(static_cast<char>(0xc0u | (code >> 6u)));
            out.push_back(static_cast<char>(0x80u | (code & 0x3fu)));
        } else if (code <= 0xffffu) {
            out.push_back(static_cast<char>(0xe0u | (code >> 12u)));
            out.push_back(static_cast<char>(0x80u | ((code >> 6u) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | (code & 0x3fu)));
        } else {
            out.push_back(static_cast<char>(0xf0u | (code >> 18u)));
            out.push_back(static_cast<char>(0x80u | ((code >> 12u) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | ((code >> 6u) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | (code & 0x3fu)));
        }
    }

    bool readNumber(Value &out) {
        const char *start = cursor_;
        bool isDouble = false;
        if (cursor_ < end_ && *cursor_ == '-') cursor_++;
        while (cursor_ < end_) {
            const char byte = *cursor_;
            if (byte >= '0' && byte <= '9') {
                cursor_++;
            } else if (byte == '.' || byte == 'e' || byte == 'E' || byte == '+' ||
                       byte == '-') {
                isDouble = true;
                cursor_++;
            } else {
                break;
            }
        }
        if (cursor_ == start) return false;
        const std::string token(start, static_cast<size_t>(cursor_ - start));
        if (isDouble) {
            out.type = Value::Type::Double;
            out.number = std::strtod(token.c_str(), nullptr);
            return true;
        }
        errno = 0;
        char *parseEnd = nullptr;
        const long long parsed = std::strtoll(token.c_str(), &parseEnd, 10);
        if (parseEnd != token.c_str() + token.size() || errno != 0) {
            out.type = Value::Type::Double;
            out.number = std::strtod(token.c_str(), nullptr);
            return true;
        }
        out.type = Value::Type::Int;
        out.integer = parsed;
        return true;
    }
};

std::string escape(const std::string &value) {
    std::string out;
    out.reserve(value.size() + 2u);
    for (unsigned char byte : value) {
        switch (byte) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (byte < 0x20u) {
                    char code[8];
                    std::snprintf(code, sizeof(code), "\\u%04x", byte);
                    out += code;
                } else {
                    out.push_back(static_cast<char>(byte));
                }
        }
    }
    return out;
}

/* Re-serialize a validated sub-object (sdp / candidate) verbatim. Only ever
 * called on values whose keys were already whitelisted, so nothing unknown
 * can survive the round trip to the other peer. */
std::string serialize(const Value &value) {
    switch (value.type) {
        case Value::Type::Null: return "null";
        case Value::Type::Bool: return value.boolean ? "true" : "false";
        case Value::Type::Int: return std::to_string(value.integer);
        case Value::Type::Double: {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%g", value.number);
            return buffer;
        }
        case Value::Type::Str: return "\"" + escape(value.str) + "\"";
        case Value::Type::Arr: {
            std::string out = "[";
            for (size_t index = 0u; index < value.arr.size(); index++) {
                if (index != 0u) out += ",";
                out += serialize(value.arr[index]);
            }
            return out + "]";
        }
        case Value::Type::Obj: {
            std::string out = "{";
            for (size_t index = 0u; index < value.obj.size(); index++) {
                if (index != 0u) out += ",";
                out += "\"" + escape(value.obj[index].first) + "\":" +
                       serialize(value.obj[index].second);
            }
            return out + "}";
        }
    }
    return "null";
}

bool exactKeys(const Value &object, std::initializer_list<const char *> keys) {
    if (object.obj.size() != keys.size()) return false;
    for (const char *key : keys) {
        if (object.find(key) == nullptr) return false;
    }
    return true;
}

bool positiveU32(const Value *value) {
    return value != nullptr && value->isInt() && value->integer >= 1 &&
           value->integer <= 0xffffffffLL;
}

} // namespace json

/* Validate {type, sdp} exactly and confirm the description direction. */
const json::Value *normalizedDescription(const json::Value &value,
                                         const char *expectedType) {
    if (!value.isObject() || !json::exactKeys(value, {"sdp", "type"})) return nullptr;
    const json::Value *type = value.find("type");
    const json::Value *sdp = value.find("sdp");
    if (type == nullptr || !type->isString() || type->str != expectedType) return nullptr;
    if (sdp == nullptr || !sdp->isString() || sdp->str.empty() ||
        sdp->str.size() > 60u * 1024u) {
        return nullptr;
    }
    return &value;
}

/* Validate an ICE candidate: `candidate` required, the rest optional and
 * type-checked; no key outside the whitelist may appear. */
bool normalizedCandidate(const json::Value &value) {
    if (!value.isObject()) return false;
    bool sawCandidate = false;
    for (const auto &entry : value.obj) {
        const std::string &key = entry.first;
        const json::Value &field = entry.second;
        if (key == "candidate") {
            if (!field.isString() || field.str.empty() ||
                field.str.size() > 4u * 1024u) {
                return false;
            }
            sawCandidate = true;
        } else if (key == "sdpMid") {
            if (field.type != json::Value::Type::Null &&
                (!field.isString() || field.str.size() > 256u)) {
                return false;
            }
        } else if (key == "sdpMLineIndex") {
            if (field.type != json::Value::Type::Null &&
                (!field.isInt() || field.integer < 0 || field.integer > 0xffff)) {
                return false;
            }
        } else if (key == "usernameFragment") {
            if (field.type != json::Value::Type::Null &&
                (!field.isString() || field.str.size() > 256u)) {
                return false;
            }
        } else {
            return false; /* unknown field */
        }
    }
    return sawCandidate;
}

/* ---- Name normalization ------------------------------------------------ */

/* Lenient UTF-8 decode into code points; malformed lead/continuation bytes are
 * skipped rather than trusted, so no half-formed sequence reaches the host. */
std::vector<uint32_t> decodeUtf8(const std::string &value) {
    std::vector<uint32_t> out;
    const size_t size = value.size();
    for (size_t index = 0u; index < size;) {
        const unsigned char lead = static_cast<unsigned char>(value[index]);
        uint32_t code;
        size_t width;
        if (lead < 0x80u) { code = lead; width = 1u; }
        else if ((lead & 0xe0u) == 0xc0u) { code = lead & 0x1fu; width = 2u; }
        else if ((lead & 0xf0u) == 0xe0u) { code = lead & 0x0fu; width = 3u; }
        else if ((lead & 0xf8u) == 0xf0u) { code = lead & 0x07u; width = 4u; }
        else { index++; continue; }
        if (index + width > size) break;
        bool ok = true;
        for (size_t offset = 1u; offset < width; offset++) {
            const unsigned char cont = static_cast<unsigned char>(value[index + offset]);
            if ((cont & 0xc0u) != 0x80u) { ok = false; break; }
            code = (code << 6u) | (cont & 0x3fu);
        }
        if (!ok) { index++; continue; }
        index += width;
        out.push_back(code);
    }
    return out;
}

void encodeUtf8(std::string &out, uint32_t code) {
    if (code <= 0x7fu) {
        out.push_back(static_cast<char>(code));
    } else if (code <= 0x7ffu) {
        out.push_back(static_cast<char>(0xc0u | (code >> 6u)));
        out.push_back(static_cast<char>(0x80u | (code & 0x3fu)));
    } else if (code <= 0xffffu) {
        out.push_back(static_cast<char>(0xe0u | (code >> 12u)));
        out.push_back(static_cast<char>(0x80u | ((code >> 6u) & 0x3fu)));
        out.push_back(static_cast<char>(0x80u | (code & 0x3fu)));
    } else {
        out.push_back(static_cast<char>(0xf0u | (code >> 18u)));
        out.push_back(static_cast<char>(0x80u | ((code >> 12u) & 0x3fu)));
        out.push_back(static_cast<char>(0x80u | ((code >> 6u) & 0x3fu)));
        out.push_back(static_cast<char>(0x80u | (code & 0x3fu)));
    }
}

/* The code points security.ts normalizeName strips: C0 controls, DEL + C1,
 * zero-width and directional-formatting characters, bidi overrides/isolates,
 * word joiner and BOM. */
bool nameStripped(uint32_t code) {
    return code <= 0x1fu || (code >= 0x7fu && code <= 0x9fu) ||
           (code >= 0x200bu && code <= 0x200fu) || code == 0x2028u ||
           code == 0x2029u || (code >= 0x202au && code <= 0x202eu) ||
           code == 0x2060u || (code >= 0x2066u && code <= 0x2069u) ||
           code == 0xfeffu;
}

/* Unicode White_Space that can survive the strip above, for the .trim() step. */
bool nameWhitespace(uint32_t code) {
    return code == 0x20u || code == 0xa0u || code == 0x1680u ||
           (code >= 0x2000u && code <= 0x200au) || code == 0x202fu ||
           code == 0x205fu || code == 0x3000u;
}

/* Native port of security.ts normalizeName: strip the invisible/bidi code
 * points above, trim surrounding whitespace, and cap at the code-point budget.
 * Canonical NFC composition still needs ICU and stays out of scope (the name
 * is cosmetic, rendered by the native host); what this guarantees is that no
 * control, zero-width or direction-flipping code point reaches the host row. */
std::string normalizeName(const std::string &value) {
    std::vector<uint32_t> kept;
    for (uint32_t code : decodeUtf8(value)) {
        if (!nameStripped(code)) kept.push_back(code);
    }
    size_t begin = 0u;
    size_t finish = kept.size();
    while (begin < finish && nameWhitespace(kept[begin])) begin++;
    while (finish > begin && nameWhitespace(kept[finish - 1u])) finish--;
    std::string out;
    unsigned codePoints = 0u;
    for (size_t index = begin;
         index < finish && codePoints < kMdkrLanPartyMaxNameCodePoints;
         index++, codePoints++) {
        encodeUtf8(out, kept[index]);
    }
    return out;
}

} // namespace

/* ---- Room state -------------------------------------------------------- */

struct MdkrLanPartyController {
    std::string id;
    std::string name;
    std::string publicKey;
    std::string phase; /* pending | approved | leased | connected | closed */
    int seat = 0;      /* 0 == none */
    uint32_t leaseGeneration = 0u;
    uint32_t connectionSequence = 0u;
};

struct MdkrLanPartySocketCtx {
    std::shared_ptr<MdkrLanPartyControllerSocket> socket;
    bool authenticated = false;
    std::string controllerId;
    bool removed = false;
    /* Post-auth signal throttle, one per socket (worker admitSignalMessage). */
    unsigned windowMessages = 0u;
    uint64_t windowStartedAt = 0u;
    unsigned lifetimeMessages = 0u;
};

struct MdkrLanPartyRoomState {
    std::mutex mutex;
    MdkrLanPartyRoomConfig config;
    MdkrLanPartyRoom::HostMessage hostSink;

    bool opened = false;
    std::string hostPublicKey;
    std::string controllerOrigin;

    std::string roomId;
    std::string capability;
    std::string fallbackCode;
    unsigned inviteGeneration = 0u;
    uint64_t inviteExpiresAtMs = 0u;
    uint64_t roomExpiresAtMs = 0u;
    bool inviteActive = false;

    std::string phase = "open"; /* open | closed */
    std::string closedReason;
    uint64_t transitionId = 0u;
    uint32_t nextLeaseGeneration = 1u;
    std::vector<MdkrLanPartyController> controllers;

    std::vector<std::shared_ptr<MdkrLanPartySocketCtx>> sockets;
    std::deque<uint64_t> codeAttempts;
};

namespace {

/* One deferred outbound effect, flushed after the mutex is released so no I/O
 * ever runs under the lock and a broken peer cannot stall another delivery. */
struct Effect {
    enum class Kind { Send, Close } kind = Kind::Send;
    std::shared_ptr<MdkrLanPartyControllerSocket> socket;
    std::string text;
    uint16_t code = 0u;
};

void flush(std::vector<Effect> &effects) {
    for (Effect &effect : effects) {
        switch (effect.kind) {
            case Effect::Kind::Send:
                if (effect.socket) effect.socket->sendText(effect.text);
                break;
            case Effect::Kind::Close:
                if (effect.socket) effect.socket->close(effect.code);
                break;
        }
    }
}

uint64_t nowMs(MdkrLanPartyRoomState &state) {
    if (state.config.nowMs) return state.config.nowMs();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void fillRandom(MdkrLanPartyRoomState &state, uint8_t *out, size_t length) {
    if (state.config.randomBytes) {
        state.config.randomBytes(out, length);
    } else {
        defaultRandomBytes(out, length);
    }
}

std::string mintToken(MdkrLanPartyRoomState &state, size_t bytes) {
    std::vector<uint8_t> raw(bytes);
    fillRandom(state, raw.data(), raw.size());
    return base64Url(raw.data(), raw.size());
}

/* Six-digit code via the Task 7 rejection sampler: draws at or above the
 * largest multiple of 1e6 below 2^32 are rerolled so all million codes are
 * exactly equally likely (security.ts fallbackCode, bound 4_294_000_000). */
std::string mintCode(MdkrLanPartyRoomState &state) {
    constexpr uint32_t kBound = 4294000000u;
    uint32_t draw = 0u;
    do {
        uint8_t raw[4];
        fillRandom(state, raw, sizeof(raw));
        draw = (static_cast<uint32_t>(raw[0]) << 24u) |
               (static_cast<uint32_t>(raw[1]) << 16u) |
               (static_cast<uint32_t>(raw[2]) << 8u) |
               static_cast<uint32_t>(raw[3]);
    } while (draw >= kBound);
    char buffer[7];
    std::snprintf(buffer, sizeof(buffer), "%06u", draw % 1000000u);
    return std::string(buffer, 6u);
}

/* Mint the 32-byte capability (16 room bytes + 16 secret bytes) and the
 * six-digit code, mirroring the worker's createRoom / rotateInvite. On room
 * creation the roomId is the base64url of the room-byte prefix; rotation keeps
 * that displayed roomId and re-draws a full fresh capability (the room half is
 * cosmetic for a single in-process room -- matching is against the whole
 * capability string). */
void mintInvite(MdkrLanPartyRoomState &state, bool freshRoom, uint64_t now) {
    uint8_t capability[32];
    fillRandom(state, capability, sizeof(capability));
    if (freshRoom) state.roomId = base64Url(capability, 16u);
    state.capability = base64Url(capability, sizeof(capability));
    state.fallbackCode = mintCode(state);
    state.inviteExpiresAtMs =
        std::min(now + kMdkrLanPartyInviteTtlMs, state.roomExpiresAtMs);
    state.inviteActive = true;
}

std::string controllerUrl(MdkrLanPartyRoomState &state) {
    const std::string path = "/controller/#" + state.capability;
    return state.controllerOrigin.empty() ? path : state.controllerOrigin + path;
}

uint64_t advance(MdkrLanPartyRoomState &state) {
    state.transitionId++;
    if (state.transitionId > kMdkrLanPartyMaxTransitions) {
        state.phase = "closed";
        state.closedReason = "rate_limited";
    }
    return state.transitionId;
}

std::string seatJson(int seat) { return seat == 0 ? "null" : std::to_string(seat); }

std::string controllerStateJson(MdkrLanPartyRoomState &state,
                                const MdkrLanPartyController &controller) {
    return std::string("{\"type\":\"controller_state\",\"transitionId\":") +
           std::to_string(state.transitionId) + ",\"phase\":\"" + controller.phase +
           "\",\"seat\":" + seatJson(controller.seat) + ",\"leaseGeneration\":" +
           std::to_string(controller.leaseGeneration) + ",\"connectionSequence\":" +
           std::to_string(controller.connectionSequence) + "}";
}

std::string roomStateJson(MdkrLanPartyRoomState &state, bool withInvite) {
    std::string out = "{\"type\":\"room_state\",\"transitionId\":" +
                      std::to_string(state.transitionId) + ",\"inviteExpiresAt\":" +
                      std::to_string(state.inviteExpiresAtMs) +
                      ",\"inviteGeneration\":" + std::to_string(state.inviteGeneration) +
                      ",\"phase\":\"" + state.phase + "\",\"controllers\":[";
    bool first = true;
    for (const MdkrLanPartyController &controller : state.controllers) {
        if (controller.phase == "closed") continue;
        if (!first) out += ",";
        first = false;
        out += "{\"controllerId\":\"" + controller.id + "\",\"name\":\"" +
               json::escape(controller.name) + "\",\"controllerPublicKey\":\"" +
               controller.publicKey + "\",\"phase\":\"" + controller.phase +
               "\",\"seat\":" + seatJson(controller.seat) + ",\"leaseGeneration\":" +
               std::to_string(controller.leaseGeneration) +
               ",\"connectionSequence\":" +
               std::to_string(controller.connectionSequence) + "}";
    }
    out += "]";
    if (withInvite) {
        out += ",\"fallbackCode\":\"" + state.fallbackCode + "\",\"controllerUrl\":\"" +
               json::escape(controllerUrl(state)) + "\"";
    }
    return out + "}";
}

MdkrLanPartyController *findController(MdkrLanPartyRoomState &state,
                                      const std::string &id) {
    for (MdkrLanPartyController &controller : state.controllers) {
        if (controller.id == id) return &controller;
    }
    return nullptr;
}

std::shared_ptr<MdkrLanPartyControllerSocket> socketFor(
    MdkrLanPartyRoomState &state, const std::string &controllerId) {
    for (const auto &ctx : state.sockets) {
        if (!ctx->removed && ctx->authenticated && ctx->controllerId == controllerId &&
            ctx->socket && ctx->socket->isOpen()) {
            return ctx->socket;
        }
    }
    return nullptr;
}

void pushHost(MdkrLanPartyRoomState &state, std::vector<std::string> &hostOut,
              const std::string &message) {
    if (state.hostSink) hostOut.push_back(message);
}

void broadcastRoomState(MdkrLanPartyRoomState &state,
                        std::vector<std::string> &hostOut) {
    pushHost(state, hostOut, roomStateJson(state, false));
}

void sendControllerState(MdkrLanPartyRoomState &state, std::vector<Effect> &effects,
                         const MdkrLanPartyController &controller) {
    if (auto socket = socketFor(state, controller.id)) {
        effects.push_back({Effect::Kind::Send, socket,
                           controllerStateJson(state, controller), 0u});
    }
}

void closeControllerSocket(MdkrLanPartyRoomState &state, std::vector<Effect> &effects,
                           const std::string &controllerId, uint16_t code) {
    if (auto socket = socketFor(state, controllerId)) {
        effects.push_back({Effect::Kind::Close, socket, "", code});
    }
}

const char *terminalReason(MdkrLanPartyRoomState &state) {
    if (state.phase == "closed") {
        return state.closedReason.empty() ? "host_closed" : state.closedReason.c_str();
    }
    return nullptr;
}

void refuseRedeem(std::vector<Effect> &effects,
                  const std::shared_ptr<MdkrLanPartyControllerSocket> &socket,
                  const std::string &error) {
    effects.push_back({Effect::Kind::Send, socket,
                       "{\"type\":\"redeem_result\",\"ok\":false,\"error\":\"" +
                           error + "\"}",
                       0u});
    effects.push_back({Effect::Kind::Close, socket, "", 4001u});
}

/* ---- Redeem: the authentication boundary ------------------------------- */

void handleRedeem(MdkrLanPartyRoomState &state,
                  const std::shared_ptr<MdkrLanPartySocketCtx> &ctx,
                  const json::Value &value, uint64_t now,
                  std::vector<Effect> &effects, std::vector<std::string> &hostOut) {
    const auto &socket = ctx->socket;
    if (const char *terminal = terminalReason(state)) {
        refuseRedeem(effects, socket, terminal);
        return;
    }
    const json::Value *protocol = value.find("protocol");
    const json::Value *publicKey = value.find("controllerPublicKey");
    const json::Value *capability = value.find("capability");
    const json::Value *code = value.find("code");
    const json::Value *name = value.find("name");

    const bool byCapability = capability != nullptr;
    const bool byCode = code != nullptr;
    const bool hasName = name != nullptr;
    /* Exactly one of capability|code, alongside type/protocol/
     * controllerPublicKey and an optional name -- validated with the same
     * json::exactKeys helper every other handler uses. */
    bool keysOk = false;
    if (byCapability != byCode) {
        if (byCapability) {
            keysOk = hasName
                         ? json::exactKeys(value, {"capability", "controllerPublicKey",
                                                   "name", "protocol", "type"})
                         : json::exactKeys(value, {"capability", "controllerPublicKey",
                                                   "protocol", "type"});
        } else {
            keysOk = hasName
                         ? json::exactKeys(value, {"code", "controllerPublicKey",
                                                   "name", "protocol", "type"})
                         : json::exactKeys(value, {"code", "controllerPublicKey",
                                                   "protocol", "type"});
        }
    }
    if (!keysOk) {
        refuseRedeem(effects, socket, "invalid_invite");
        return;
    }
    if (protocol == nullptr || !protocol->isInt() || protocol->integer != 2) {
        refuseRedeem(effects, socket, "protocol_update_required");
        return;
    }
    if (publicKey == nullptr || !publicKey->isString() ||
        !isBase64Url(publicKey->str, 87u)) {
        refuseRedeem(effects, socket, "invalid_controller_key");
        return;
    }
    if (name != nullptr && !name->isString()) {
        refuseRedeem(effects, socket, "invalid_invite");
        return;
    }

    if (byCode) {
        if (!code->isString() || !isSixDigits(code->str)) {
            refuseRedeem(effects, socket, "invalid_code");
            return;
        }
        /* One shared code-guess bucket: prune the window, then admit or lock. */
        while (!state.codeAttempts.empty() &&
               now - state.codeAttempts.front() >= kMdkrLanPartyCodeWindowMs) {
            state.codeAttempts.pop_front();
        }
        if (state.codeAttempts.size() >= kMdkrLanPartyCodeAttemptsPerWindow) {
            refuseRedeem(effects, socket, "rate_limited");
            return;
        }
        state.codeAttempts.push_back(now);
    } else {
        if (!capability->isString() || !isBase64Url(capability->str, 43u)) {
            refuseRedeem(effects, socket, "invalid_invite");
            return;
        }
    }

    if (now >= state.inviteExpiresAtMs || !state.inviteActive) {
        refuseRedeem(effects, socket, "invite_expired");
        return;
    }

    const bool matches = byCapability
                             ? constantTimeEqual(capability->str, state.capability)
                             : constantTimeEqual(code->str, state.fallbackCode);
    if (!matches) {
        refuseRedeem(effects, socket, "invite_rotated");
        return;
    }

    unsigned pending = 0u;
    for (const MdkrLanPartyController &controller : state.controllers) {
        if (controller.phase == "pending") pending++;
    }
    if (pending >= kMdkrLanPartyMaxPending) {
        refuseRedeem(effects, socket, "pending_full");
        return;
    }

    std::string controllerId;
    do {
        controllerId = mintToken(state, 16u);
    } while (findController(state, controllerId) != nullptr);

    MdkrLanPartyController controller;
    controller.id = controllerId;
    controller.name = name != nullptr ? normalizeName(name->str) : "";
    controller.publicKey = publicKey->str;
    controller.phase = "pending";
    controller.seat = 0;
    controller.leaseGeneration = 0u;
    controller.connectionSequence = 1u;
    state.controllers.push_back(controller);
    ctx->authenticated = true;
    ctx->controllerId = controllerId;
    advance(state);

    effects.push_back({Effect::Kind::Send, socket,
                       "{\"type\":\"redeem_result\",\"ok\":true,\"controllerId\":\"" +
                           controllerId + "\",\"roomId\":\"" + state.roomId +
                           "\",\"protocol\":2,\"hostPublicKey\":\"" +
                           state.hostPublicKey + "\"}",
                       0u});
    effects.push_back({Effect::Kind::Send, socket,
                       controllerStateJson(state, state.controllers.back()), 0u});
    broadcastRoomState(state, hostOut);
}

/* ---- Controller signaling (post-auth) ---------------------------------- */

void handleControllerSignal(MdkrLanPartyRoomState &state,
                            const std::shared_ptr<MdkrLanPartySocketCtx> &ctx,
                            const json::Value &value, std::vector<Effect> &effects,
                            std::vector<std::string> &hostOut) {
    const json::Value *type = value.find("type");
    if (type == nullptr || !type->isString()) {
        effects.push_back({Effect::Kind::Close, ctx->socket, "", 4003u});
        return;
    }
    const std::string &realId = ctx->controllerId;
    /* Defense in depth + worker parity: the supplied controllerId must be a
     * well-formed base64url-22 even though the relayed id is always forced to
     * this socket's authenticated id (never the wire value). */
    const json::Value *suppliedId = value.find("controllerId");
    const bool idWellFormed = suppliedId != nullptr && suppliedId->isString() &&
                              isBase64Url(suppliedId->str, 22u);
    if (type->str == "controller_hello") {
        if (!json::exactKeys(value, {"controllerId", "type"}) || !idWellFormed) {
            effects.push_back({Effect::Kind::Close, ctx->socket, "", 4003u});
            return;
        }
        pushHost(state, hostOut,
                 "{\"type\":\"controller_hello\",\"controllerId\":\"" + realId + "\"}");
        return;
    }
    if (type->str == "webrtc_answer") {
        const json::Value *pg = value.find("peerGeneration");
        const json::Value *sdp = value.find("sdp");
        if (!json::exactKeys(value, {"controllerId", "peerGeneration", "sdp", "type"}) ||
            !idWellFormed || !json::positiveU32(pg) || sdp == nullptr ||
            normalizedDescription(*sdp, "answer") == nullptr) {
            effects.push_back({Effect::Kind::Close, ctx->socket, "", 4003u});
            return;
        }
        pushHost(state, hostOut,
                 "{\"type\":\"webrtc_answer\",\"controllerId\":\"" + realId +
                     "\",\"peerGeneration\":" + std::to_string(pg->integer) +
                     ",\"sdp\":" + json::serialize(*sdp) + "}");
        return;
    }
    if (type->str == "webrtc_ice") {
        const json::Value *pg = value.find("peerGeneration");
        const json::Value *candidate = value.find("candidate");
        if (!json::exactKeys(value,
                             {"candidate", "controllerId", "peerGeneration", "type"}) ||
            !idWellFormed || !json::positiveU32(pg) || candidate == nullptr ||
            !normalizedCandidate(*candidate)) {
            effects.push_back({Effect::Kind::Close, ctx->socket, "", 4003u});
            return;
        }
        pushHost(state, hostOut,
                 "{\"type\":\"webrtc_ice\",\"controllerId\":\"" + realId +
                     "\",\"peerGeneration\":" + std::to_string(pg->integer) +
                     ",\"candidate\":" + json::serialize(*candidate) + "}");
        return;
    }
    effects.push_back({Effect::Kind::Close, ctx->socket, "", 4003u});
}

/* Worker admitSignalMessage, verbatim: reset the fixed window on rollover,
 * count the frame against both the window and the lifetime, and admit only
 * while both stay under their caps. Called once per post-auth frame BEFORE the
 * parse, so a flood of malformed frames is throttled too. */
bool admitSignal(MdkrLanPartySocketCtx &ctx, uint64_t now) {
    if (now - ctx.windowStartedAt >= kMdkrLanPartySignalWindowMs) {
        ctx.windowMessages = 0u;
        ctx.windowStartedAt = now;
    }
    ctx.windowMessages += 1u;
    ctx.lifetimeMessages += 1u;
    return ctx.windowMessages <= kMdkrLanPartySignalWindowMessages &&
           ctx.lifetimeMessages <= kMdkrLanPartySignalLifetimeMessages;
}

void handleControllerMessage(MdkrLanPartyRoomState &state,
                             const std::shared_ptr<MdkrLanPartySocketCtx> &ctx,
                             const std::string &text, std::vector<Effect> &effects,
                             std::vector<std::string> &hostOut) {
    if (ctx->removed || !ctx->socket) return;
    if (text.size() > kMdkrLanPartyMaxSignalBytes) {
        effects.push_back({Effect::Kind::Close, ctx->socket, "", 4009u});
        return;
    }
    /* Redeem is a one-shot on an anonymous socket and is bounded by the code
     * throttle; the per-socket signal rate limit governs a REDEEMED socket's
     * frames, exactly as the worker applies admitSignalMessage only to
     * authenticated peers. A breach closes 4008, mirroring the worker. */
    if (ctx->authenticated && !admitSignal(*ctx, nowMs(state))) {
        effects.push_back({Effect::Kind::Close, ctx->socket, "", 4008u});
        return;
    }
    json::Value value;
    json::Reader reader;
    const bool parsed = reader.parse(text, value) && value.isObject();
    if (!ctx->authenticated) {
        /* THE boundary: only a valid redeem may authenticate. Anything else
         * from an anonymous socket -- unparseable, wrong type, a stray signal
         * -- closes it (4001) without touching room state. */
        const json::Value *type = parsed ? value.find("type") : nullptr;
        if (!parsed || type == nullptr || !type->isString() || type->str != "redeem") {
            effects.push_back({Effect::Kind::Close, ctx->socket, "", 4001u});
            return;
        }
        handleRedeem(state, ctx, value, nowMs(state), effects, hostOut);
        return;
    }
    if (!parsed) {
        effects.push_back({Effect::Kind::Close, ctx->socket, "", 4003u});
        return;
    }
    handleControllerSignal(state, ctx, value, effects, hostOut);
}

/* ---- Host commands ----------------------------------------------------- */

std::string commandIdentity(const json::Value &value) {
    static const char *kActions[] = {"approve", "reject", "remove",
                                     "rotate",  "revoke", "close"};
    std::string identity;
    const json::Value *action = value.find("action");
    if (action != nullptr && action->isString()) {
        for (const char *known : kActions) {
            if (action->str == known) {
                identity += ",\"command\":\"" + action->str + "\"";
                break;
            }
        }
    }
    const json::Value *controllerId = value.find("controllerId");
    if (controllerId != nullptr && controllerId->isString() &&
        isBase64Url(controllerId->str, 22u)) {
        identity += ",\"controllerId\":\"" + controllerId->str + "\"";
    }
    return identity;
}

void commandError(MdkrLanPartyRoomState &state, std::vector<std::string> &hostOut,
                  const std::string &error, const std::string &identity) {
    pushHost(state, hostOut,
             "{\"type\":\"host_command_result\",\"ok\":false,\"error\":\"" + error +
                 "\"" + identity + "}");
}

bool validHostCommandShape(const json::Value &value) {
    const json::Value *action = value.find("action");
    if (action == nullptr || !action->isString()) return false;
    const json::Value *controllerId = value.find("controllerId");
    if (action->str == "approve") {
        const json::Value *seat = value.find("seat");
        return json::exactKeys(value, {"action", "controllerId", "seat", "type"}) &&
               controllerId != nullptr && controllerId->isString() &&
               isBase64Url(controllerId->str, 22u) && seat != nullptr &&
               seat->isInt() && seat->integer >= 1 &&
               seat->integer <= static_cast<long long>(kMdkrLanPartyMaxSeats);
    }
    if (action->str == "reject" || action->str == "remove") {
        return json::exactKeys(value, {"action", "controllerId", "type"}) &&
               controllerId != nullptr && controllerId->isString() &&
               isBase64Url(controllerId->str, 22u);
    }
    if (action->str == "rotate") {
        return json::exactKeys(value, {"action", "expectedInviteGeneration", "type"}) &&
               json::positiveU32(value.find("expectedInviteGeneration"));
    }
    return (action->str == "revoke" || action->str == "close") &&
           json::exactKeys(value, {"action", "type"});
}

void applyHostCommand(MdkrLanPartyRoomState &state, const json::Value &value,
                      uint64_t now, std::vector<Effect> &effects,
                      std::vector<std::string> &hostOut) {
    const std::string identity = commandIdentity(value);
    if (state.phase == "closed") {
        commandError(state, hostOut, "not_found", identity);
        return;
    }
    if (!validHostCommandShape(value)) {
        commandError(state, hostOut, "invalid_command", identity);
        return;
    }
    const std::string action = value.find("action")->str;

    if (action == "approve" || action == "reject" || action == "remove") {
        const std::string controllerId = value.find("controllerId")->str;
        MdkrLanPartyController *controller = findController(state, controllerId);
        if (action == "approve") {
            if (controller == nullptr || controller->phase != "pending") {
                commandError(state, hostOut, "not_found", identity);
                return;
            }
            const int seat = static_cast<int>(value.find("seat")->integer);
            for (const MdkrLanPartyController &other : state.controllers) {
                if (other.phase != "closed" && other.seat == seat) {
                    commandError(state, hostOut, "room_full", identity);
                    return;
                }
            }
            controller->phase = "leased";
            controller->seat = seat;
            controller->leaseGeneration = state.nextLeaseGeneration++;
            advance(state);
            broadcastRoomState(state, hostOut);
            sendControllerState(state, effects, *controller);
            return;
        }
        if (action == "reject") {
            if (controller == nullptr || controller->phase != "pending") {
                commandError(state, hostOut, "not_found", identity);
                return;
            }
            controller->phase = "closed";
            advance(state);
            broadcastRoomState(state, hostOut);
            sendControllerState(state, effects, *controller);
            closeControllerSocket(state, effects, controllerId, 4000u);
            return;
        }
        /* remove */
        if (controller == nullptr || controller->phase == "pending" ||
            controller->phase == "closed") {
            commandError(state, hostOut, "not_found", identity);
            return;
        }
        controller->phase = "closed";
        controller->seat = 0;
        advance(state);
        broadcastRoomState(state, hostOut);
        sendControllerState(state, effects, *controller);
        closeControllerSocket(state, effects, controllerId, 4000u);
        return;
    }

    if (action == "rotate") {
        const long long expected = value.find("expectedInviteGeneration")->integer;
        if (expected != static_cast<long long>(state.inviteGeneration)) {
            commandError(state, hostOut, "invalid_state", identity);
            return;
        }
        mintInvite(state, false, now);
        state.inviteGeneration++;
        advance(state);
        pushHost(state, hostOut, roomStateJson(state, true));
        return;
    }

    if (action == "revoke") {
        state.capability.clear();
        state.fallbackCode.clear();
        state.inviteActive = false;
        state.inviteGeneration++;
        state.inviteExpiresAtMs = now;
        advance(state);
        broadcastRoomState(state, hostOut);
        return;
    }

    /* close */
    state.phase = "closed";
    state.closedReason = "host_closed";
    for (MdkrLanPartyController &controller : state.controllers) {
        controller.phase = "closed";
    }
    advance(state);
    for (const auto &ctx : state.sockets) {
        if (!ctx->removed && ctx->socket) {
            effects.push_back({Effect::Kind::Close, ctx->socket, "", 4000u});
        }
    }
    pushHost(state, hostOut, "{\"type\":\"host_closed\",\"reason\":\"host_closed\"}");
}

void handleHostMessage(MdkrLanPartyRoomState &state, const json::Value &value,
                       std::vector<Effect> &effects,
                       std::vector<std::string> &hostOut) {
    const json::Value *type = value.find("type");
    if (type == nullptr || !type->isString()) return;
    if (type->str == "host_command") {
        applyHostCommand(state, value, nowMs(state), effects, hostOut);
        return;
    }
    if (type->str == "webrtc_offer") {
        const json::Value *to = value.find("to");
        const json::Value *pg = value.find("peerGeneration");
        const json::Value *sdp = value.find("sdp");
        if (!json::exactKeys(value, {"peerGeneration", "sdp", "to", "type"}) ||
            to == nullptr || !to->isString() || !isBase64Url(to->str, 22u) ||
            !json::positiveU32(pg) || sdp == nullptr ||
            normalizedDescription(*sdp, "offer") == nullptr) {
            return;
        }
        if (auto socket = socketFor(state, to->str)) {
            effects.push_back({Effect::Kind::Send, socket,
                               "{\"type\":\"webrtc_offer\",\"to\":\"" + to->str +
                                   "\",\"peerGeneration\":" +
                                   std::to_string(pg->integer) + ",\"sdp\":" +
                                   json::serialize(*sdp) + "}",
                               0u});
        }
        return;
    }
    if (type->str == "webrtc_ice") {
        const json::Value *to = value.find("to");
        const json::Value *pg = value.find("peerGeneration");
        const json::Value *candidate = value.find("candidate");
        if (!json::exactKeys(value, {"candidate", "peerGeneration", "to", "type"}) ||
            to == nullptr || !to->isString() || !isBase64Url(to->str, 22u) ||
            !json::positiveU32(pg) || candidate == nullptr ||
            !normalizedCandidate(*candidate)) {
            return;
        }
        if (auto socket = socketFor(state, to->str)) {
            effects.push_back({Effect::Kind::Send, socket,
                               "{\"type\":\"webrtc_ice\",\"to\":\"" + to->str +
                                   "\",\"peerGeneration\":" +
                                   std::to_string(pg->integer) + ",\"candidate\":" +
                                   json::serialize(*candidate) + "}",
                               0u});
        }
    }
}

void flushHost(MdkrLanPartyRoomState &state, std::vector<std::string> &hostOut) {
    if (!state.hostSink) return;
    for (const std::string &message : hostOut) state.hostSink(message);
}

} // namespace

/* ---- Public surface ---------------------------------------------------- */

MdkrLanPartyRoom::MdkrLanPartyRoom(MdkrLanPartyRoomConfig config)
    : state_(std::make_shared<MdkrLanPartyRoomState>()) {
    state_->config = std::move(config);
}

MdkrLanPartyRoom::~MdkrLanPartyRoom() = default;

MdkrLanPartyInvite MdkrLanPartyRoom::open(const std::string &hostPublicKey,
                                          const std::string &controllerOrigin,
                                          HostMessage onHostMessage) {
    std::vector<std::string> hostOut;
    MdkrLanPartyInvite invite;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->opened && state_->phase == "open") {
            invite.roomId = state_->roomId;
            invite.capability = state_->capability;
            invite.controllerPath = "/controller/#" + state_->capability;
            invite.controllerUrl = controllerUrl(*state_);
            invite.fallbackCode = state_->fallbackCode;
            invite.hostPublicKey = state_->hostPublicKey;
            invite.inviteExpiresAtMs = state_->inviteExpiresAtMs;
            invite.inviteGeneration = state_->inviteGeneration;
            invite.active = state_->inviteActive;
            return invite;
        }
        const uint64_t now = nowMs(*state_);
        state_->opened = true;
        state_->hostPublicKey = hostPublicKey;
        state_->controllerOrigin = controllerOrigin;
        state_->hostSink = std::move(onHostMessage);
        state_->phase = "open";
        state_->closedReason.clear();
        state_->transitionId = 1u;
        state_->nextLeaseGeneration = 1u;
        state_->inviteGeneration = 1u;
        state_->roomExpiresAtMs = now + kMdkrLanPartyRoomTtlMs;
        mintInvite(*state_, true, now);

        invite.roomId = state_->roomId;
        invite.capability = state_->capability;
        invite.controllerPath = "/controller/#" + state_->capability;
        invite.controllerUrl = controllerUrl(*state_);
        invite.fallbackCode = state_->fallbackCode;
        invite.hostPublicKey = state_->hostPublicKey;
        invite.inviteExpiresAtMs = state_->inviteExpiresAtMs;
        invite.inviteGeneration = state_->inviteGeneration;
        invite.active = state_->inviteActive;

        pushHost(*state_, hostOut, roomStateJson(*state_, false));
    }
    flushHost(*state_, hostOut);
    return invite;
}

void MdkrLanPartyRoom::attachController(
    std::shared_ptr<MdkrLanPartyControllerSocket> socket) {
    if (!socket) return;
    auto ctx = std::make_shared<MdkrLanPartySocketCtx>();
    ctx->socket = socket;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->sockets.push_back(ctx);
    }
    /* The socket owns these callbacks and the callbacks reach the socket
     * through ctx (ctx->socket). Capturing ctx weakly breaks that cycle -- the
     * room's `sockets` vector is the one strong owner of ctx -- so a socket is
     * never kept alive by its own callbacks and is never destroyed from inside
     * one of them. */
    std::weak_ptr<MdkrLanPartyRoomState> weakState = state_;
    std::weak_ptr<MdkrLanPartySocketCtx> weakCtx = ctx;
    socket->onMessage([weakState, weakCtx](const std::string &payload) {
        auto state = weakState.lock();
        auto liveCtx = weakCtx.lock();
        if (!state || !liveCtx) return;
        std::vector<Effect> effects;
        std::vector<std::string> hostOut;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            handleControllerMessage(*state, liveCtx, payload, effects, hostOut);
        }
        flush(effects);
        flushHost(*state, hostOut);
    });
    socket->onClosed([weakState, weakCtx]() {
        auto state = weakState.lock();
        auto liveCtx = weakCtx.lock();
        if (!state || !liveCtx) return;
        std::lock_guard<std::mutex> lock(state->mutex);
        liveCtx->removed = true;
        auto &sockets = state->sockets;
        sockets.erase(std::remove(sockets.begin(), sockets.end(), liveCtx),
                      sockets.end());
    });
}

void MdkrLanPartyRoom::deliverFromHost(const std::string &jsonText) {
    std::vector<Effect> effects;
    std::vector<std::string> hostOut;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        json::Value value;
        json::Reader reader;
        if (reader.parse(jsonText, value) && value.isObject()) {
            handleHostMessage(*state_, value, effects, hostOut);
        }
    }
    flush(effects);
    flushHost(*state_, hostOut);
}

void MdkrLanPartyRoom::close() {
    std::vector<Effect> effects;
    std::vector<std::string> hostOut;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->phase == "closed") return;
        state_->phase = "closed";
        state_->closedReason = "host_closed";
        for (MdkrLanPartyController &controller : state_->controllers) {
            controller.phase = "closed";
        }
        advance(*state_);
        for (const auto &ctx : state_->sockets) {
            if (!ctx->removed && ctx->socket) {
                effects.push_back({Effect::Kind::Close, ctx->socket, "", 4000u});
            }
        }
        pushHost(*state_, hostOut, "{\"type\":\"host_closed\",\"reason\":\"host_closed\"}");
    }
    flush(effects);
    flushHost(*state_, hostOut);
}

MdkrLanPartyInvite MdkrLanPartyRoom::invite() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    MdkrLanPartyInvite invite;
    invite.roomId = state_->roomId;
    invite.capability = state_->capability;
    invite.controllerPath = "/controller/#" + state_->capability;
    invite.controllerUrl = controllerUrl(*state_);
    invite.fallbackCode = state_->fallbackCode;
    invite.hostPublicKey = state_->hostPublicKey;
    invite.inviteExpiresAtMs = state_->inviteExpiresAtMs;
    invite.inviteGeneration = state_->inviteGeneration;
    invite.active = state_->inviteActive;
    return invite;
}
