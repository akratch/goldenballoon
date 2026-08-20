#include "lan_party_server.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

#ifdef _WIN32
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
constexpr SocketHandle kInvalidSocket = -1;
#endif

/*
 * Bounds. The request-line bound exists so an attacker cannot feed this
 * server an unbounded first line; past it the connection is CUT, with no
 * status line that could serve as a parser oracle. The header bound caps
 * one whole request head; the per-connection request cap and the idle
 * deadline bound how long a keep-alive connection may hold its thread.
 */
constexpr size_t kMaxRequestLineBytes = 2048u;
constexpr size_t kMaxRequestHeaderBytes = 8192u;
constexpr size_t kMaxRequestsPerConnection = 128u;
constexpr size_t kMaxConnections = 32u;
constexpr unsigned kRecvPollMs = 250u;
constexpr unsigned kSendTimeoutMs = 5000u;
constexpr unsigned kAcceptPollMs = 200u;
constexpr unsigned kHttpIdleDeadlineMs = 15000u;
/* After sending a close frame, how long the reader lingers to absorb the
 * peer's in-flight bytes so the close is delivered on a FIN, not lost to a
 * reset from closing with unread data queued. */
constexpr unsigned kCloseDrainMs = 500u;

const char kWsPath[] = "/party-ws";
/* RFC 6455 section 4.2.2: fixed handshake GUID, not a secret. */
const char kWsGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

struct ResponseHeader {
    const char *name;
    const char *value;
};

/*
 * THE header table: every HTTP response this server writes (200 and
 * refusal alike) carries exactly these, so there is no path that forgets
 * them. Reference: dist/web/_headers, its "/controller/" scope -- the cloud
 * deploy of this same controller page. Deltas for the plain-HTTP LAN
 * transport, each deliberate:
 *   - connect-src gains ws:  -- the phone's control channel is same-origin
 *     plain ws on this very port; engines have not uniformly extended
 *     'self' to ws://, and the page's own code only ever dials its origin.
 *   - manifest-src 'self' (was 'none'): this server serves the packaged
 *     webmanifest from its own asset manifest.
 *   - upgrade-insecure-requests is dropped: on an http: origin it rewrites
 *     every same-origin subresource to https:, which this server never
 *     speaks -- certificates are off the product path by owner decision.
 *   - Strict-Transport-Security is dropped: RFC 6797 ignores it over
 *     insecure transport, and an HSTS entry cached for a bare LAN host
 *     would lock the browser out of the only scheme served here.
 *   - Cache-Control: no-store added so a rebuilt game never fights a
 *     phone's stale copy of the controller page.
 */
constexpr ResponseHeader kResponseHeaders[] = {
    {"Content-Security-Policy",
     "default-src 'self'; base-uri 'none'; object-src 'none'; "
     "frame-ancestors 'none'; form-action 'self'; script-src 'self'; "
     "style-src 'self'; img-src 'self'; connect-src 'self' ws:; "
     "font-src 'none'; media-src 'none'; worker-src 'none'; "
     "manifest-src 'self'"},
    {"Referrer-Policy", "no-referrer"},
    {"X-Content-Type-Options", "nosniff"},
    {"X-Frame-Options", "DENY"},
    {"Cross-Origin-Opener-Policy", "same-origin"},
    {"Cross-Origin-Resource-Policy", "same-origin"},
    {"Permissions-Policy",
     "camera=(), microphone=(), geolocation=(), payment=(), usb=(), "
     "serial=(), bluetooth=(), accelerometer=(), gyroscope=(), "
     "magnetometer=()"},
    {"Cache-Control", "no-store"},
};

/* ---- Socket shims -------------------------------------------------------- */

void ensureSocketsInitialized() {
#ifdef _WIN32
    static const int initialized = []() {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data);
    }();
    (void)initialized;
#endif
}

void closeSocket(SocketHandle fd) {
#ifdef _WIN32
    ::closesocket(fd);
#else
    ::close(fd);
#endif
}

void shutdownBoth(SocketHandle fd) {
#ifdef _WIN32
    ::shutdown(fd, SD_BOTH);
#else
    ::shutdown(fd, SHUT_RDWR);
#endif
}

void shutdownWrite(SocketHandle fd) {
#ifdef _WIN32
    ::shutdown(fd, SD_SEND);
#else
    ::shutdown(fd, SHUT_WR);
#endif
}

