/*
 * MdkrLanPartyServer: the embedded HTTP+WS server for local-only Phone
 * Party. One port serves the packaged controller assets over plain HTTP
 * and upgrades /party-ws to RFC 6455 -- 'self' CSP semantics and a single
 * firewall prompt depend on that.
 *
 * Everything here runs against a real socket on 127.0.0.1 with an
 * ephemeral port: the properties worth pinning (traversal impossible by
 * construction, no request echo, byte-identical bodies, the security
 * header table on every 200, the bounded WS frame rules) are wire-level
 * facts, not function-call facts.
 */
#include "party/lan_party_server.h"

/* Assert-driven test: NDEBUG would compile every check away. */
#undef NDEBUG

#include <atomic>
#include <cassert>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using TestSocket = SOCKET;
static const TestSocket kBadSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using TestSocket = int;
static const TestSocket kBadSocket = -1;
#endif

namespace {

void closeTestSocket(TestSocket fd) {
#ifdef _WIN32
    ::closesocket(fd);
#else
    ::close(fd);
#endif
}

/* Bounded-everything loopback client. Every read carries a timeout so a
 * server defect fails the test instead of hanging the suite. */
class TestClient {
public:
    explicit TestClient(uint16_t port) {
#ifdef _WIN32
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(fd_ != kBadSocket);
#ifdef SO_NOSIGPIPE
        int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE,
                     reinterpret_cast<const char *>(&one), sizeof(one));
#endif
#ifdef _WIN32
        DWORD timeout = 5000u;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char *>(&timeout), sizeof(timeout));
#else
        struct timeval timeout;
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char *>(&timeout), sizeof(timeout));
#endif
        struct sockaddr_in address;
        std::memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const int connected = ::connect(
            fd_, reinterpret_cast<struct sockaddr *>(&address),
            sizeof(address));
        assert(connected == 0);
    }

    ~TestClient() {
        if (fd_ != kBadSocket) closeTestSocket(fd_);
    }

    TestClient(const TestClient &) = delete;
    TestClient &operator=(const TestClient &) = delete;

    /* True only if every byte was handed to the kernel; false once the
     * server has cut the connection (the drip test's detection signal). */
    bool trySend(const std::string &bytes) {
#ifdef MSG_NOSIGNAL
        const int flags = MSG_NOSIGNAL; /* No SIGPIPE when the server cuts. */
#else
        const int flags = 0;
#endif
        size_t sent = 0u;
        while (sent < bytes.size()) {
            const int wrote = static_cast<int>(::send(
                fd_, bytes.data() + sent,
#ifdef _WIN32
                static_cast<int>(bytes.size() - sent),
#else
                bytes.size() - sent,
#endif
                flags));
            if (wrote <= 0) return false;
            sent += static_cast<size_t>(wrote);
        }
        return true;
    }

    void send(const std::string &bytes) {
        (void)trySend(bytes); /* Peer closed early; tests assert reads. */
    }

    /* One recv() worth of bytes, or empty on EOF/timeout/reset. */
    std::string recvSome() {
        char chunk[4096];
        const int got = static_cast<int>(::recv(fd_, chunk, sizeof(chunk), 0));
        if (got <= 0) return std::string{};
        return std::string(chunk, static_cast<size_t>(got));
    }

    /* Accumulate until `needle` appears or the peer closes. */
    std::string readUntil(const std::string &needle) {
        std::string collected;
        while (collected.find(needle) == std::string::npos) {
            const std::string chunk = recvSome();
            if (chunk.empty()) break;
            collected += chunk;
        }
        return collected;
    }

    std::string readExactly(size_t count, std::string carried) {
        while (carried.size() < count) {
            const std::string chunk = recvSome();
            if (chunk.empty()) break;
            carried += chunk;
        }
        return carried;
    }

    std::string readToClose() {
        std::string collected;
        for (;;) {
            const std::string chunk = recvSome();
            if (chunk.empty()) return collected;
            collected += chunk;
        }
    }

private:
    TestSocket fd_ = kBadSocket;
};

struct HttpResponse {
    std::string statusLine;
    std::map<std::string, std::string> headers; /* lowercase names */
    std::string body;
    std::string raw;
};

std::string lowered(std::string value) {
    for (char &byte : value) {
        byte = static_cast<char>(
            std::tolower(static_cast<unsigned char>(byte)));
    }
    return value;
}

