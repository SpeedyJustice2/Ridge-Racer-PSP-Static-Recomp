#include "rr_http.hpp"

#include <cstdio>
#include <cstdlib>

#if defined(__EMSCRIPTEN__)
// Browser: a synchronous XMLHttpRequest.  Only ever called on the network helper thread (a Web Worker of its own), where
// blocking is allowed.  The room service answers with "Access-Control-Allow-Origin: *", which the page's cross-origin
// isolation needs.
#include <emscripten.h>
EM_JS_DEPS(rr_http, "$stringToNewUTF8,$UTF8ToString");
EM_JS(char *, rr_xhr, (const char *method, const char *url, int timeout_ms, int *status), {
    var st = 0, body = '';
    try {
        var x = new XMLHttpRequest();
        x.open(UTF8ToString(method), UTF8ToString(url), false);
        try { x.timeout = timeout_ms; } catch (e) {}   // (not allowed for synchronous requests in some browsers)
        x.send();
        st = x.status; body = x.responseText || '';
    } catch (e) { st = 0; }
    HEAP32[status >> 2] = st;
    return stringToNewUTF8(body);
});
namespace rr::net {
HttpResult http_request(const std::string &method, const std::string &url, int timeout_ms) {
    HttpResult r;
    char *body = rr_xhr(method.c_str(), url.c_str(), timeout_ms, &r.status);
    if (body != nullptr) { r.body = body; std::free(body); }
    return r;
}
}
#elif defined(__ANDROID__)
// no implementation yet: Android needs a JNI bridge
namespace rr::net {
HttpResult http_request(const std::string &, const std::string &, int) { return {}; }
}
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <vector>
namespace rr::net {
HttpResult http_request(const std::string &method, const std::string &url, int timeout_ms) {
    HttpResult out;
    std::wstring wurl(url.begin(), url.end());   // the URLs are plain ASCII
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2047;
    uc.dwSchemeLength = 1;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return out;
    HINTERNET session = WinHttpOpen(L"RidgeRacerPort/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return out;
    WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
    HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (conn) {
        const DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        std::wstring wmethod(method.begin(), method.end());
        HINTERNET req = WinHttpOpenRequest(conn, wmethod.c_str(), path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (req) {
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
                DWORD status = 0, size = sizeof status;
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
                out.status = static_cast<int>(status);
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
                    std::vector<char> buf(avail);
                    DWORD got = 0;
                    if (!WinHttpReadData(req, buf.data(), avail, &got) || got == 0) break;
                    out.body.append(buf.data(), got);
                    if (out.body.size() > 65536) break;
                }
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(conn);
    }
    WinHttpCloseHandle(session);
    return out;
}
}
#else
namespace rr::net {
HttpResult http_request(const std::string &method, const std::string &url, int timeout_ms) {
    HttpResult out;
    for (const char c : url + method)   // we build these strings ourselves; refuse anything that could break out of the shell quoting
        if (c == '\'' || c == '\\' || c == '`' || c == '$' || c == '\n' || c == '"' || c == ' ') return out;
    const int secs = timeout_ms / 1000 > 0 ? timeout_ms / 1000 : 1;
    const std::string cmd = "curl -s -m " + std::to_string(secs) + " -X " + method + " -w '\\n%{http_code}' '" + url + "' 2>/dev/null";
    FILE *p = popen(cmd.c_str(), "r");
    if (p == nullptr) return out;
    char buf[1024];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0 && out.body.size() < 65536) out.body.append(buf, n);
    pclose(p);
    const auto nl = out.body.rfind('\n');
    if (nl == std::string::npos) { out.body.clear(); return out; }
    out.status = std::atoi(out.body.c_str() + nl + 1);
    out.body.resize(nl);
    return out;
}
}
#endif
