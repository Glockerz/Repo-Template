// net/client.h — WinHTTP, called straight from the `request()` closure.
//
// There is no loopback HTTP server, no /req proxy, no HttpService, no
// RequestInternal: the game process never listens on anything. Requests run on a
// worker thread (never on the drain thread, which must stay non-blocking), and
// the async variants hand their result back through a scheduler CallInto job.
//
// WinHTTP is used rather than WinINet because it is contractible per-request
// (timeouts, proxies, TLS behaviour) and does not share the process's cookie
// jar with the client's own traffic.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace phetamine::net {

struct Request {
    std::string url;
    std::string method = "GET";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    bool followRedirects = true;
    uint32_t timeoutMs = 30'000;
};

struct Response {
    bool ok = false;
    uint32_t status = 0;                 // HTTP status when ok, or Win32 error
    std::string statusMessage;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string error;                   // populated when ok == false
};

// Blocking. Safe to call from any thread; never call it from the drain.
Response Send(const Request& request);

// Convenience wrappers used by the httpget/httppost family.
Response Get(const std::string& url, uint32_t timeoutMs = 30'000);
Response Post(const std::string& url, const std::string& body, uint32_t timeoutMs = 30'000);

// Proxy configuration, applied to every request. Empty ⇒ WinHTTP defaults.
void SetProxy(const std::string& proxy);

// `WinHttpWebSocket*` support lives behind this flag: the API exists, but a
// websocket that outlives the request needs a lifetime owner in the scheduler,
// so it is not exposed to scripts yet (docs/UNC_COVERAGE.md Tier 2).
bool WebSocketSupported();

}  // namespace phetamine::net