HttpResponse readResponse(TestClient &client) {
    HttpResponse response;
    std::string head = client.readUntil("\r\n\r\n");
    const size_t headerEnd = head.find("\r\n\r\n");
    assert(headerEnd != std::string::npos);
    response.raw = head;
    const std::string headerBlock = head.substr(0u, headerEnd);
    size_t lineStart = 0u;
    bool first = true;
    while (lineStart <= headerBlock.size()) {
        size_t lineEnd = headerBlock.find("\r\n", lineStart);
        if (lineEnd == std::string::npos) lineEnd = headerBlock.size();
        const std::string line =
            headerBlock.substr(lineStart, lineEnd - lineStart);
        if (first) {
            response.statusLine = line;
            first = false;
        } else if (!line.empty()) {
            const size_t colon = line.find(':');
            assert(colon != std::string::npos);
            std::string name = lowered(line.substr(0u, colon));
            std::string value = line.substr(colon + 1u);
            while (!value.empty() && value.front() == ' ') value.erase(0u, 1u);
            response.headers[name] = value;
        }
        if (lineEnd == headerBlock.size()) break;
        lineStart = lineEnd + 2u;
    }
    size_t contentLength = 0u;
    const auto found = response.headers.find("content-length");
    if (found != response.headers.end()) {
        contentLength = static_cast<size_t>(std::stoul(found->second));
    }
    std::string body = head.substr(headerEnd + 4u);
    body = client.readExactly(contentLength, std::move(body));
    response.raw += body;
    response.body = body.substr(0u, contentLength);
    return response;
}

MdkrLanPartyManifest testManifest() {
    MdkrLanPartyManifest manifest;
    const char kHtml[] = "<!doctype html><title>party pad</title>";
    manifest["/controller/index.html"] = MdkrLanPartyAsset{
        std::vector<uint8_t>(kHtml, kHtml + sizeof(kHtml) - 1u),
        mdkr_lan_party_content_type("/controller/index.html")};
    const char kJs[] = "export const pad = () => 'ready';\n";
    manifest["/controller/controller.js"] = MdkrLanPartyAsset{
        std::vector<uint8_t>(kJs, kJs + sizeof(kJs) - 1u),
        mdkr_lan_party_content_type("/controller/controller.js")};
    const char kCss[] = "body{background:#123;}\n";
    manifest["/controller/controller.css"] = MdkrLanPartyAsset{
        std::vector<uint8_t>(kCss, kCss + sizeof(kCss) - 1u),
        mdkr_lan_party_content_type("/controller/controller.css")};
    /* Binary asset with NUL, high bytes and CRLF inside: the byte-identical
     * assertion must survive anything a text-path bug would mangle. */
    std::vector<uint8_t> png = {0x89u, 'P', 'N', 'G', 0x0du, 0x0au,
                                0x1au, 0x0au, 0x00u, 0xffu, 0x7fu, 0x00u,
                                0x0du, 0x0au, 0x00u, 0xfeu};
    manifest["/assets/icon.png"] =
        MdkrLanPartyAsset{png, mdkr_lan_party_content_type("/assets/icon.png")};
    const char kManifestJson[] = "{\"name\":\"party pad\"}";
    manifest["/manifest.webmanifest"] = MdkrLanPartyAsset{
        std::vector<uint8_t>(kManifestJson,
                             kManifestJson + sizeof(kManifestJson) - 1u),
        mdkr_lan_party_content_type("/manifest.webmanifest")};
    return manifest;
}

std::string simpleGet(const std::string &target) {
    return "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
}

/* ---- WebSocket client-side helpers -------------------------------------- */

std::string maskedFrame(uint8_t opcode, const std::string &payload, bool fin,
                        bool masked) {
    std::string frame;
    frame.push_back(static_cast<char>((fin ? 0x80u : 0x00u) | opcode));
    const uint8_t maskBit = masked ? 0x80u : 0x00u;
    if (payload.size() < 126u) {
        frame.push_back(
            static_cast<char>(maskBit | static_cast<uint8_t>(payload.size())));
    } else {
        frame.push_back(static_cast<char>(maskBit | 126u));
        frame.push_back(static_cast<char>((payload.size() >> 8u) & 0xffu));
        frame.push_back(static_cast<char>(payload.size() & 0xffu));
    }
    const uint8_t mask[4] = {0x11u, 0x22u, 0x33u, 0x44u};
    if (masked) {
        for (const uint8_t byte : mask) {
            frame.push_back(static_cast<char>(byte));
        }
    }
    for (size_t index = 0u; index < payload.size(); index++) {
        const uint8_t plain = static_cast<uint8_t>(payload[index]);
        frame.push_back(static_cast<char>(
            masked ? (plain ^ mask[index % 4u]) : plain));
    }
    return frame;
}

struct WsFrame {
    uint8_t opcode = 0u;
    bool fin = false;
    std::string payload;
};

/* Reads one server frame (servers never mask). `carried` holds bytes already
 * pulled off the socket; leftovers stay in it for the next call. */
