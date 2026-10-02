// (The old bridge design had this split across winhttp_client.h + a proxy
// endpoint. The proxy is gone; the WinHTTP surface lives here and is called
// directly from the request() closure — see docs/DECISIONS.md ADR-2/ADR-3.)
#include "net/client.h"

#include <windows.h>
#include <winhttp.h>

#include <cstring>

#pragma comment(lib, "winhttp.lib")

namespace phetamine::net {
namespace {

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), size);
    return out;
}

std::string Narrow(const std::wstring& s) {
    if (s.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), size, nullptr, nullptr);
    return out;
}

struct Url {
    std::wstring scheme;
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
    bool valid = false;
};

Url Crack(const std::string& raw) {
    Url url{};
    const std::wstring wide = Widen(raw);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256]{};
    wchar_t path[2048]{};
    wchar_t scheme[16]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    parts.lpszScheme = scheme;
    parts.dwSchemeLength = static_cast<DWORD>(std::size(scheme));

    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts)) return url;
    url.scheme.assign(scheme);
    url.host.assign(host);
    url.path.assign(path);
    url.port = parts.nPort;
    url.secure = (parts.nScheme == INTERNET_SCHEME_HTTPS);
    url.valid = !url.host.empty();
    return url;
}

std::string ProxyString;
CRITICAL_SECTION ProxyLock;
bool ProxyLockReady = false;

void EnsureProxyLock() {
    if (!ProxyLockReady) {
        InitializeCriticalSection(&ProxyLock);
        ProxyLockReady = true;
    }
}

// Translate a WinHTTP error into something a script author can act on.
std::string Explain(DWORD error) {
    switch (error) {
        case ERROR_WINHTTP_TIMEOUT:              return "request timed out";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:    return "could not resolve the host";
        case ERROR_WINHTTP_CANNOT_CONNECT:       return "connection refused";
        case ERROR_WINHTTP_SECURE_FAILURE:       return "TLS verification failed";
        case ERROR_WINHTTP_INVALID_URL:          return "invalid URL";
        case ERROR_WINHTTP_REDIRECT_FAILED:      return "redirect failed";
        default: {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "winhttp error %lu", error);
            return buffer;
        }
    }
}

}  // namespace

Response Send(const Request& request) {
    Response response{};
    const Url url = Crack(request.url);
    if (!url.valid) {
        response.error = "invalid URL";
        response.status = ERROR_WINHTTP_INVALID_URL;
        return response;
    }

    HINTERNET session = WinHttpOpen(L"PHETAMINE/1.0",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        response.error = Explain(GetLastError());
        return response;
    }
    WinHttpSetTimeouts(session, request.timeoutMs, request.timeoutMs,
                       request.timeoutMs, request.timeoutMs);

    EnsureProxyLock();
    EnterCriticalSection(&ProxyLock);
    const std::string proxy = ProxyString;
    LeaveCriticalSection(&ProxyLock);
    if (!proxy.empty()) {
        WinHttpSetOption(session, WINHTTP_OPTION_PROXY,
                         const_cast<char*>(proxy.c_str()), static_cast<DWORD>(proxy.size()));
    }

    HINTERNET connection = WinHttpConnect(session, url.host.c_str(), url.port, 0);
    if (!connection) {
        response.error = Explain(GetLastError());
        WinHttpCloseHandle(session);
        return response;
    }

    const std::wstring method = Widen(request.method);
    HINTERNET req = WinHttpOpenRequest(connection, method.c_str(), url.path.c_str(),
                                       nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       url.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req) {
        response.error = Explain(GetLastError());
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return response;
    }

    DWORD redirects = request.followRedirects ? WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS
                                              : WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));

    std::wstring headers;
    for (const auto& [key, value] : request.headers) {
        headers += Widen(key + ": " + value + "\r\n");
    }

    const LPVOID body = request.body.empty() ? WINHTTP_NO_REQUEST_DATA
                                             : const_cast<char*>(request.body.data());
    const DWORD bodyLength = static_cast<DWORD>(request.body.size());

    if (!WinHttpSendRequest(req,
                            headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : static_cast<DWORD>(headers.size()),
                            body, bodyLength, bodyLength, 0)) {
        response.error = Explain(GetLastError());
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return response;
    }

    if (!WinHttpReceiveResponse(req, nullptr)) {
        response.error = Explain(GetLastError());
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return response;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    response.status = status;

    wchar_t statusText[256]{};
    DWORD statusTextSize = sizeof(statusText);
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_TEXT, WINHTTP_HEADER_NAME_BY_INDEX,
                            statusText, &statusTextSize, WINHTTP_NO_HEADER_INDEX)) {
        response.statusMessage = Narrow(statusText);
    }

    // headers
    DWORD headerSize = 0;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                        nullptr, &headerSize, WINHTTP_NO_HEADER_INDEX);
    if (headerSize) {
        std::wstring raw(headerSize / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                                raw.data(), &headerSize, WINHTTP_NO_HEADER_INDEX)) {
            const std::string text = Narrow(raw);
            size_t start = 0;
            while (start < text.size()) {
                const size_t end = text.find("\r\n", start);
                if (end == std::string::npos) break;
                const std::string line = text.substr(start, end - start);
                start = end + 2;
                const size_t colon = line.find(':');
                if (colon == std::string::npos) continue;
                response.headers[line.substr(0, colon)] = line.substr(colon + 2);
            }
        }
    }

    // body
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(req, &available) || available == 0) break;
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(req, chunk.data(), available, &read) || read == 0) break;
        chunk.resize(read);
        response.body += chunk;
        if (response.body.size() > (64u << 20)) {
            response.error = "response exceeded the 64 MiB cap";
            break;
        }
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    response.ok = response.error.empty();
    return response;
}

Response Get(const std::string& url, uint32_t timeoutMs) {
    Request request{};
    request.url = url;
    request.timeoutMs = timeoutMs;
    return Send(request);
}

Response Post(const std::string& url, const std::string& body, uint32_t timeoutMs) {
    Request request{};
    request.url = url;
    request.method = "POST";
    request.body = body;
    request.timeoutMs = timeoutMs;
    request.headers.emplace_back("Content-Type", "application/x-www-form-urlencoded");
    return Send(request);
}

void SetProxy(const std::string& proxy) {
    EnsureProxyLock();
    EnterCriticalSection(&ProxyLock);
    ProxyString = proxy;
    LeaveCriticalSection(&ProxyLock);
}

bool WebSocketSupported() {
    // The API is present in WinHTTP; the missing piece is a lifetime owner in the
    // scheduler, which is why scripts do not get `websocket.connect` yet.
    return false;
}

}  // namespace phetamine::net