void setSocketTimeouts(SocketHandle fd) {
#ifdef _WIN32
    DWORD receiveTimeout = kRecvPollMs;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char *>(&receiveTimeout),
                 sizeof(receiveTimeout));
    DWORD sendTimeout = kSendTimeoutMs;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                 reinterpret_cast<const char *>(&sendTimeout),
                 sizeof(sendTimeout));
#else
    struct timeval receiveTimeout;
    receiveTimeout.tv_sec = 0;
    receiveTimeout.tv_usec = static_cast<suseconds_t>(kRecvPollMs) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char *>(&receiveTimeout),
                 sizeof(receiveTimeout));
    struct timeval sendTimeout;
    sendTimeout.tv_sec = kSendTimeoutMs / 1000u;
    sendTimeout.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                 reinterpret_cast<const char *>(&sendTimeout),
                 sizeof(sendTimeout));
#endif
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE,
                 reinterpret_cast<const char *>(&one), sizeof(one));
#endif
}

bool lastErrorWasTimeout() {
#ifdef _WIN32
    const int code = WSAGetLastError();
    return code == WSAETIMEDOUT || code == WSAEWOULDBLOCK || code == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

/* > 0 bytes read, 0 on orderly EOF, -1 on hard error/reset, -2 on the
 * bounded poll timeout (the caller re-checks its stop condition). */
int recvSome(SocketHandle fd, char *buffer, size_t capacity) {
#ifdef _WIN32
    const int got = ::recv(fd, buffer, static_cast<int>(capacity), 0);
#else
    const int got = static_cast<int>(::recv(fd, buffer, capacity, 0));
#endif
    if (got > 0) return got;
    if (got == 0) return 0;
    return lastErrorWasTimeout() ? -2 : -1;
}

bool sendAll(SocketHandle fd, const void *data, size_t size) {
    const char *bytes = static_cast<const char *>(data);
    size_t sent = 0u;
    while (sent < size) {
#ifdef MSG_NOSIGNAL
        const int flags = MSG_NOSIGNAL;
#else
        const int flags = 0;
#endif
#ifdef _WIN32
        const int wrote =
            ::send(fd, bytes + sent, static_cast<int>(size - sent), flags);
#else
        const int wrote =
            static_cast<int>(::send(fd, bytes + sent, size - sent, flags));
#endif
        if (wrote <= 0) return false; /* SO_SNDTIMEO bounds a stalled peer. */
        sent += static_cast<size_t>(wrote);
    }
    return true;
}

/* Absorb whatever the peer already sent, bounded, so the close frame we
 * just wrote arrives on a clean FIN instead of dying to an RST. */
void drainBriefly(SocketHandle fd) {
    const Clock::time_point deadline =
        Clock::now() + std::chrono::milliseconds(kCloseDrainMs);
    char discard[1024];
    while (Clock::now() < deadline) {
        const int got = recvSome(fd, discard, sizeof(discard));
        if (got == 0 || got == -1) return;
    }
}

/* ---- SHA-1 + base64 (RFC 6455 Sec-WebSocket-Accept derivation) ----------- */

uint32_t rotateLeft(uint32_t value, unsigned bits) {
    return (value << bits) | (value >> (32u - bits));
}

void sha1(const std::string &input, uint8_t digest[20]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u,
                     0xC3D2E1F0u};
    std::vector<uint8_t> data(input.begin(), input.end());
    const uint64_t bitLength = static_cast<uint64_t>(input.size()) * 8u;
    data.push_back(0x80u);
    while (data.size() % 64u != 56u) data.push_back(0u);
    for (int shift = 56; shift >= 0; shift -= 8) {
        data.push_back(static_cast<uint8_t>((bitLength >> shift) & 0xffu));
    }
    for (size_t offset = 0u; offset < data.size(); offset += 64u) {
        uint32_t w[80];
        for (unsigned index = 0u; index < 16u; index++) {
            w[index] =
                (static_cast<uint32_t>(data[offset + index * 4u]) << 24u) |
                (static_cast<uint32_t>(data[offset + index * 4u + 1u]) << 16u) |
                (static_cast<uint32_t>(data[offset + index * 4u + 2u]) << 8u) |
                static_cast<uint32_t>(data[offset + index * 4u + 3u]);
        }
        for (unsigned index = 16u; index < 80u; index++) {
            w[index] = rotateLeft(w[index - 3u] ^ w[index - 8u] ^
                                      w[index - 14u] ^ w[index - 16u],
                                  1u);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (unsigned index = 0u; index < 80u; index++) {
            uint32_t f;
            uint32_t k;
            if (index < 20u) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (index < 40u) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (index < 60u) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const uint32_t next = rotateLeft(a, 5u) + f + e + k + w[index];
            e = d;
            d = c;
            c = rotateLeft(b, 30u);
            b = a;
            a = next;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    for (unsigned index = 0u; index < 5u; index++) {
        digest[index * 4u] = static_cast<uint8_t>((h[index] >> 24u) & 0xffu);
        digest[index * 4u + 1u] =
            static_cast<uint8_t>((h[index] >> 16u) & 0xffu);
        digest[index * 4u + 2u] = static_cast<uint8_t>((h[index] >> 8u) & 0xffu);
        digest[index * 4u + 3u] = static_cast<uint8_t>(h[index] & 0xffu);
    }
}

std::string base64(const uint8_t *bytes, size_t size) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((size + 2u) / 3u) * 4u);
    size_t index = 0u;
    while (index + 3u <= size) {
        const uint32_t triple = (static_cast<uint32_t>(bytes[index]) << 16u) |
                                (static_cast<uint32_t>(bytes[index + 1u]) << 8u) |
                                static_cast<uint32_t>(bytes[index + 2u]);
        result.push_back(kAlphabet[(triple >> 18u) & 63u]);
        result.push_back(kAlphabet[(triple >> 12u) & 63u]);
        result.push_back(kAlphabet[(triple >> 6u) & 63u]);
        result.push_back(kAlphabet[triple & 63u]);
        index += 3u;
    }
    if (index + 1u == size) {
        const uint32_t rest = static_cast<uint32_t>(bytes[index]) << 16u;
        result.push_back(kAlphabet[(rest >> 18u) & 63u]);
        result.push_back(kAlphabet[(rest >> 12u) & 63u]);
        result += "==";
    } else if (index + 2u == size) {
        const uint32_t rest = (static_cast<uint32_t>(bytes[index]) << 16u) |
                              (static_cast<uint32_t>(bytes[index + 1u]) << 8u);
        result.push_back(kAlphabet[(rest >> 18u) & 63u]);
        result.push_back(kAlphabet[(rest >> 12u) & 63u]);
        result.push_back(kAlphabet[(rest >> 6u) & 63u]);
        result.push_back('=');
    }
    return result;
}

std::string wsAcceptValue(const std::string &key) {
    uint8_t digest[20];
    sha1(key + kWsGuid, digest);
    return base64(digest, sizeof(digest));
}

/* ---- HTTP parsing --------------------------------------------------------- */

struct Request {
    std::string method;
    std::string target;
    std::string version;
    std::map<std::string, std::string> headers; /* lowercase names */
};

std::string loweredCopy(const std::string &value) {
    std::string result = value;
    for (char &byte : result) {
        if (byte >= 'A' && byte <= 'Z') {
            byte = static_cast<char>(byte - 'A' + 'a');
        }
    }
    return result;
}

std::string trimmedCopy(const std::string &value) {
    size_t begin = 0u;
    size_t end = value.size();
    while (begin < end && (value[begin] == ' ' || value[begin] == '\t')) {
        begin++;
    }
    while (end > begin &&
           (value[end - 1u] == ' ' || value[end - 1u] == '\t')) {
        end--;
    }
    return value.substr(begin, end - begin);
}

bool parseRequestHead(const std::string &head, Request &request) {
    size_t lineEnd = head.find("\r\n");
    if (lineEnd == std::string::npos) lineEnd = head.size();
    const std::string line = head.substr(0u, lineEnd);
    const size_t firstSpace = line.find(' ');
    const size_t lastSpace = line.rfind(' ');
    if (firstSpace == std::string::npos || lastSpace == firstSpace) {
        return false;
    }
    request.method = line.substr(0u, firstSpace);
    request.target = line.substr(firstSpace + 1u, lastSpace - firstSpace - 1u);
    request.version = line.substr(lastSpace + 1u);
    if (request.method.empty() || request.target.empty() ||
        request.target.find(' ') != std::string::npos ||
        (request.version != "HTTP/1.1" && request.version != "HTTP/1.0")) {
        return false;
    }
    size_t cursor = lineEnd;
    while (cursor < head.size()) {
        cursor += 2u; /* the CRLF */
        size_t nextEnd = head.find("\r\n", cursor);
        if (nextEnd == std::string::npos) nextEnd = head.size();
        const std::string headerLine = head.substr(cursor, nextEnd - cursor);
        if (!headerLine.empty()) {
            const size_t colon = headerLine.find(':');
            if (colon == std::string::npos || colon == 0u) return false;
            const std::string name =
                loweredCopy(trimmedCopy(headerLine.substr(0u, colon)));
            const std::string value =
                trimmedCopy(headerLine.substr(colon + 1u));
            auto existing = request.headers.find(name);
            if (existing == request.headers.end()) {
                request.headers[name] = value;
            } else {
                existing->second += ", " + value;
            }
        }
        cursor = nextEnd;
    }
    return true;
}

bool tokenListContains(const std::string &value, const char *token) {
    const std::string wanted = loweredCopy(token);
    size_t start = 0u;
    while (start <= value.size()) {
        size_t end = value.find(',', start);
        if (end == std::string::npos) end = value.size();
        if (loweredCopy(trimmedCopy(value.substr(start, end - start))) ==
            wanted) {
            return true;
        }
        if (end == value.size()) break;
        start = end + 1u;
    }
    return false;
}

bool requestWantsClose(const Request &request) {
    const auto connection = request.headers.find("connection");
    if (connection != request.headers.end() &&
        tokenListContains(connection->second, "close")) {
        return true;
    }
    if (request.version == "HTTP/1.0") {
        return connection == request.headers.end() ||
               !tokenListContains(connection->second, "keep-alive");
    }
    return false;
}

/* ---- HTTP responses -------------------------------------------------------- */

bool sendResponse(SocketHandle fd, int status, const char *reason,
                  const char *contentType, const uint8_t *body,
                  size_t bodySize, const std::string &extraHeaders,
                  bool keepAlive) {
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason +
                       "\r\nContent-Type: " + contentType +
                       "\r\nContent-Length: " + std::to_string(bodySize) +
                       "\r\nConnection: " +
                       (keepAlive ? "keep-alive" : "close") + "\r\n" +
                       extraHeaders;
    for (const ResponseHeader &header : kResponseHeaders) {
        head += header.name;
        head += ": ";
        head += header.value;
        head += "\r\n";
    }
    head += "\r\n";
    if (!sendAll(fd, head.data(), head.size())) return false;
    return bodySize == 0u || sendAll(fd, body, bodySize);
}

bool sendPlainResponse(SocketHandle fd, int status, const char *reason,
                       const char *body, const std::string &extraHeaders,
                       bool keepAlive) {
    return sendResponse(fd, status, reason, "text/plain; charset=utf-8",
                        reinterpret_cast<const uint8_t *>(body),
                        std::strlen(body), extraHeaders, keepAlive);
}

/* Fixed refusal bodies. Nothing the requester sent is ever reflected:
 * reflected content on a LAN HTTP server is a phishing canvas. */
bool send404(SocketHandle fd, bool keepAlive) {
    return sendPlainResponse(fd, 404, "Not Found", "Not found.\n",
                             std::string{}, keepAlive);
}

} /* namespace */