bool readServerFrame(TestClient &client, std::string &carried, WsFrame &frame) {
    while (carried.size() < 2u) {
        const std::string chunk = client.recvSome();
        if (chunk.empty()) return false;
        carried += chunk;
    }
    const uint8_t byte0 = static_cast<uint8_t>(carried[0]);
    const uint8_t byte1 = static_cast<uint8_t>(carried[1]);
    assert((byte1 & 0x80u) == 0u); /* Server frames must be unmasked. */
    frame.fin = (byte0 & 0x80u) != 0u;
    frame.opcode = byte0 & 0x0fu;
    size_t headerSize = 2u;
    size_t length = byte1 & 0x7fu;
    if (length == 126u) {
        while (carried.size() < 4u) {
            const std::string chunk = client.recvSome();
            if (chunk.empty()) return false;
            carried += chunk;
        }
        length = (static_cast<size_t>(static_cast<uint8_t>(carried[2])) << 8u) |
                 static_cast<size_t>(static_cast<uint8_t>(carried[3]));
        headerSize = 4u;
    }
    assert(length != 127u);
    while (carried.size() < headerSize + length) {
        const std::string chunk = client.recvSome();
        if (chunk.empty()) return false;
        carried += chunk;
    }
    frame.payload = carried.substr(headerSize, length);
    carried.erase(0u, headerSize + length);
    return true;
}

uint16_t closeCode(const WsFrame &frame) {
    assert(frame.opcode == 0x8u);
    assert(frame.payload.size() >= 2u);
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(static_cast<uint8_t>(frame.payload[0])) << 8u) |
        static_cast<uint16_t>(static_cast<uint8_t>(frame.payload[1])));
}

/* RFC 6455 section 1.3 sample handshake: this exact key must produce this
 * exact accept value, which pins the SHA-1 + base64 derivation. */
const char kSampleKey[] = "dGhlIHNhbXBsZSBub25jZQ==";
const char kSampleAccept[] = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

std::string upgradeRequest(const std::string &host = "127.0.0.1") {
    return std::string("GET /party-ws HTTP/1.1\r\n") +
           "Host: " + host + "\r\n" +
           "Upgrade: websocket\r\n" +
           "Connection: Upgrade\r\n" +
           "Sec-WebSocket-Key: " + kSampleKey + "\r\n" +
           "Sec-WebSocket-Version: 13\r\n\r\n";
}

/* An upgrade that offers a Sec-WebSocket-Protocol value (a single token or a
 * comma list), so the subprotocol-negotiation branch can be exercised. */
std::string upgradeRequestWithProtocol(const std::string &offered) {
    return std::string("GET /party-ws HTTP/1.1\r\n") +
           "Host: 127.0.0.1\r\n" +
           "Upgrade: websocket\r\n" +
           "Connection: Upgrade\r\n" +
           "Sec-WebSocket-Key: " + kSampleKey + "\r\n" +
           "Sec-WebSocket-Protocol: " + offered + "\r\n" +
           "Sec-WebSocket-Version: 13\r\n\r\n";
}

/* Completes the client half of the upgrade and asserts the 101. */
void completeUpgrade(TestClient &client) {
    client.send(upgradeRequest());
    const std::string reply = client.readUntil("\r\n\r\n");
    assert(reply.find("HTTP/1.1 101") == 0u);
    assert(reply.find(kSampleAccept) != std::string::npos);
}

