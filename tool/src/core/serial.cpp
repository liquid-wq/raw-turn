#include "core/serial.h"

#include <windows.h>
#include <setupapi.h>
#include <devguid.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "core/i18n.h"
#include "core/protocol.h"

namespace core {
namespace {

struct Bekannt { uint16_t vid, pid; const char* name; };

// USB-Seriell-Wandler nach Haeufigkeit. Verbreitet sind Adapter mit CH340.
const Bekannt kAdapter[] = {
    { 0x1A86, 0x7523, "CH340" },
    { 0x1A86, 0x5523, "CH341" },
    { 0x1A86, 0x55D4, "CH9102" },
    { 0x0403, 0x6001, "FTDI FT232R" },
    { 0x0403, 0x6015, "FTDI FT231X" },
    { 0x10C4, 0xEA60, "CP210x" },
    { 0x067B, 0x2303, "Prolific PL2303" },
};

// Woerter aus echten Mitschnitten der SAROO-Ausgabe.
const char* const kSarooHinweise[] = {
    "stm32>", "play_task", "fad_start", "seek_cd",
    "buffer full", "abort_file", "init_cdblock",
};

std::string NachUtf8(const wchar_t* w) {
    if (!w) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s((size_t)n - 1, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::string KleinBuchstaben(std::string s) {
    for (char& c : s) c = (char)::tolower((unsigned char)c);
    return s;
}

// "USB\VID_1A86&PID_7523\..." -> vid/pid
void LiesVidPid(const std::string& hardware_id, uint16_t* vid, uint16_t* pid) {
    const std::string s = KleinBuchstaben(hardware_id);
    const size_t v = s.find("vid_");
    const size_t p = s.find("pid_");
    if (v != std::string::npos) *vid = (uint16_t)std::strtoul(s.substr(v + 4, 4).c_str(), nullptr, 16);
    if (p != std::string::npos) *pid = (uint16_t)std::strtoul(s.substr(p + 4, 4).c_str(), nullptr, 16);
}

}  // namespace

std::vector<PortInfo> OrdnePorts(const std::string& bevorzugt) {
    std::vector<PortInfo> aus;

    HDEVINFO info = ::SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr,
                                           DIGCF_PRESENT);
    if (info == INVALID_HANDLE_VALUE) return aus;

    SP_DEVINFO_DATA daten = {};
    daten.cbSize = sizeof(daten);
    for (DWORD i = 0; ::SetupDiEnumDeviceInfo(info, i, &daten); ++i) {
        // Portname steht im Geraeteschluessel unter "PortName".
        HKEY key = ::SetupDiOpenDevRegKey(info, &daten, DICS_FLAG_GLOBAL, 0,
                                          DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) continue;
        wchar_t name[64] = {};
        DWORD typ = 0, groesse = sizeof(name);
        const LONG r = ::RegQueryValueExW(key, L"PortName", nullptr, &typ,
                                          (LPBYTE)name, &groesse);
        ::RegCloseKey(key);
        if (r != ERROR_SUCCESS || name[0] == 0) continue;

        PortInfo p;
        p.geraet = NachUtf8(name);

        wchar_t puffer[512] = {};
        if (::SetupDiGetDeviceRegistryPropertyW(info, &daten, SPDRP_FRIENDLYNAME,
                                                nullptr, (PBYTE)puffer, sizeof(puffer), nullptr))
            p.beschreibung = NachUtf8(puffer);

        puffer[0] = 0;
        if (::SetupDiGetDeviceRegistryPropertyW(info, &daten, SPDRP_HARDWAREID,
                                                nullptr, (PBYTE)puffer, sizeof(puffer), nullptr))
            LiesVidPid(NachUtf8(puffer), &p.vid, &p.pid);

        for (const Bekannt& b : kAdapter)
            if (b.vid == p.vid && b.pid == p.pid) { p.adapter = b.name; break; }

        if (!p.adapter.empty())  p.punkte += 50;
        else if (p.vid != 0)     p.punkte += 20;   // irgendein USB-Geraet
        if (KleinBuchstaben(p.beschreibung).find("bluetooth") != std::string::npos)
            p.punkte -= 40;                        // nie gemeint
        if (!bevorzugt.empty() && p.geraet == bevorzugt) p.punkte += 100;

        aus.push_back(std::move(p));
    }
    ::SetupDiDestroyDeviceInfoList(info);

    std::sort(aus.begin(), aus.end(), [](const PortInfo& a, const PortInfo& b) {
        if (a.punkte != b.punkte) return a.punkte > b.punkte;
        return a.geraet < b.geraet;
    });
    return aus;
}

const char* SicherheitText(Sicherheit s) {
    switch (s) {
        case Sicherheit::Frames:    return T("gültige Frames empfangen");
        case Sicherheit::Text:      return T("SAROO-Klartext empfangen");
        case Sicherheit::Bytes:     return T("Bytes empfangen, nicht zuordenbar");
        case Sicherheit::Vermutung: return T("Vermutung, nichts gehört");
        default:                    return T("still");
    }
}

namespace {

Sicherheit HoereHin(const std::string& geraet, int baud, int ms) {
    SerialPort port;
    if (!port.Oeffnen(geraet, baud)) return Sicherheit::Nichts;

    FrameParser parser;
    std::string roh;
    const DWORD ende = ::GetTickCount() + (DWORD)ms;
    uint8_t puffer[512];
    bool etwas = false;

    while (::GetTickCount() < ende) {
        const int n = port.Lesen(puffer, sizeof(puffer), 100);
        if (n <= 0) continue;
        etwas = true;
        if (roh.size() < 1024) roh.append((const char*)puffer, (size_t)n);
        if (!parser.Fuettern(puffer, (size_t)n).empty()) return Sicherheit::Frames;
        for (const char* h : kSarooHinweise)
            if (roh.find(h) != std::string::npos) return Sicherheit::Text;
    }
    return etwas ? Sicherheit::Bytes : Sicherheit::Nichts;
}

int Rang(Sicherheit s) {
    switch (s) {
        case Sicherheit::Frames: return 3;
        case Sicherheit::Text:   return 2;
        case Sicherheit::Bytes:  return 1;
        default:                 return 0;
    }
}

}  // namespace

Suchergebnis SuchePort(const std::string& bevorzugt, int baud, int ms_je_port,
                       void (*fortschritt)(const char*, void*), void* benutzerdaten) {
    Suchergebnis erg;
    erg.alle = OrdnePorts(bevorzugt);
    if (erg.alle.empty()) return erg;

    int bester = -1;
    Sicherheit beste = Sicherheit::Nichts;

    for (size_t i = 0; i < erg.alle.size(); ++i) {
        if (fortschritt) fortschritt(erg.alle[i].geraet.c_str(), benutzerdaten);
        const Sicherheit s = HoereHin(erg.alle[i].geraet, baud, ms_je_port);
        if (s == Sicherheit::Frames) {
            erg.gefunden = true;
            erg.port = erg.alle[i];
            erg.sicherheit = s;
            erg.erlaeuterung = SicherheitText(s);
            return erg;                      // besser wird es nicht
        }
        if (Rang(s) > Rang(beste)) { beste = s; bester = (int)i; }
    }

    erg.gefunden = true;
    if (bester >= 0) {
        erg.port = erg.alle[(size_t)bester];
        erg.sicherheit = beste;
        erg.erlaeuterung = SicherheitText(beste);
    } else {
        // Nichts gehoert. Stille ist kein Fehler: ohne laufendes Spiel oder
        // ohne installierten Hook sendet das Geraet nichts.
        erg.port = erg.alle.front();
        erg.sicherheit = Sicherheit::Vermutung;
        erg.erlaeuterung = T("Vermutung, nichts gehört. Wahrscheinlichster Anschluss "
                             "nach USB-Kennung.");
    }
    return erg;
}

// ---------------------------------------------------------------------------

SerialPort::~SerialPort() { Schliessen(); }

bool SerialPort::Oeffnen(const std::string& geraet, int baud, std::string* fehler) {
    Schliessen();

    // Ab COM10 ist die \\.\-Schreibweise Pflicht.
    std::string pfad = "\\\\.\\" + geraet;
    HANDLE h = ::CreateFileA(pfad.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (fehler) {
            char t[128];
            std::snprintf(t, sizeof(t), T("Anschluss %s nicht zu öffnen (Fehler %lu)"),
                          geraet.c_str(), ::GetLastError());
            *fehler = t;
        }
        return false;
    }

    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    if (!::GetCommState(h, &dcb)) { ::CloseHandle(h); return false; }
    dcb.BaudRate = (DWORD)baud;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary  = TRUE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    if (!::SetCommState(h, &dcb)) {
        if (fehler) *fehler = T("Baudrate oder Rahmenformat nicht setzbar");
        ::CloseHandle(h);
        return false;
    }

    // Nicht blockierend: zurueckkehren, sobald etwas da ist.
    COMMTIMEOUTS t = {};
    t.ReadIntervalTimeout = MAXDWORD;
    t.ReadTotalTimeoutMultiplier = 0;
    t.ReadTotalTimeoutConstant = 0;
    t.WriteTotalTimeoutConstant = 200;
    ::SetCommTimeouts(h, &t);

    ::SetupComm(h, 1 << 16, 1 << 12);
    ::PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

    handle_ = h;
    return true;
}

void SerialPort::Schliessen() {
    if (handle_) { ::CloseHandle((HANDLE)handle_); handle_ = nullptr; }
}

int SerialPort::Lesen(uint8_t* puffer, int max_bytes, int timeout_ms) {
    if (!handle_) return -1;
    const DWORD ende = ::GetTickCount() + (DWORD)(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
        DWORD gelesen = 0;
        if (!::ReadFile((HANDLE)handle_, puffer, (DWORD)max_bytes, &gelesen, nullptr))
            return -1;
        if (gelesen > 0) return (int)gelesen;
        if (::GetTickCount() >= ende) return 0;
        ::Sleep(2);
    }
}

bool SerialPort::Schreiben(const uint8_t* daten, int anzahl) {
    if (!handle_ || anzahl <= 0) return false;
    int raus = 0;
    while (raus < anzahl) {
        DWORD n = 0;
        if (!::WriteFile((HANDLE)handle_, daten + raus, (DWORD)(anzahl - raus),
                         &n, nullptr))
            return false;
        if (n == 0) return false;
        raus += (int)n;
    }
    return true;
}

}  // namespace core