/* ---- WebSocket connection state ------------------------------------------ */

struct MdkrLanPartyWsState {
    SocketHandle fd = kInvalidSocket;
    /* Serializes every frame write; sendText() may race the reader thread's
     * pong or close. closeSent is guarded by it: after a close frame goes
     * out, no other frame ever may (RFC 6455 5.5.1). */
    std::mutex sendMutex;
    bool closeSent = false;
    std::atomic<bool> open{true};
    std::mutex callbackMutex;
    std::function<void(const std::string &)> messageCallback;
    std::function<void()> closedCallback;
    bool closedNotified = false; /* guarded by callbackMutex */
};

namespace {

std::string wsFrameHeader(uint8_t opcode, size_t payloadSize) {
    std::string header;
    header.push_back(static_cast<char>(0x80u | opcode));
    if (payloadSize < 126u) {
        header.push_back(static_cast<char>(payloadSize));
    } else {
        /* The payload bound fits 16 bits with room to spare, so the 64-bit
         * length form is never produced. */
        header.push_back(static_cast<char>(126u));
        header.push_back(static_cast<char>((payloadSize >> 8u) & 0xffu));
        header.push_back(static_cast<char>(payloadSize & 0xffu));
    }
    return header;
}

bool wsSendFrame(MdkrLanPartyWsState &socket, uint8_t opcode,
                 const char *payload, size_t size) {
    std::lock_guard<std::mutex> lock(socket.sendMutex);
    if (socket.closeSent) return false;
    const std::string header = wsFrameHeader(opcode, size);
    if (!sendAll(socket.fd, header.data(), header.size())) return false;
    return size == 0u || sendAll(socket.fd, payload, size);
}

/* Latches closeSent, sends the close frame (code big-endian when present)
 * and half-closes the write side so the peer sees frame-then-FIN. */
void wsSendClose(MdkrLanPartyWsState &socket, const char *payload,
                 size_t size) {
    std::lock_guard<std::mutex> lock(socket.sendMutex);
    if (socket.closeSent) return;
    socket.closeSent = true;
    socket.open = false;
    const std::string header = wsFrameHeader(0x8u, size);
    if (sendAll(socket.fd, header.data(), header.size()) && size != 0u) {
        sendAll(socket.fd, payload, size);
    }
    shutdownWrite(socket.fd);
}

void wsSendCloseCode(MdkrLanPartyWsState &socket, uint16_t code) {
    char payload[2];
    payload[0] = static_cast<char>((code >> 8u) & 0xffu);
    payload[1] = static_cast<char>(code & 0xffu);
    wsSendClose(socket, payload, sizeof(payload));
}

void wsNotifyClosed(MdkrLanPartyWsState &socket) {
    socket.open = false;
    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lock(socket.callbackMutex);
        if (socket.closedNotified) return;
        socket.closedNotified = true;
        callback = socket.closedCallback;
    }
    if (callback) callback();
}