bool waitFor(const std::function<bool()> &done, unsigned budgetMs = 5000u) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(budgetMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

/* Collects what the server-side socket sees, across threads. */
struct WsProbe {
    std::mutex mutex;
    std::shared_ptr<MdkrLanPartyWebSocket> socket;
    std::vector<std::string> messages;
    bool closed = false;

    void attach(MdkrLanPartyServer &server) {
        server.onWebSocket(
            [this](std::shared_ptr<MdkrLanPartyWebSocket> accepted) {
                accepted->onMessage([this](const std::string &payload) {
                    std::lock_guard<std::mutex> lock(mutex);
                    messages.push_back(payload);
                });
                accepted->onClosed([this]() {
                    std::lock_guard<std::mutex> lock(mutex);
                    closed = true;
                });
                std::lock_guard<std::mutex> lock(mutex);
                socket = std::move(accepted);
            });
    }

    bool hasSocket() {
        std::lock_guard<std::mutex> lock(mutex);
        return socket != nullptr;
    }

    size_t messageCount() {
        std::lock_guard<std::mutex> lock(mutex);
        return messages.size();
    }

    bool wasClosed() {
        std::lock_guard<std::mutex> lock(mutex);
        return closed;
    }
};

/* ---- Cases --------------------------------------------------------------- */

void contentTypeTableCoversThePackagedKinds() {
    assert(mdkr_lan_party_content_type("/a/index.html") ==
           std::string("text/html; charset=utf-8"));
    assert(mdkr_lan_party_content_type("/a/app.js") ==
           std::string("text/javascript; charset=utf-8"));
    assert(mdkr_lan_party_content_type("/a/style.css") ==
           std::string("text/css; charset=utf-8"));
    assert(mdkr_lan_party_content_type("/a/icon.png") ==
           std::string("image/png"));
    assert(mdkr_lan_party_content_type("/a/data.json") ==
           std::string("application/json"));
    assert(mdkr_lan_party_content_type("/a/art.svg") ==
           std::string("image/svg+xml"));
    assert(mdkr_lan_party_content_type("/manifest.webmanifest") ==
           std::string("application/manifest+json"));
    assert(mdkr_lan_party_content_type("/mystery.bin") ==
           std::string("application/octet-stream"));
}

void startBindsAnEphemeralPortAndStops() {
    MdkrLanPartyServer server;
    assert(server.start(0u, testManifest()));
    assert(server.port() != 0u);
    /* Second start while running must refuse rather than double-bind. */
    assert(!server.start(0u, testManifest()));
    server.stop();
    assert(server.port() == 0u);
    /* stop() must be re-entrant and the instance restartable. */
    server.stop();
    assert(server.start(0u, testManifest()));
    assert(server.port() != 0u);
}

void manifestAssetsServeByteIdenticalWithTheHeaderTable(
    MdkrLanPartyServer &server, const MdkrLanPartyManifest &manifest) {
    for (const auto &entry : manifest) {
        TestClient client(server.port());
        client.send(simpleGet(entry.first));
        const HttpResponse response = readResponse(client);
        assert(response.statusLine.find("HTTP/1.1 200") == 0u);
        const std::string expected(entry.second.bytes.begin(),
                                   entry.second.bytes.end());
        assert(response.body == expected); /* byte-identical, always */
        assert(response.headers.at("content-type") ==
               entry.second.contentType);
        /* The security-header table rides EVERY 200. */
        const std::string csp =
            response.headers.at("content-security-policy");
        assert(csp.find("default-src 'self'") != std::string::npos);
        assert(csp.find("connect-src 'self' ws:") != std::string::npos);
        assert(csp.find("frame-ancestors 'none'") != std::string::npos);
        /* A plain-http origin must never carry the cloud's TLS-only
         * directives: upgrade-insecure-requests would rewrite every
         * same-origin subresource to https: this server cannot speak. */
        assert(csp.find("upgrade-insecure-requests") == std::string::npos);
        assert(response.headers.count("strict-transport-security") == 0u);
        assert(response.headers.at("x-content-type-options") == "nosniff");
        assert(response.headers.at("x-frame-options") == "DENY");
        assert(response.headers.at("referrer-policy") == "no-referrer");
        assert(response.headers.at("cross-origin-opener-policy") ==
               "same-origin");
        assert(response.headers.at("cross-origin-resource-policy") ==
               "same-origin");
        assert(response.headers.count("permissions-policy") == 1u);
        assert(response.headers.at("cache-control") == "no-store");
    }
}

void keepAliveServesASecondRequestOnTheSameConnection(
    MdkrLanPartyServer &server) {
    TestClient client(server.port());
    client.send(simpleGet("/controller/index.html"));
    const HttpResponse first = readResponse(client);
    assert(first.statusLine.find("200") != std::string::npos);
    client.send(simpleGet("/controller/controller.js"));
    const HttpResponse second = readResponse(client);
    assert(second.statusLine.find("200") != std::string::npos);
    assert(second.body.find("pad") != std::string::npos);
}

void traversalShapesAll404WithoutEchoingTheRequest(
    MdkrLanPartyServer &server) {
    const std::vector<std::string> shapes = {
        "/../launcher.ini",
        "/controller/../../launcher.ini",
        "/%2e%2e/%2e%2e/launcher.ini",
        "/controller/..%2f..%2flauncher.ini",
        "//controller/index.html",
        "/..\\..\\launcher.ini",
        "\\controller\\index.html",
        "http://evil.example/controller/index.html",
        "/%c0%ae%c0%ae/launcher.ini",
        "/controller/index.html/..",
        "/controller/index.html%00",
        "/nonexistent.js",
    };
    for (const std::string &shape : shapes) {
        TestClient client(server.port());
        client.send(simpleGet(shape));
        const HttpResponse response = readResponse(client);
        assert(response.statusLine.find("HTTP/1.1 404") == 0u);
        /* No echo: nothing the requester sent may appear anywhere in the
         * response bytes -- reflected content on a LAN server is a phishing
         * canvas. "launcher.ini" stands in for every shape's tail. */
        assert(response.raw.find("launcher.ini") == std::string::npos);
        assert(response.raw.find("evil.example") == std::string::npos);
        assert(response.raw.find("%2e") == std::string::npos);
        /* 404s still carry the header table (fail-safe posture). */
        assert(response.headers.count("content-security-policy") == 1u);
    }
}

void nonGetIsRefusedWith405(MdkrLanPartyServer &server) {
    for (const char *method : {"POST", "PUT", "DELETE", "OPTIONS", "HEAD"}) {
        TestClient client(server.port());
        client.send(std::string(method) +
                    " /controller/index.html HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\nContent-Length: 0\r\n\r\n");
        const HttpResponse response = readResponse(client);
        assert(response.statusLine.find("HTTP/1.1 405") == 0u);
        assert(response.headers.at("allow") == "GET");
    }
}

void oversizedRequestLineClosesWithoutAResponse(MdkrLanPartyServer &server) {
    TestClient client(server.port());
    /* No CRLF anywhere: the line just grows past any sane bound. The server
     * must cut the connection without ever writing a status line an
     * attacker could use as an oracle. */
    client.send("GET /" + std::string(6000u, 'A'));
    const std::string received = client.readToClose();
    assert(received.empty());
}

void plainGetOnThePartyWsPathDoesNotUpgrade(MdkrLanPartyServer &server) {
    TestClient client(server.port());
    client.send(simpleGet("/party-ws"));
    const HttpResponse response = readResponse(client);
    /* No Upgrade tokens -> not a WebSocket handshake; the reply must be an
     * HTTP refusal, never a 101 and never an asset. */
    assert(response.statusLine.find("101") == std::string::npos);
    assert(response.statusLine.find("HTTP/1.1 426") == 0u);
}

void upgradeMatchesTheRfcSampleAndRoundTripsMessages(
    MdkrLanPartyServer &server, WsProbe &probe) {
    TestClient client(server.port());
    completeUpgrade(client);
    assert(waitFor([&probe]() { return probe.hasSocket(); }));

    /* Client -> server text. */
    client.send(maskedFrame(0x1u, "hello-party", true, true));
    assert(waitFor([&probe]() { return probe.messageCount() >= 1u; }));
    {
        std::lock_guard<std::mutex> lock(probe.mutex);
        assert(probe.messages[0] == "hello-party");
    }

    /* Server -> client text. */
    std::shared_ptr<MdkrLanPartyWebSocket> socket;
    {
        std::lock_guard<std::mutex> lock(probe.mutex);
        socket = probe.socket;
    }
    assert(socket->isOpen());
    assert(socket->sendText("seat-assigned"));
    std::string carried;
    WsFrame frame;
    assert(readServerFrame(client, carried, frame));
    assert(frame.opcode == 0x1u);
    assert(frame.fin);
    assert(frame.payload == "seat-assigned");

    /* Ping -> pong with the identical payload. */
    client.send(maskedFrame(0x9u, "abc", true, true));
    assert(readServerFrame(client, carried, frame));
    assert(frame.opcode == 0xau);
    assert(frame.payload == "abc");

    /* Clean close handshake: client close 1000 -> echoed close, EOF,
     * onClosed observed. */
    std::string closePayload;
    closePayload.push_back(static_cast<char>(0x03));
    closePayload.push_back(static_cast<char>(0xe8));
    client.send(maskedFrame(0x8u, closePayload, true, true));
    assert(readServerFrame(client, carried, frame));
    assert(frame.opcode == 0x8u);
    assert(closeCode(frame) == 1000u);
    assert(client.recvSome().empty()); /* FIN after the close echo. */
    assert(waitFor([&probe]() { return probe.wasClosed(); }));
    assert(waitFor([&socket]() { return !socket->isOpen(); }));
}

void unmaskedClientFrameIsAProtocolError(MdkrLanPartyServer &server) {
    TestClient client(server.port());
    completeUpgrade(client);
    client.send(maskedFrame(0x1u, "bare", true, /*masked=*/false));
    std::string carried;
    WsFrame frame;
    assert(readServerFrame(client, carried, frame));
    assert(frame.opcode == 0x8u);
    assert(closeCode(frame) == 1002u);
    assert(client.readToClose().empty());
}

void fragmentationIsRefusedPolitelyWith1003(MdkrLanPartyServer &server) {
    TestClient client(server.port());
    completeUpgrade(client);
    client.send(maskedFrame(0x1u, "part-one", /*fin=*/false, true));
    std::string carried;
    WsFrame frame;
    assert(readServerFrame(client, carried, frame));
    assert(frame.opcode == 0x8u);
    assert(closeCode(frame) == 1003u);
}

void oversizedDeclaredPayloadClosesWith1009(MdkrLanPartyServer &server) {
    TestClient client(server.port());
    completeUpgrade(client);
    /* Declare kMdkrLanPartyMaxWsPayloadBytes + 1 and send only the header:
     * the refusal must come from the declared length, before any payload
     * byte is buffered. */
    const size_t declared = kMdkrLanPartyMaxWsPayloadBytes + 1u;
    std::string header;
    header.push_back(static_cast<char>(0x81));
    header.push_back(static_cast<char>(0x80u | 126u));
    header.push_back(static_cast<char>((declared >> 8u) & 0xffu));
    header.push_back(static_cast<char>(declared & 0xffu));
    client.send(header);
    std::string carried;
    WsFrame frame;
    assert(readServerFrame(client, carried, frame));
    assert(frame.opcode == 0x8u);
    assert(closeCode(frame) == 1009u);
}

void boundedPayloadStillFits(MdkrLanPartyServer &server, WsProbe &probe) {
    const size_t before = probe.messageCount();
    TestClient client(server.port());
    completeUpgrade(client);
    assert(waitFor([&probe]() { return probe.hasSocket(); }));
    const std::string payload(kMdkrLanPartyMaxWsPayloadBytes, 'p');
    client.send(maskedFrame(0x1u, payload, true, true));
    assert(waitFor(
        [&probe, before]() { return probe.messageCount() > before; }));
    std::lock_guard<std::mutex> lock(probe.mutex);
    assert(probe.messages.back() == payload);
}

/*
 * Fix round 1, finding 3: DNS rebinding. A malicious internet page can
 * rebind its hostname to this host's LAN IP and speak same-origin plain
 * HTTP+WS to this server (no TLS mismatch to stop it; Safari has no
 * Private Network Access). The browser still sends the ATTACKER'S name in
 * Host, so the upgrade gate is: only the names this server actually
 * answers as (loopback + the machine's LAN IPs) may open /party-ws.
 */
void foreignHostCannotOpenThePartyWs(MdkrLanPartyServer &server) {
    const std::string port = std::to_string(server.port());
    const std::vector<std::string> allowed = {
        "127.0.0.1",
        "127.0.0.1:" + port,
        "localhost:" + port,
        "LOCALHOST:" + port, /* Host names are case-insensitive. */
    };
    for (const std::string &host : allowed) {
        TestClient client(server.port());
        client.send(upgradeRequest(host));
        const std::string reply = client.readUntil("\r\n\r\n");
        assert(reply.find("HTTP/1.1 101") == 0u);
    }
    const std::vector<std::string> foreign = {
        "evil.example",
        "evil.example:" + port,
        "127.0.0.1.evil.example",   /* prefix-lookalike */
        "127.0.0.1@evil.example",   /* userinfo smuggle shape */
        "localhost.evil.example",
        "127.0.0.1:80x",            /* non-numeric port tail */
        "",                          /* empty Host value */
    };
    for (const std::string &host : foreign) {
        TestClient client(server.port());
        client.send(upgradeRequest(host));
        const HttpResponse response = readResponse(client);
        assert(response.statusLine.find("HTTP/1.1 403") == 0u);
        /* Never a 101, and never an echo of the attacker's name. */
        assert(response.raw.find("evil.example") == std::string::npos);
        assert(client.readToClose().empty());
    }
    /* A request with no Host header at all is refused the same way. */
    {
        TestClient client(server.port());
        client.send(std::string("GET /party-ws HTTP/1.1\r\n") +
                    "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                    "Sec-WebSocket-Key: " +
                    kSampleKey + "\r\nSec-WebSocket-Version: 13\r\n\r\n");
        const HttpResponse response = readResponse(client);
        assert(response.statusLine.find("HTTP/1.1 403") == 0u);
    }
}

/*
 * The 101 must SELECT the server's own control subprotocol when the page offers
 * it (RFC 6455 4.2.2; Chromium fails the whole handshake if the client offered a
 * Sec-WebSocket-Protocol and the server names none) and must NEVER reflect a
 * foreign token -- the same non-reflection posture foreignHostCannotOpenThePartyWs
 * pins for the Host name. This locks that branch in milliseconds, instead of
 * leaving it guarded only by the Chrome+LAN-gated end-to-end lane.
 */
void subprotocolIsSelectedNotReflected(MdkrLanPartyServer &server) {
    /* Positive: the page offers exactly what it does over LAN redeem-over-ws
     * (controller.js sends ["gb-control-v1"]); the 101 echoes that token. */
    {
        TestClient client(server.port());
        client.send(upgradeRequestWithProtocol("gb-control-v1"));
        const HttpResponse response = readResponse(client);
        assert(response.statusLine.find("HTTP/1.1 101") == 0u);
        const auto selected = response.headers.find("sec-websocket-protocol");
        assert(selected != response.headers.end());
        assert(selected->second == "gb-control-v1");
    }
    /* Positive, mixed offer: the server picks its own supported token out of a
     * comma list and reflects none of the junk offered beside it. */
    {
        TestClient client(server.port());
        client.send(
            upgradeRequestWithProtocol("evil, gb-control-v1, other-junk"));
        const HttpResponse response = readResponse(client);
        assert(response.statusLine.find("HTTP/1.1 101") == 0u);
        const auto selected = response.headers.find("sec-websocket-protocol");
        assert(selected != response.headers.end());
        assert(selected->second == "gb-control-v1"); /* its own constant only */
        assert(response.raw.find("evil") == std::string::npos);
        assert(response.raw.find("other-junk") == std::string::npos);
    }
    /* Negative (the injection guard): a foreign-only offer is NOT reflected --
     * the server selects no subprotocol at all, never echoes the attacker's
     * token. It still upgrades; the browser makes its own accept decision. */
    {
        TestClient client(server.port());
        client.send(upgradeRequestWithProtocol("evil"));
        const HttpResponse response = readResponse(client);
        assert(response.statusLine.find("HTTP/1.1 101") == 0u);
        assert(response.headers.find("sec-websocket-protocol") ==
               response.headers.end());
        assert(response.raw.find("evil") == std::string::npos);
    }
    /* No offer at all: the response omits the header, matching the
     * completeUpgrade path every other 101 test exercises. */
    {
        TestClient client(server.port());
        client.send(upgradeRequest());
        const HttpResponse response = readResponse(client);
        assert(response.statusLine.find("HTTP/1.1 101") == 0u);
        assert(response.headers.find("sec-websocket-protocol") ==
               response.headers.end());
    }
}

/* Fix round 1, minor 4: a GET carrying a body would desync keep-alive
 * framing (the unread body bytes would parse as the next request head).
 * Content-Length: 0 stays accepted -- some HTTP libraries attach it. */
void getWithABodyIsRefusedNotReframed(MdkrLanPartyServer &server) {
    {
        TestClient client(server.port());
        client.send("GET /controller/index.html HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\nContent-Length: 5\r\n\r\nhello");
        const HttpResponse response = readResponse(client);
        assert(response.statusLine.find("HTTP/1.1 400") == 0u);
        assert(client.readToClose().empty()); /* closed, not resynced */
    }
    {
        TestClient client(server.port());
        client.send("GET /controller/index.html HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\nTransfer-Encoding: chunked\r\n\r\n"
                    "0\r\n\r\n");
        const HttpResponse response = readResponse(client);
        assert(response.statusLine.find("HTTP/1.1 400") == 0u);
    }
    {
        TestClient client(server.port());
        client.send("GET /controller/index.html HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\nContent-Length: 0\r\n\r\n");
        const HttpResponse response = readResponse(client);
        assert(response.statusLine.find("HTTP/1.1 200") == 0u);
    }
}

/* Post-upgrade idle reaper: a socket that upgrades and then goes silent (never
 * redeems, never pongs) must not hold its connection slot forever. The server
 * pings the idle socket, then closes it when no inbound activity follows. A raw
 * client that ignores the ping (as here) is reaped; a real browser pongs
 * automatically and stays alive, which is why an approval-waiting phone is safe.
 * Red-first: without the reaper this socket never receives a frame, so the read
 * below times out and the close assertion fails. */
void idleWebSocketIsReaped(MdkrLanPartyServer &server) {
    mdkr_lan_party_test_ws_idle_deadline_ms = 600u; /* ping ~300ms, close ~600ms */
    TestClient client(server.port());
    completeUpgrade(client);
    std::string carried;
    bool sawClose = false;
    for (int frames = 0; frames < 6 && !sawClose; frames++) {
        WsFrame frame;
        if (!readServerFrame(client, carried, frame)) break;
        if (frame.opcode == 0x8u) {
            assert(closeCode(frame) == 1000u);
            sawClose = true;
        }
        /* Otherwise a 0x9 ping the reaper sent first; keep reading. */
    }
    assert(sawClose);
    mdkr_lan_party_test_ws_idle_deadline_ms = 0u;
}

/* Fix round 1, minor 9: the peer's close code is not echoed back --
 * RFC 6455 7.4 reserves ranges, and the non-reflection rule wins: the
 * reply is always our own normal-closure 1000. */
void peerCloseCodeIsNotEchoedVerbatim(MdkrLanPartyServer &server) {
    TestClient client(server.port());
    completeUpgrade(client);
    std::string closePayload;
    closePayload.push_back(static_cast<char>(0x0f)); /* 3999: reserved- */
    closePayload.push_back(static_cast<char>(0x9f)); /* range garbage   */
    client.send(maskedFrame(0x8u, closePayload, true, true));
    std::string carried;
    WsFrame frame;
    assert(readServerFrame(client, carried, frame));
    assert(frame.opcode == 0x8u);
    assert(closeCode(frame) == 1000u);
    assert(client.readToClose().empty());
}

/*
 * Fix round 1, finding 2: slow drip. One byte per poll interval used to
 * dodge the idle deadline (checked only on the poll-timeout branch), so a
 * single hostile device could hold every slot for ~34 minutes at a time.
 * The deadline is anchored at the last COMPLETE request: dripping bytes is
 * not progress, so the head must arrive whole inside one deadline window.
 * Runs against the test-only shortened deadline; the enforcement path is
 * byte-identical to the product's.
 */
void slowDripCannotHoldAConnectionSlot(MdkrLanPartyServer &server) {
    mdkr_lan_party_test_http_deadline_ms = 1500u;
    TestClient client(server.port());
    client.send("GET /controller/index.html HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                "X-Drip: ");
    const auto start = std::chrono::steady_clock::now();
    bool cut = false;
    /* 50 ms per byte keeps every recv poll fed, which is exactly the shape
     * that used to starve the deadline check. 6 s cap >> 1.5 s deadline. */
    for (unsigned step = 0u; step < 120u; step++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!client.trySend("a")) {
            cut = true;
            break;
        }
    }
    mdkr_lan_party_test_http_deadline_ms = 0u;
    assert(cut);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    /* Cut at the deadline (plus send-failure detection lag), long before
     * the drip could ride to the 8 KB head cap. */
    assert(elapsed >= 1000 && elapsed < 5000);
    assert(client.readToClose().empty()); /* cut without a status line */
}

/*
 * Fix round 1, finding 1 (Critical): teardown must SEAL the socket -- latch
 * closeSent under sendMutex -- before the fd can be closed, or a sender
 * inside the send critical section can write onto a kernel-recycled fd
 * (cross-connection frame injection). Deterministic via the test-only park
 * hook: a sender is parked INSIDE the critical section, the client drops
 * abruptly (no close frame), and the assertion is that teardown CANNOT
 * complete (onClosed cannot fire) while the sender still holds the lock.
 * Pre-fix, teardown ignored sendMutex and onClosed fired while parked.
 */
std::atomic<bool> gParkArmed{false};
std::atomic<bool> gParkEntered{false};
std::atomic<bool> gParkRelease{false};

void parkOnceInsideSendCriticalSection() {
    if (!gParkArmed.exchange(false)) return;
    gParkEntered = true;
    while (!gParkRelease) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void abruptTeardownWaitsForTheInFlightSender(
    const MdkrLanPartyManifest &manifest) {
    MdkrLanPartyServer server;
    WsProbe probe;
    probe.attach(server);
    assert(server.start(0u, manifest));
    auto client = std::make_unique<TestClient>(server.port());
    completeUpgrade(*client);
    assert(waitFor([&probe]() { return probe.hasSocket(); }));
    std::shared_ptr<MdkrLanPartyWebSocket> socket;
    {
        std::lock_guard<std::mutex> lock(probe.mutex);
        socket = probe.socket;
    }

    gParkEntered = false;
    gParkRelease = false;
    mdkr_lan_party_test_send_hook = parkOnceInsideSendCriticalSection;
    gParkArmed = true;
    std::thread sender([socket]() { (void)socket->sendText("late-frame"); });
    assert(waitFor([]() { return gParkEntered.load(); }));

    /* Abrupt client exit: TCP close with NO WebSocket close frame. */
    client.reset();

    /* The reader sees EOF at once; the ONLY thing that may hold its
     * teardown back is the sealed ordering waiting on sendMutex. */
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    assert(!probe.wasClosed()); /* pre-fix: already closed here -> red */

    gParkRelease = true;
    sender.join();
    assert(waitFor([&probe]() { return probe.wasClosed(); }));
    assert(!socket->isOpen());
    assert(!socket->sendText("after-close")); /* sealed for good */
    mdkr_lan_party_test_send_hook = nullptr;
    server.stop();
}

void stopWithALiveWebSocketReturnsAndTearsDown(
    const MdkrLanPartyManifest &manifest) {
    MdkrLanPartyServer server;
    WsProbe probe;
    probe.attach(server);
    assert(server.start(0u, manifest));
    const uint16_t port = server.port();
    TestClient client(port);
    completeUpgrade(client);
    assert(waitFor([&probe]() { return probe.hasSocket(); }));
    server.stop(); /* Must join every thread; a hang here trips the ctest
                    * timeout backstop rather than passing. */
    assert(client.readToClose().empty() || true); /* Connection is gone. */
    assert(waitFor([&probe]() { return probe.wasClosed(); }));
}

} // namespace

int main() {
    contentTypeTableCoversThePackagedKinds();
    startBindsAnEphemeralPortAndStops();

    const MdkrLanPartyManifest manifest = testManifest();
    MdkrLanPartyServer server;
    WsProbe probe;
    probe.attach(server);
    assert(server.start(0u, manifest));

    manifestAssetsServeByteIdenticalWithTheHeaderTable(server, manifest);
    keepAliveServesASecondRequestOnTheSameConnection(server);
    traversalShapesAll404WithoutEchoingTheRequest(server);
    nonGetIsRefusedWith405(server);
    oversizedRequestLineClosesWithoutAResponse(server);
    plainGetOnThePartyWsPathDoesNotUpgrade(server);
    upgradeMatchesTheRfcSampleAndRoundTripsMessages(server, probe);
    unmaskedClientFrameIsAProtocolError(server);
    fragmentationIsRefusedPolitelyWith1003(server);
    oversizedDeclaredPayloadClosesWith1009(server);
    boundedPayloadStillFits(server, probe);

    /* Fix round 1 regressions. */
    foreignHostCannotOpenThePartyWs(server);
    subprotocolIsSelectedNotReflected(server);
    getWithABodyIsRefusedNotReframed(server);
    peerCloseCodeIsNotEchoedVerbatim(server);
    idleWebSocketIsReaped(server);
    slowDripCannotHoldAConnectionSlot(server);
    server.stop();

    abruptTeardownWaitsForTheInFlightSender(manifest);
    stopWithALiveWebSocketReturnsAndTearsDown(manifest);
    std::fprintf(stderr, "test_lan_party_server: all cases passed\n");
    return 0;
}
