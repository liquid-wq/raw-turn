#include "core/http.h"

#include <windows.h>
#include <winhttp.h>

#include <cstdio>
#include <vector>

#include "core/i18n.h"

namespace core {
namespace {

const wchar_t* kAgent = L"SaturnRA/1.0";

std::wstring NachWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

struct Schliesser {
    HINTERNET h = nullptr;
    ~Schliesser() { if (h) ::WinHttpCloseHandle(h); }
};

}  // namespace

std::string UrlKodieren(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string aus;
    aus.reserve(s.size() * 3 / 2);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            aus.push_back((char)c);
        } else {
            aus.push_back('%');
            aus.push_back(hex[c >> 4]);
            aus.push_back(hex[c & 0x0F]);
        }
    }
    return aus;
}

void FormularFeld(std::string& formular, const std::string& name,
                  const std::string& wert) {
    if (!formular.empty()) formular += '&';
    formular += UrlKodieren(name);
    formular += '=';
    formular += UrlKodieren(wert);
}

namespace {

HttpAntwort Anfrage(const wchar_t* methode, const std::string& host,
                    const std::string& pfad, const std::string& formular,
                    int timeout_ms);

}  // namespace

HttpAntwort HttpPost(const std::string& host, const std::string& pfad,
                     const std::string& formular, int timeout_ms) {
    return Anfrage(L"POST", host, pfad, formular, timeout_ms);
}

HttpAntwort HttpGet(const std::string& host, const std::string& pfad, int timeout_ms) {
    return Anfrage(L"GET", host, pfad, std::string(), timeout_ms);
}

namespace {

HttpAntwort Anfrage(const wchar_t* methode, const std::string& host,
                    const std::string& pfad, const std::string& formular,
                    int timeout_ms) {
    HttpAntwort a;

    Schliesser sitzung;
    sitzung.h = ::WinHttpOpen(kAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!sitzung.h) { a.fehler = T("WinHttpOpen fehlgeschlagen"); return a; }
    ::WinHttpSetTimeouts(sitzung.h, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    Schliesser verbindung;
    verbindung.h = ::WinHttpConnect(sitzung.h, NachWide(host).c_str(),
                                    INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!verbindung.h) {
        char t[300];
        std::snprintf(t, sizeof(t), T("Verbindung zu %s fehlgeschlagen"), host.c_str());
        a.fehler = t;
        return a;
    }

    Schliesser anfrage;
    anfrage.h = ::WinHttpOpenRequest(verbindung.h, methode, NachWide(pfad).c_str(),
                                     nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     WINHTTP_FLAG_SECURE);
    if (!anfrage.h) { a.fehler = T("Anfrage konnte nicht angelegt werden"); return a; }

    const bool post = formular.size() > 0 || ::wcscmp(methode, L"POST") == 0;
    const wchar_t* kopf = post ? L"Content-Type: application/x-www-form-urlencoded\r\n"
                               : WINHTTP_NO_ADDITIONAL_HEADERS;
    if (!::WinHttpSendRequest(anfrage.h, kopf, post ? (DWORD)-1 : 0,
                              post ? (LPVOID)formular.data() : WINHTTP_NO_REQUEST_DATA,
                              (DWORD)formular.size(), (DWORD)formular.size(), 0)) {
        char t[128];
        std::snprintf(t, sizeof(t), T("Senden fehlgeschlagen (Fehler %lu)"), ::GetLastError());
        a.fehler = t;
        return a;
    }
    if (!::WinHttpReceiveResponse(anfrage.h, nullptr)) {
        char t[128];
        std::snprintf(t, sizeof(t), T("Keine Antwort (Fehler %lu)"), ::GetLastError());
        a.fehler = t;
        return a;
    }

    DWORD status = 0, groesse = sizeof(status);
    ::WinHttpQueryHeaders(anfrage.h,
                          WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &groesse,
                          WINHTTP_NO_HEADER_INDEX);
    a.status = (int)status;

    if (status == 429) {
        wchar_t wert[32] = {};
        DWORD wg = sizeof(wert);
        if (::WinHttpQueryHeaders(anfrage.h, WINHTTP_QUERY_CUSTOM,
                                  L"Retry-After", wert, &wg, WINHTTP_NO_HEADER_INDEX))
            a.wiederholen_nach = ::_wtoi(wert);
    }

    for (;;) {
        DWORD verfuegbar = 0;
        if (!::WinHttpQueryDataAvailable(anfrage.h, &verfuegbar) || verfuegbar == 0) break;
        std::vector<char> puffer(verfuegbar);
        DWORD gelesen = 0;
        if (!::WinHttpReadData(anfrage.h, puffer.data(), verfuegbar, &gelesen)) break;
        a.koerper.append(puffer.data(), gelesen);
    }

    a.ok = (status >= 200 && status < 300);
    if (!a.ok && a.fehler.empty()) {
        char t[96];
        std::snprintf(t, sizeof(t), "HTTP %d", a.status);
        a.fehler = t;
    }
    return a;
}

}  // namespace
}  // namespace core