void wsDeliver(MdkrLanPartyWsState &socket, const std::string &payload) {
    std::function<void(const std::string &)> callback;
    {
        std::lock_guard<std::mutex> lock(socket.callbackMutex);
        callback = socket.messageCallback;
    }
    if (callback) callback(payload);
}

} /* namespace */

/* ---- MdkrLanPartyWebSocket ------------------------------------------------ */

MdkrLanPartyWebSocket::MdkrLanPartyWebSocket(
    std::shared_ptr<MdkrLanPartyWsState> state)
    : state_(std::move(state)) {}

MdkrLanPartyWebSocket::~MdkrLanPartyWebSocket() = default;

void MdkrLanPartyWebSocket::onMessage(
    std::function<void(const std::string &)> callback) {
    std::lock_guard<std::mutex> lock(state_->callbackMutex);
    state_->messageCallback = std::move(callback);
}

void MdkrLanPartyWebSocket::onClosed(std::function<void()> callback) {
    bool alreadyClosed = false;
    {
        std::lock_guard<std::mutex> lock(state_->callbackMutex);
        if (state_->closedNotified) {
            alreadyClosed = true; /* Late attach must not miss the close. */
        } else {
            state_->closedCallback = std::move(callback);
        }
    }
    if (alreadyClosed && callback) callback();
}

