/*
 * Embedded LAN HTTP + WebSocket server for local-only Phone Party.
 *
 * ONE port carries both jobs: plain HTTP/1.1 GET for the packaged
 * controller assets and the RFC 6455 upgrade on /party-ws. A single port
 * is what keeps the page's CSP 'self' semantics honest for the ws:
 * endpoint and costs the player exactly one firewall prompt.
 *
 * Security is by construction, not by filtering: every request resolves
 * against an in-memory manifest (exact path -> bytes + content type)
 * frozen at start(). There is no filesystem access at request time, so no
 * traversal shape -- encoded, overlong, backslashed or otherwise -- can
 * name anything the manifest does not already contain.
 *
 * Threading contract, matching the transport rules the rest of Phone
 * Party lives by (MdkrPartyTransport in native_party_host.h and the
 * libdatachannel transport behind it): the server owns an accept thread
 * and one bounded thread per connection; start()/stop()/port() are
 * launcher-thread calls and never serve a request themselves. The
 * onWebSocket / onMessage / onClosed callbacks fire on server-owned
 * connection threads -- a consumer must do what the transport does with
 * libdatachannel's callbacks: copy into its own bounded queue and return,
 * never call back into launcher-side models. stop() is the one blocking
 * call: it joins every thread, each of which wakes on a short bounded
 * interval, so teardown is prompt and deterministic.
 */
#ifndef MDKR_LAN_PARTY_SERVER_H
#define MDKR_LAN_PARTY_SERVER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

/*
 * Upper bound for one WebSocket payload on /party-ws, inbound and
 * outbound alike. This is the party control-message bound -- the same
 * value the WebRTC transport enforces per control message
 * (libdatachannel_party_transport.cpp, kMaxControlBytes): the same phones
 * speak the same protocol over this socket, so a larger frame is never
 * legitimate and is refused from its declared length alone.
 */
inline constexpr size_t kMdkrLanPartyMaxWsPayloadBytes = 4096u;

/* An asset frozen into the manifest at start(): exact bytes plus the full
 * Content-Type value emitted verbatim on its 200. */
struct MdkrLanPartyAsset {
    std::vector<uint8_t> bytes;
    std::string contentType;
};

/* Exact request path (query stripped) -> asset. Built by the caller from
 * the packaged dist/web controller assets; this server never discovers
 * files itself. */
using MdkrLanPartyManifest = std::map<std::string, MdkrLanPartyAsset>;

/*
 * Content-Type values for the asset kinds the packaged controller page
 * ships (html/js/css/png/json/svg/webmanifest). Manifest builders feed
 * this through MdkrLanPartyAsset::contentType so the MIME strings live
 * exactly once.
 */
inline const char *mdkr_lan_party_content_type(const std::string &path) {
    struct Kind {
        const char *suffix;
        const char *value;
    };
    static constexpr Kind kKinds[] = {
        {".html", "text/html; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".png", "image/png"},
        {".json", "application/json"},
        {".svg", "image/svg+xml"},
        {".webmanifest", "application/manifest+json"},
    };
    for (const Kind &kind : kKinds) {
        const size_t suffixLength = std::string(kind.suffix).size();
        if (path.size() >= suffixLength &&
            path.compare(path.size() - suffixLength, suffixLength,
                         kind.suffix) == 0) {
            return kind.value;
        }
    }
    return "application/octet-stream";
}

/*
 * Every IPv4 address this machine answers on (loopback included). This is the
 * exact set the server freezes into its /party-ws Host allowlist at start(), so
 * a local-play invite that advertises one of these hosts (lan_party_launch.h)
 * is always a host the socket will accept. Enumerates getifaddrs on POSIX and
 * gethostname/getaddrinfo on Windows; the order is the OS enumeration order.
 */
std::vector<std::string> mdkr_lan_party_machine_ipv4_addresses();

#ifdef MDKR_LAN_PARTY_TESTING
/*
 * Test-only seams, compiled solely into the unit test: cmake/tests.cmake
 * defines MDKR_LAN_PARTY_TESTING there and nowhere else, so no shipped
 * build contains the pointer, the call site, or the override (the
 * MDKR_A11Y_SPEECH_TESTING pattern). The send hook runs INSIDE the
 * frame-send critical section, which is what makes the teardown-vs-send
 * ordering deterministically testable instead of a scheduler lottery; the
 * deadline override lets the slow-drip regression run in seconds instead
 * of the product deadline.
 */
extern void (*mdkr_lan_party_test_send_hook)();
extern unsigned mdkr_lan_party_test_http_deadline_ms; /* 0 = product value */
/* Post-upgrade idle-reaper close deadline; the ping fires at half of it. 0 =
 * product value. Lets the reaper test run in a second, not 45. */
extern unsigned mdkr_lan_party_test_ws_idle_deadline_ms;
#endif

struct MdkrLanPartyWsState; /* Internal; defined in lan_party_server.cpp. */

/*
 * One upgraded /party-ws connection. Handed to the onWebSocket callback on
 * the connection's own thread, before any frame is parsed, so callbacks
 * attached inside that callback can never miss a message. sendText() and
 * close() are safe from any thread; frames above the payload bound are
 * refused. onMessage receives text and binary payloads alike, verbatim.
 */
class MdkrLanPartyWebSocket {
public:
    explicit MdkrLanPartyWebSocket(std::shared_ptr<MdkrLanPartyWsState> state);
    ~MdkrLanPartyWebSocket();

    MdkrLanPartyWebSocket(const MdkrLanPartyWebSocket &) = delete;
    MdkrLanPartyWebSocket &operator=(const MdkrLanPartyWebSocket &) = delete;

    void onMessage(std::function<void(const std::string &)> callback);
    void onClosed(std::function<void()> callback);

    bool sendText(const std::string &payload);
    /* Close with an optional RFC 6455 reason string (UTF-8, truncated to the
     * 123-byte control-frame budget). The reason reaches the browser as the
     * WebSocket close event's `reason`, which the controller page reads to show a
     * terminal state (e.g. host_closed) from the frame alone, without a
     * re-redeem. Sent synchronously before the write side is half-closed. */
    void close(uint16_t code = 1000u, const std::string &reason = std::string());
    bool isOpen() const;

private:
    std::shared_ptr<MdkrLanPartyWsState> state_;
};

struct MdkrLanPartyServerState; /* Internal; defined in lan_party_server.cpp. */

class MdkrLanPartyServer {
public:
    MdkrLanPartyServer();
    ~MdkrLanPartyServer();

    MdkrLanPartyServer(const MdkrLanPartyServer &) = delete;
    MdkrLanPartyServer &operator=(const MdkrLanPartyServer &) = delete;

    /* Register the /party-ws consumer. Set it before start(); the callback
     * fires on connection threads (see the threading contract above). An
     * upgrade that arrives while no consumer is registered is refused. */
    void onWebSocket(
        std::function<void(std::shared_ptr<MdkrLanPartyWebSocket>)> callback);

    /* Bind (port 0 = ephemeral), freeze the manifest, and begin serving.
     * Returns false if already running or the socket cannot bind. */
    bool start(uint16_t port, MdkrLanPartyManifest manifest);

    /* Idempotent. Joins the accept thread and every connection thread;
     * live WebSockets observe their onClosed. The instance is restartable
     * afterwards. */
    void stop();

    /* The bound port while running, 0 otherwise. */
    uint16_t port() const;

private:
    std::shared_ptr<MdkrLanPartyServerState> state_;
};

#endif /* MDKR_LAN_PARTY_SERVER_H */