bool MdkrLanPartyWebSocket::sendText(const std::string &payload) {
    if (payload.size() > kMdkrLanPartyMaxWsPayloadBytes) return false;
    if (!state_->open) return false;
    return wsSendFrame(*state_, 0x1u, payload.data(), payload.size());
}

void MdkrLanPartyWebSocket::close(uint16_t code) {
    wsSendCloseCode(*state_, code);
    /* The connection's reader thread completes the handshake and fires
     * onClosed; it owns the socket's lifetime end to end. */
}

bool MdkrLanPartyWebSocket::isOpen() const { return state_->open; }

/* ---- Server state ---------------------------------------------------------- */

namespace {

struct Connection {
    SocketHandle fd = kInvalidSocket;
    std::thread thread;
    std::atomic<bool> done{false};
};

} /* namespace */

struct MdkrLanPartyServerState {
    std::mutex mutex;
    bool running = false;
    std::atomic<bool> stopping{false};
    SocketHandle listenFd = kInvalidSocket;
    uint16_t boundPort = 0u;
    /* Frozen before the accept thread exists, cleared after every
     * connection thread is joined: connection threads read it unlocked. */
    MdkrLanPartyManifest manifest;
    std::function<void(std::shared_ptr<MdkrLanPartyWebSocket>)> wsCallback;
    std::thread acceptThread;
    std::vector<std::shared_ptr<Connection>> connections; /* guarded */
};

namespace {

/* ---- The WebSocket frame loop --------------------------------------------- */

void runWebSocket(const std::shared_ptr<MdkrLanPartyServerState> &state,
                  const std::shared_ptr<MdkrLanPartyWsState> &socket,
                  std::string carried) {
    bool drainBeforeTeardown = false;
    const auto need = [&](size_t count) -> bool {
        while (carried.size() < count) {
            if (state->stopping) return false;
            char chunk[4096];
            const int got = recvSome(socket->fd, chunk, sizeof(chunk));
            if (got == 0 || got == -1) return false;
            if (got == -2) continue;
            carried.append(chunk, static_cast<size_t>(got));
        }
        return true;
    };
    for (;;) {
        if (!need(2u)) break;
        const uint8_t byte0 = static_cast<uint8_t>(carried[0]);
        const uint8_t byte1 = static_cast<uint8_t>(carried[1]);
        const bool fin = (byte0 & 0x80u) != 0u;
        const uint8_t reserved = byte0 & 0x70u;
        const uint8_t opcode = byte0 & 0x0fu;
        const bool masked = (byte1 & 0x80u) != 0u;
        size_t length = byte1 & 0x7fu;
        /* Masked client frames are REQUIRED (RFC 6455 5.1); an unmasked or
         * reserved-bit frame is a protocol error. */
        if (reserved != 0u || !masked) {
            wsSendCloseCode(*socket, 1002u);
            drainBeforeTeardown = true;
            break;
        }
        size_t headerSize = 2u;
        if (length == 127u) {
            /* A 64-bit length can only ever declare more than the party
             * control-message bound: refuse from the declaration alone. */
            wsSendCloseCode(*socket, 1009u);
            drainBeforeTeardown = true;
            break;
        }
        if (length == 126u) {
            if (!need(4u)) break;
            length =
                (static_cast<size_t>(static_cast<uint8_t>(carried[2])) << 8u) |
                static_cast<size_t>(static_cast<uint8_t>(carried[3]));
            headerSize = 4u;
        }
        if (length > kMdkrLanPartyMaxWsPayloadBytes) {
            wsSendCloseCode(*socket, 1009u);
            drainBeforeTeardown = true;
            break;
        }
        const bool control = (opcode & 0x8u) != 0u;
        if (opcode == 0x0u || (!control && !fin)) {
            /* Fragmentation refused politely: bounded control messages
             * never need it, and a reassembly buffer is attack surface. */
            wsSendCloseCode(*socket, 1003u);
            drainBeforeTeardown = true;
            break;
        }
        if (control && (!fin || length > 125u)) {
            wsSendCloseCode(*socket, 1002u);
            drainBeforeTeardown = true;
            break;
        }
        if (!need(headerSize + 4u + length)) break;
        uint8_t mask[4];
        for (unsigned index = 0u; index < 4u; index++) {
            mask[index] = static_cast<uint8_t>(carried[headerSize + index]);
        }
        std::string payload = carried.substr(headerSize + 4u, length);
        for (size_t index = 0u; index < payload.size(); index++) {
            payload[index] = static_cast<char>(
                static_cast<uint8_t>(payload[index]) ^ mask[index % 4u]);
        }
        carried.erase(0u, headerSize + 4u + length);
        if (opcode == 0x1u || opcode == 0x2u) {
            wsDeliver(*socket, payload);
        } else if (opcode == 0x9u) {
            /* Ping -> pong with the identical payload. */
            wsSendFrame(*socket, 0xau, payload.data(), payload.size());
        } else if (opcode == 0xau) {
            /* Unsolicited pong: ignored. */
        } else if (opcode == 0x8u) {
            /* Close handshake: echo their code (or an empty close when
             * they sent none) unless our side already sent one. */
            wsSendClose(*socket, payload.data(),
                        payload.size() >= 2u ? 2u : 0u);
            break;
        } else {
            wsSendCloseCode(*socket, 1002u);
            drainBeforeTeardown = true;
            break;
        }
    }
    if (drainBeforeTeardown) drainBriefly(socket->fd);
    shutdownBoth(socket->fd);
    wsNotifyClosed(*socket);
}

/* ---- The per-connection HTTP loop ------------------------------------------ */

enum class UpgradeCheck { Ok, NotAnUpgrade, BadKey };

UpgradeCheck validateUpgrade(const Request &request, std::string &key) {
    const auto upgrade = request.headers.find("upgrade");
    const auto connection = request.headers.find("connection");
    const auto version = request.headers.find("sec-websocket-version");
    if (upgrade == request.headers.end() ||
        !tokenListContains(upgrade->second, "websocket") ||
        connection == request.headers.end() ||
        !tokenListContains(connection->second, "upgrade") ||
        version == request.headers.end() || version->second != "13") {
        return UpgradeCheck::NotAnUpgrade;
    }
    const auto found = request.headers.find("sec-websocket-key");
    if (found == request.headers.end() || found->second.size() != 24u) {
        return UpgradeCheck::BadKey;
    }
    key = found->second;
    return UpgradeCheck::Ok;
}

void serveConnection(std::shared_ptr<MdkrLanPartyServerState> state,
                     std::shared_ptr<Connection> connection) {
    setSocketTimeouts(connection->fd);
    std::string buffer;
    size_t served = 0u;
    Clock::time_point deadline =
        Clock::now() + std::chrono::milliseconds(kHttpIdleDeadlineMs);
    while (!state->stopping) {
        const size_t headerEnd = buffer.find("\r\n\r\n");
        if (headerEnd == std::string::npos) {
            /* Bounds first: an oversized request line (or head) cuts the
             * connection with no status line an attacker could probe. */
            const size_t lineEnd = buffer.find("\r\n");
            const size_t lineSize =
                lineEnd == std::string::npos ? buffer.size() : lineEnd;
            if (lineSize > kMaxRequestLineBytes ||
                buffer.size() > kMaxRequestHeaderBytes) {
                break;
            }
            char chunk[4096];
            const int got = recvSome(connection->fd, chunk, sizeof(chunk));
            if (got == 0 || got == -1) break;
            if (got == -2) {
                if (Clock::now() >= deadline) break;
                continue;
            }
            buffer.append(chunk, static_cast<size_t>(got));
            continue;
        }
        Request request;
        if (!parseRequestHead(buffer.substr(0u, headerEnd), request)) {
            sendPlainResponse(connection->fd, 400, "Bad Request",
                              "Bad request.\n", std::string{}, false);
            break;
        }
        buffer.erase(0u, headerEnd + 4u);
        served++;
        if (request.method != "GET") {
            /* Any body that came with it stays unread, so this connection
             * cannot stay in framing sync: refuse and close. */
            sendPlainResponse(connection->fd, 405, "Method Not Allowed",
                              "Method not allowed.\n", "Allow: GET\r\n",
                              false);
            break;
        }
        std::string path = request.target;
        const size_t cut = path.find_first_of("?#");
        if (cut != std::string::npos) path.erase(cut);
        if (path == kWsPath) {
            std::string key;
            const UpgradeCheck check = validateUpgrade(request, key);
            if (check == UpgradeCheck::NotAnUpgrade) {
                sendPlainResponse(connection->fd, 426, "Upgrade Required",
                                  "WebSocket upgrade required.\n",
                                  "Upgrade: websocket\r\n"
                                  "Sec-WebSocket-Version: 13\r\n",
                                  false);
                break;
            }
            if (check == UpgradeCheck::BadKey) {
                sendPlainResponse(connection->fd, 400, "Bad Request",
                                  "Bad request.\n", std::string{}, false);
                break;
            }
            std::function<void(std::shared_ptr<MdkrLanPartyWebSocket>)>
                callback;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                callback = state->wsCallback;
            }
            if (!callback) {
                /* No consumer registered: refuse without upgrading. */
                send404(connection->fd, false);
                break;
            }
            const std::string response =
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: " +
                wsAcceptValue(key) + "\r\n\r\n";
            if (!sendAll(connection->fd, response.data(), response.size())) {
                break;
            }
            auto wsState = std::make_shared<MdkrLanPartyWsState>();
            wsState->fd = connection->fd;
            /* The consumer gets the socket BEFORE any frame is parsed, on
             * this thread, so callbacks it attaches inside the callback
             * can never miss a message. Any pipelined bytes after the
             * upgrade head are the first frames -- they ride along. */
            callback(std::make_shared<MdkrLanPartyWebSocket>(wsState));
            runWebSocket(state, wsState, std::move(buffer));
            break;
        }
        const auto found = state->manifest.find(path);
        const bool keepAlive = !requestWantsClose(request) &&
                               served < kMaxRequestsPerConnection;
        bool sent;
        if (found == state->manifest.end()) {
            sent = send404(connection->fd, keepAlive);
        } else {
            sent = sendResponse(
                connection->fd, 200, "OK", found->second.contentType.c_str(),
                found->second.bytes.data(), found->second.bytes.size(),
                std::string{}, keepAlive);
        }
        if (!sent || !keepAlive) break;
        deadline = Clock::now() + std::chrono::milliseconds(kHttpIdleDeadlineMs);
    }
    closeSocket(connection->fd);
    connection->done = true;
}

/* ---- Accept thread ----------------------------------------------------------- */

void reapFinishedConnections(
    const std::shared_ptr<MdkrLanPartyServerState> &state) {
    std::vector<std::shared_ptr<Connection>> finished;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        for (auto iterator = state->connections.begin();
             iterator != state->connections.end();) {
            if ((*iterator)->done) {
                finished.push_back(*iterator);
                iterator = state->connections.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }
    for (const auto &connection : finished) {
        if (connection->thread.joinable()) connection->thread.join();
    }
}

void acceptLoop(std::shared_ptr<MdkrLanPartyServerState> state) {
    const SocketHandle listenFd = state->listenFd;
    while (!state->stopping) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listenFd, &readable);
        struct timeval interval;
        interval.tv_sec = 0;
        interval.tv_usec =
            static_cast<decltype(interval.tv_usec)>(kAcceptPollMs) * 1000;
#ifdef _WIN32
        const int ready = ::select(0, &readable, nullptr, nullptr, &interval);
#else
        const int ready =
            ::select(listenFd + 1, &readable, nullptr, nullptr, &interval);
#endif
        reapFinishedConnections(state);
        if (state->stopping) break;
        if (ready <= 0) continue;
        const SocketHandle client = ::accept(listenFd, nullptr, nullptr);
        if (client == kInvalidSocket) continue;
        auto connection = std::make_shared<Connection>();
        connection->fd = client;
        bool admitted = false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->stopping &&
                state->connections.size() < kMaxConnections) {
                state->connections.push_back(connection);
                admitted = true;
            }
        }
        if (!admitted) {
            /* Full house: a LAN party is four phones and a page load or
             * two; anything past the cap is cut rather than queued. */
            closeSocket(client);
            continue;
        }
        connection->thread = std::thread(serveConnection, state, connection);
    }
}

} /* namespace */

/* ---- MdkrLanPartyServer -------------------------------------------------------- */

MdkrLanPartyServer::MdkrLanPartyServer()
    : state_(std::make_shared<MdkrLanPartyServerState>()) {}

MdkrLanPartyServer::~MdkrLanPartyServer() { stop(); }

void MdkrLanPartyServer::onWebSocket(
    std::function<void(std::shared_ptr<MdkrLanPartyWebSocket>)> callback) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->wsCallback = std::move(callback);
}

bool MdkrLanPartyServer::start(uint16_t port, MdkrLanPartyManifest manifest) {
    ensureSocketsInitialized();
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->running) return false;
    const SocketHandle fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kInvalidSocket) return false;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char *>(&one), sizeof(one));
    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    /* Every interface: the phones are on the LAN, and one port here is the
     * one firewall prompt the player ever sees. */
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<struct sockaddr *>(&address),
               sizeof(address)) != 0 ||
        ::listen(fd, 16) != 0) {
        closeSocket(fd);
        return false;
    }
    struct sockaddr_in bound;
    std::memset(&bound, 0, sizeof(bound));
    socklen_t boundSize = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<struct sockaddr *>(&bound),
                      &boundSize) != 0) {
        closeSocket(fd);
        return false;
    }
    state_->listenFd = fd;
    state_->boundPort = ntohs(bound.sin_port);
    state_->manifest = std::move(manifest);
    state_->stopping = false;
    state_->running = true;
    state_->acceptThread = std::thread(acceptLoop, state_);
    return true;
}

void MdkrLanPartyServer::stop() {
    std::thread acceptThread;
    SocketHandle listenFd = kInvalidSocket;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->running) return;
        state_->running = false;
        state_->stopping = true;
        listenFd = state_->listenFd;
        state_->listenFd = kInvalidSocket;
        state_->boundPort = 0u;
        acceptThread = std::move(state_->acceptThread);
    }
    /* Join the accept thread FIRST (it wakes within its poll interval): once
     * it is gone, no new connection can join the vector we drain next. */
    if (acceptThread.joinable()) acceptThread.join();
    if (listenFd != kInvalidSocket) closeSocket(listenFd);
    std::vector<std::shared_ptr<Connection>> connections;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        connections.swap(state_->connections);
    }
    for (const auto &connection : connections) {
        /* Wakes any blocking read; each thread then tears itself down and
         * fires its socket's onClosed on the way out. */
        shutdownBoth(connection->fd);
    }
    for (const auto &connection : connections) {
        if (connection->thread.joinable()) connection->thread.join();
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->manifest.clear();
    state_->stopping = false;
}

uint16_t MdkrLanPartyServer::port() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->boundPort;
}
