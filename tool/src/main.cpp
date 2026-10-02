// RAW-TURN, das PC-Werkzeug.
//
// Gliederung: vier Gruppen untereinander
// (Anmeldung, Verbindung, Spiel, SAROO einrichten), darunter zweigeteilt
// Achievements links, Live-Werte und Protokoll rechts.
//
// KEINE BEISPIELDATEN. Die Oberflaeche zeigt ausschliesslich, was in
// app::State steht, und das ist beim Start leer. Eine Oberflaeche, die
// erfundene Werte zeigt, als waeren sie echt, fuehrt in die Irre.
//
// Die Logik (Protokoll, Mailbox, Hook-Installation, RA-Auswertung) kommt in
// eine eigene Schicht OHNE Oberflaechen-Bibliothek -- dieselbe
// Architekturregel wie in mega-raw, wo sich genau das ausgezahlt hat.
//
// Fenster und Zeichnung: Win32 + OpenGL2, beides gehoert zu Windows. Dadurch
// braucht das fertige Programm keine einzige mitgelieferte DLL. OpenGL2 und
// nicht OpenGL3, weil Windows ohne Grafiktreiber nur "OpenGL 1.1.0 / GDI
// Generic" liefert.

#include <windows.h>
#include <shellapi.h>   // ShellExecuteW fuer Ko-fi und Support
#include <gl/gl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_opengl2.h"

#include <atomic>
#include <algorithm>
#include <string>
#include <thread>
#include <vector>

#include "app_state.h"
#include "buildinfo.h"
#include "connection.h"
#include "core/http.h"
#include "core/i18n.h"
#include "core/mailbox.h"
#include "erzeugt_katze.h"
#include "erzeugt_logo.h"
#include "core/dialoge.h"
#include "core/serial.h"
#include "ra_session.h"
#include "setup_window.h"
#include "theme.h"
#include "unlock_popup.h"

extern "C" {
#include "rc_version.h"
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

HDC   g_dc = nullptr;
HGLRC g_rc = nullptr;
float g_dpi = 1.0f;
app::State g_state;
Connection g_verbindung;
RaSession g_sitzung;
SetupWindow g_einrichtung;
uint32_t g_zuletzt_ausgewertet = 0;
bool g_je_ausgewertet = false;
// Seit wann steht die Lage „Spiel erkannt, Set geladen, aber kein Hook"?
// 0 heisst: steht gerade nicht. Siehe das automatische Nachtragen in DrawUi.
unsigned g_hook_wartet_seit = 0;
// Wann zuletzt nachgesehen wurde, ob die SD-Karte wieder am PC steckt.
unsigned g_karte_geprueft = 0;

// Hardcore zaehlt nur, wenn die Saturn-Firmware gesperrte Speicherstaende
// bestaetigt und seit dem Spielstart keiner geladen wurde. Die Bestaetigung
// kommt als Statuswort ueber die Mailbox, siehe core/mailbox.h. So wie in
// mega-raw: Ist das Menue mit Speicherstaenden offen, laeuft die Sitzung als
// Softcore.
enum class HardcoreLage { Aus, Wirksam, NichtBestaetigt, StandGeladen };

bool SaturnMeldetSperre(const app::State& s) {
    return s.saturn_status_da &&
           (s.saturn_status & core::kStatusKennMaske) == core::kStatusKenn &&
           (s.saturn_status & core::kStatusGesperrt) != 0;
}

HardcoreLage HardcoreLageBestimmen(const app::State& s) {
    if (!s.hardcore) return HardcoreLage::Aus;
    if (!SaturnMeldetSperre(s)) return HardcoreLage::NichtBestaetigt;
    if (s.saturn_status & core::kStatusGeladen) return HardcoreLage::StandGeladen;
    return HardcoreLage::Wirksam;
}

// Katzen-Portrait als OpenGL-Textur, einmal beim Start hochgeladen.
GLuint g_logo = 0;

// Adressen wie in mega-raw.
const wchar_t* const kKofiUrl    = L"https://ko-fi.com/liqui69747";
const wchar_t* const kSupportUrl = L"https://liquid-wq.github.io/data/";
// Fehlerliste bei GitHub.
const wchar_t* const kIssuesUrl  = L"https://github.com/liquid-wq/raw-turn/issues";
const wchar_t* const kReleaseUrl = L"https://github.com/liquid-wq/raw-turn/releases/latest";

// Update-Hinweis wie in mega-raw: Die Seite nennt in einer Textdatei die
// Build-Nummer der aktuellen Veroeffentlichung. Ist sie hoeher als die eigene,
// erscheint einmal ein Hinweis. Ohne Netz bleibt es still.
const char* const kVersionHost = "liquid-wq.github.io";
const char* const kVersionPfad = "/data/version_rawturn.txt";
std::atomic<int> g_neuer_build{0};

void UpdatePruefen() {
    // Losgeloest: Die Abfrage soll das Beenden nie aufhalten.
    std::thread([]() {
        ::Sleep(2000);
        const core::HttpAntwort a = core::HttpGet(kVersionHost, kVersionPfad, 8000);
        if (!a.ok) return;
        const int n = std::atoi(a.koerper.c_str());
        if (n > buildinfo::kBuild) g_neuer_build = n;
    }).detach();
}

// Portsuche laeuft nebenher, damit das Fenster waehrenddessen bedienbar bleibt.
std::thread g_suchthread;
std::atomic<bool> g_sucht{false};
std::mutex g_suchsperre;
core::Suchergebnis g_suchergebnis;
bool g_suchergebnis_neu = false;

// ---------------------------------------------------------------------------
// Startprotokoll.
//
// Ein Fensterprogramm, das beim Start abbricht, hinterlaesst sonst nichts --
// es geht einfach wieder zu. Deshalb schreibt jeder Schritt eine Zeile nach
// %TEMP%\saturn_ra_monitor.log.
// ---------------------------------------------------------------------------
FILE* g_log = nullptr;

void LogOeffnen() {
    char pfad[MAX_PATH] = {};
    if (::GetTempPathA(MAX_PATH, pfad) == 0) return;
    std::strncat(pfad, "saturn_ra_monitor.log", MAX_PATH - std::strlen(pfad) - 1);
    g_log = std::fopen(pfad, "w");
}

void Log(const char* was) {
    if (!g_log) return;
    std::fprintf(g_log, "%s\n", was);
    std::fflush(g_log);   // sofort schreiben, sonst ist es nach einem Absturz weg
}

void LogSchliessen() {
    if (g_log) { std::fclose(g_log); g_log = nullptr; }
}

LONG WINAPI AufAbsturz(EXCEPTION_POINTERS* e) {
    char text[128];
    std::snprintf(text, sizeof(text), "ABSTURZ code=0x%08lX",
                  e && e->ExceptionRecord ? e->ExceptionRecord->ExceptionCode : 0);
    Log(text);
    LogSchliessen();
    ::MessageBoxA(nullptr,
                  T("Das Programm ist abgestuerzt.\n\n"
                    "Einzelheiten stehen in %TEMP%\\saturn_ra_monitor.log"),
                  "RAW-TURN", MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

// ---------------------------------------------------------------------------

bool CreateGlContext(HWND hwnd) {
    g_dc = ::GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    const int pf = ::ChoosePixelFormat(g_dc, &pfd);
    if (pf == 0) return false;
    if (!::SetPixelFormat(g_dc, pf, &pfd)) return false;
    g_rc = ::wglCreateContext(g_dc);
    if (!g_rc) return false;
    return ::wglMakeCurrent(g_dc, g_rc) != FALSE;
}

void DestroyGlContext(HWND hwnd) {
    ::wglMakeCurrent(nullptr, nullptr);
    if (g_rc) { ::wglDeleteContext(g_rc); g_rc = nullptr; }
    if (g_dc) { ::ReleaseDC(hwnd, g_dc); g_dc = nullptr; }
}

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) ::glViewport(0, 0, LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// --- kleine Helfer ------------------------------------------------------

void Luft(float y) { ImGui::Dummy(ImVec2(0.0f, y * g_dpi)); }
void Trenner()     { ImGui::SameLine(0.0f, 14.0f * g_dpi); }

// Abstaende bewusst knapp: Eine zu luftige Fassung schiebt den unteren Teil
// aus dem Fenster.
// Zwei Darstellungen, je nach Farbschema: Im Schema „Heute" sind die Gruppen
// nur durch Haarlinien getrennt. Die anderen Schemata legen jede Gruppe auf
// eine leicht abgesetzte Flaeche -- das war die wichtigste Massnahme gegen den
// Eindruck „schwarz auf schwarz".
bool g_in_karte = false;

// `knopf`: optionaler Knopf rechtsbuendig in der Titelzeile der Gruppe.
// Rueckgabe: ob er angeklickt wurde.
//
// Warum in der Titelzeile und nicht neben dem Beschreibungstext: Bricht der
// Text daneben auf zwei Zeilen um, geraet der Knopf durcheinander. In der
// Titelzeile gibt es
// keinen Umbruch, und die Beschreibung bekommt die volle Breite.
bool GruppeBeginn(const char* marke, const char* knopf = nullptr) {
    Luft(4.0f);
    if (theme::kKarte) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(theme::kKarte));
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(theme::kKartenrand));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * g_dpi, 7.0f * g_dpi));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f * g_dpi);
        ImGui::BeginChild(marke, ImVec2(0.0f, 0.0f),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
                          ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        g_in_karte = true;
    }
    bool geklickt = false;
    if (knopf) {
        ImGui::AlignTextToFramePadding();
        theme::Marke(marke);
        const float breite = ImGui::CalcTextSize(knopf).x +
                             ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - breite);
        geklickt = ImGui::Button(knopf);
    } else {
        theme::Marke(marke);
    }
    Luft(1.0f);
    return geklickt;
}

void GruppeEnde(bool letzte = false) {
    if (g_in_karte) {
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        g_in_karte = false;
        Luft(2.0f);
        return;
    }
    Luft(4.0f);
    if (!letzte) theme::Haarlinie();
}

void Beschriftung(const char* t) {
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kLeise));
    ImGui::TextUnformatted(t);
    ImGui::PopStyleColor();
}

void Gedaempft(const char* t) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kSehrLeise));
    ImGui::TextUnformatted(t);
    ImGui::PopStyleColor();
}

// Knopf, der noch keine Funktion hat. Sieht bedienbar aus, ist es aber nicht --
// deshalb beim Anklicken ein ehrlicher Hinweis statt stiller Wirkungslosigkeit.
bool KnopfOhneFunktion(const char* text) {
    const bool k = ImGui::Button(text);
    if (k) g_state.Protokoll(std::string(text) +
        T(": noch ohne Funktion, die Logik ist noch nicht angeschlossen."),
        app::Rang::Warnung);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Noch ohne Funktion"));
    return k;
}

// --- Anschluesse --------------------------------------------------------

void VerbindenMit(const std::string& anschluss);

void PortsAuflisten(bool melden) {
    const std::vector<core::PortInfo> liste = core::OrdnePorts(g_state.port);
    g_state.gefundene_ports.clear();
    for (const core::PortInfo& p : liste) g_state.gefundene_ports.push_back(p.geraet);

    if (liste.empty()) {
        if (melden) g_state.Protokoll(T("Kein serieller Anschluss gefunden."),
                                      app::Rang::Warnung);
        return;
    }
    if (g_state.port[0] == '\0') {
        std::snprintf(g_state.port, sizeof(g_state.port), "%s", liste[0].geraet.c_str());
        if (!liste[0].adapter.empty()) {
            char t[160];
            std::snprintf(t, sizeof(t), T("Anschluss vorausgewählt: %s (%s)."),
                          liste[0].geraet.c_str(), liste[0].adapter.c_str());
            g_state.Protokoll(t);
        }
    }
    if (melden) {
        char t[96];
        std::snprintf(t, sizeof(t), T("%d Anschluss/Anschlüsse gefunden."), (int)liste.size());
        g_state.Protokoll(t);
    }
}

void PortSuchen() {
    if (g_sucht.load()) return;
    if (g_verbindung.Laeuft()) {
        g_state.Protokoll(T("Erst trennen, der Anschluss ist belegt."), app::Rang::Warnung);
        return;
    }
    g_sucht = true;
    g_state.Protokoll(T("Suche Anschluss, höre an jedem kurz mit…"));

    const std::string bevorzugt = g_state.port;
    const int baud = std::atoi(g_state.baud) > 0 ? std::atoi(g_state.baud)
                                                 : core::kStandardBaud;
    if (g_suchthread.joinable()) g_suchthread.join();
    g_suchthread = std::thread([bevorzugt, baud]() {
        core::Suchergebnis e = core::SuchePort(bevorzugt, baud, 1200);
        {
            std::lock_guard<std::mutex> g(g_suchsperre);
            g_suchergebnis = std::move(e);
            g_suchergebnis_neu = true;
        }
        g_sucht = false;
    });
}

void SuchergebnisAbholen() {
    std::lock_guard<std::mutex> g(g_suchsperre);
    if (!g_suchergebnis_neu) return;
    g_suchergebnis_neu = false;

    g_state.gefundene_ports.clear();
    for (const core::PortInfo& p : g_suchergebnis.alle)
        g_state.gefundene_ports.push_back(p.geraet);

    if (!g_suchergebnis.gefunden) {
        g_state.Protokoll(T("Kein serieller Anschluss gefunden."), app::Rang::Warnung);
        return;
    }
    std::snprintf(g_state.port, sizeof(g_state.port), "%s",
                  g_suchergebnis.port.geraet.c_str());

    const std::string adapter = g_suchergebnis.port.adapter.empty()
                                    ? std::string()
                                    : " (" + g_suchergebnis.port.adapter + ")";
    char t[320];
    std::snprintf(t, sizeof(t), "%s%s — %s", g_suchergebnis.port.geraet.c_str(),
                  adapter.c_str(), g_suchergebnis.erlaeuterung.c_str());
    g_state.Protokoll(t, g_suchergebnis.sicherheit == core::Sicherheit::Frames
                             ? app::Rang::Gut : app::Rang::Normal);

    if (g_suchergebnis.sicherheit == core::Sicherheit::Vermutung)
        g_state.Protokoll(T("Kein Gerät hat geantwortet. Der Anschluss ist nur nach "
                            "USB-Kennung geraten."), app::Rang::Warnung);

    // Direkt verbinden: gedrueckt wurde "Verbinden", nicht "Suchen".
    VerbindenMit(g_suchergebnis.port.geraet);
}

void VerbindenMit(const std::string& anschluss) {
    const int baud = std::atoi(g_state.baud);
    if (baud <= 0) {
        g_state.Protokoll(T("Baudrate muss eine Zahl sein."), app::Rang::Warnung);
        return;
    }
    std::string fehler;
    if (!g_verbindung.Starten(anschluss, baud, &fehler))
        g_state.Protokoll(fehler, app::Rang::Warnung);
}

// Ein Knopf statt Auswahlliste: Das Werkzeug sucht die SAROO selbst.
void VerbindungUmschalten() {
    if (g_verbindung.Laeuft()) { g_verbindung.Beenden(); return; }
    if (g_sucht.load()) return;

    if (g_state.port[0] != '\0') { VerbindenMit(g_state.port); return; }

    // Noch kein Anschluss bekannt: erst suchen, dann verbinden.
    g_sucht = true;
    g_state.Protokoll(T("Suche die SAROO…"));
    const int baud = std::atoi(g_state.baud) > 0 ? std::atoi(g_state.baud)
                                                 : core::kStandardBaud;
    if (g_suchthread.joinable()) g_suchthread.join();
    g_suchthread = std::thread([baud]() {
        core::Suchergebnis e = core::SuchePort("", baud, 1500);
        {
            std::lock_guard<std::mutex> g(g_suchsperre);
            g_suchergebnis = std::move(e);
            g_suchergebnis_neu = true;
        }
        g_sucht = false;
    });
}

// ---------------------------------------------------------------------------

// --- Kopfbereich, Menue und Dialoge: nach dem Vorbild von mega-raw ----------
//
// mega-raw (mainwindow.cpp): Menueleiste mit „Optionen" und „Ko-fi" als direkte
// Eintraege, darunter das Katzen-Portrait zentriert auf 53 Pixel Hoehe,
// darunter zentriert und grau der Copyright-Hinweis. Ko-fi fragt erst nach und
// verweist auf „ueber die Katze"; die Seite oeffnet sich erst mit OK.

void LogoHochladen() {
    ::glGenTextures(1, &g_logo);
    ::glBindTexture(GL_TEXTURE_2D, g_logo);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    ::glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    // Kantenlaenge ist eine Zweierpotenz: OpenGL 1.1 (die Software-Fassung von
    // Windows) kennt nichts anderes. Angezeigt wird nur der belegte Ausschnitt.
    ::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kLogoTexturKante, kLogoTexturKante, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, kLogoPixel);
}

void OeffneSeite(const wchar_t* url) {
    ::ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

// Zeilenweise zentriert, wie das zentrierte Label in mega-raw.
void Zentriert(const char* text, ImFont* schrift = nullptr, ImU32 farbe = 0) {
    if (schrift) ImGui::PushFont(schrift, 0.0f);
    if (farbe) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(farbe));
    const float verfuegbar = ImGui::GetContentRegionAvail().x;
    const float start = ImGui::GetCursorPosX();
    const char* anfang = text;
    for (;;) {
        const char* ende = std::strchr(anfang, '\n');
        const char* stop = ende ? ende : anfang + std::strlen(anfang);
        const float b = ImGui::CalcTextSize(anfang, stop).x;
        ImGui::SetCursorPosX(start + (verfuegbar - b) * 0.5f);
        ImGui::TextUnformatted(anfang, stop);
        if (!ende) break;
        anfang = ende + 1;
    }
    if (farbe) ImGui::PopStyleColor();
    if (schrift) ImGui::PopFont();
}

// Zwei gleich breite Knoepfe mittig nebeneinander. Rueckgabe: 1 links, 2 rechts.
int KnopfPaar(const char* links, const char* rechts) {
    const float b = 100.0f * g_dpi, luecke = 10.0f * g_dpi;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         (ImGui::GetContentRegionAvail().x - (2.0f * b + luecke)) * 0.5f);
    int geklickt = 0;
    if (ImGui::Button(links, ImVec2(b, 0.0f))) geklickt = 1;
    ImGui::SameLine(0.0f, luecke);
    if (ImGui::Button(rechts, ImVec2(b, 0.0f))) geklickt = 2;
    return geklickt;
}

// Uebersetzt ueber T(); hier nur der deutsche Schluessel.
const char* const kKatzeHinweis =
    TK("aber falls du darüber nachdenkst,\nlies bitte zuerst 'über die Katze'");

// Titel mit fester Kennung hinter "###". Dear ImGui erkennt Dialoge an ihrem
// Titel; ohne feste Kennung wuerde ein offener Dialog beim Sprachwechsel
// zuklappen, weil sich sein Titel aendert.
std::string DialogTitel(const char* de, const char* kennung) {
    return std::string(T(de)) + "###" + kennung;
}

// Freischaltung im Werkzeugfenster, wenn in den Optionen gewaehlt. Sonst
// erscheint sie am Bildschirmrand (unlock_popup). Die Karte liegt unten im
// Werkzeugfenster, sechs Sekunden, mehrere nacheinander.
struct FensterKarte {
    std::string titel, beschreibung;
    int punkte = 0;
    DWORD start = 0;          // 0 = noch nicht angezeigt
};
std::vector<FensterKarte> g_fenster_karten;
constexpr DWORD kKarteMs = 6000;
// Freier Streifen unter dem Werkzeug, in logischen Punkten. Dort faehrt die
// Karte heraus, ohne etwas vom Werkzeug zu verdecken.
constexpr float kStreifen = 130.0f;
// Wahl in den Optionen: false = Einblendung am Bildschirmrand, kein Streifen;
// true = Karte im Werkzeugfenster, dafuer der schwarze Streifen unten.
bool g_karte_im_werkzeug = false;
HWND g_fenster = nullptr;

// Fenster mitwachsen lassen, wenn Inhalt dazukommt (Spiel erkannt,
// Speicherstaende, Achievement-Liste). `bedarf` ist die noetige Hoehe der
// Innenflaeche in Pixeln. Gewachsen wird nur, wenn der Bedarf steigt; wer das
// Fenster danach selbst kleiner zieht, behaelt es so.
void FensterWachsen(float bedarf) {
    static float zuletzt = 0.0f;
    if (bedarf < zuletzt - 1.0f) { zuletzt = bedarf; return; }
    if (bedarf <= zuletzt + 1.0f) return;
    zuletzt = bedarf;
    if (!g_fenster || ::IsZoomed(g_fenster) || ::IsIconic(g_fenster)) return;
    RECT c, r;
    if (!::GetClientRect(g_fenster, &c) || !::GetWindowRect(g_fenster, &r)) return;
    const int fehlt = (int)bedarf - (c.bottom - c.top);
    if (fehlt <= 0) return;
    int h = (r.bottom - r.top) + fehlt;
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!::GetMonitorInfoW(::MonitorFromWindow(g_fenster, MONITOR_DEFAULTTONEAREST), &mi)) return;
    const RECT& a = mi.rcWork;
    if (h > a.bottom - a.top) h = a.bottom - a.top;
    int y = r.top;
    if (y + h > a.bottom) y = a.bottom - h;
    ::SetWindowPos(g_fenster, nullptr, r.left, y, r.right - r.left, h,
                   SWP_NOZORDER | SWP_NOACTIVATE);
}

// Fensterhoehe um den Streifen aendern, wenn die Wahl umgestellt wird. Das
// Werkzeug selbst behaelt so seine Hoehe; hoechstens bis zum Arbeitsbereich.
void StreifenHoeheAnpassen(bool mit_streifen) {
    if (!g_fenster || ::IsZoomed(g_fenster)) return;
    RECT r;
    if (!::GetWindowRect(g_fenster, &r)) return;
    const int d = (int)(kStreifen * g_dpi);
    int h = (r.bottom - r.top) + (mit_streifen ? d : -d);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (::GetMonitorInfoW(::MonitorFromWindow(g_fenster, MONITOR_DEFAULTTONEAREST), &mi)) {
        const RECT& a = mi.rcWork;
        if (h > a.bottom - a.top) h = a.bottom - a.top;
        int y = r.top;
        if (y + h > a.bottom) y = a.bottom - h;
        ::SetWindowPos(g_fenster, nullptr, r.left, y, r.right - r.left, h,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void FensterKarteZeigen(const app::Freischaltung& f) {
    FensterKarte k;
    k.titel = f.titel;
    k.beschreibung = f.beschreibung;
    k.punkte = f.punkte;
    g_fenster_karten.push_back(k);
}

// Eine Freischaltung anzeigen, je nach Wahl im Werkzeug oder am Bildschirmrand.
void FreischaltungZeigen(const app::Freischaltung& f) {
    if (g_karte_im_werkzeug) FensterKarteZeigen(f);
    else popup::Zeigen(f, i18n::AktuelleSprache() == i18n::Sprache::En, g_dpi);
}

void ZeichneFensterKarte() {
    if (g_fenster_karten.empty()) return;
    FensterKarte& k = g_fenster_karten.front();
    const DWORD jetzt = ::GetTickCount();
    if (k.start == 0) k.start = jetzt;
    const DWORD lauf = jetzt - k.start;
    if (lauf >= kKarteMs) {
        g_fenster_karten.erase(g_fenster_karten.begin());
        return;
    }
    // Faehrt unten aus dem Fensterrand herauf (400 ms, mit Nachfedern) und am
    // Ende wieder hinunter (300 ms).
    float raus = 1.0f;                       // 1 = ganz draussen, 0 = versenkt
    if (lauf < 400) {
        const float t = lauf / 400.0f - 1.0f;
        raus = 1.0f + 2.7f * t * t * t + 1.7f * t * t;   // aus mit Nachfedern
    } else if (lauf > kKarteMs - 300) {
        raus = (kKarteMs - lauf) / 300.0f;
    }
    const int ai = 255;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float s = g_dpi;
    const float breite = std::min(vp->Size.x - 24.0f * s, 460.0f * s);
    const float hoehe = 96.0f * s;
    // Faehrt am Fensterrand aus dem Streifen unter dem Werkzeug herauf und
    // steht dann mittig darin. Der Streifen schneidet ab, was noch versenkt ist.
    const float unten = vp->Pos.y + vp->Size.y;
    const float oben = unten - kStreifen * s;
    const ImVec2 p0(vp->Pos.x + (vp->Size.x - breite) * 0.5f,
                    unten - raus * (hoehe + (kStreifen * s - hoehe) * 0.5f));
    ImGui::GetForegroundDrawList()->PushClipRect(ImVec2(vp->Pos.x, oben),
                                                 ImVec2(vp->Pos.x + vp->Size.x, unten));
    struct ClipEnde { ~ClipEnde() { ImGui::GetForegroundDrawList()->PopClipRect(); } } clip_ende;
    const ImVec2 p1(p0.x + breite, p0.y + hoehe);

    ImDrawList* d = ImGui::GetForegroundDrawList();
    const ImU32 gold = IM_COL32(0xf5, 0xc2, 0x42, ai);
    d->AddRectFilled(p0, p1, IM_COL32(0x16, 0x16, 0x1c, 245), 10.0f * s);
    d->AddRect(p0, p1, gold, 10.0f * s, 0, 2.5f * s);

    const bool en = i18n::AktuelleSprache() == i18n::Sprache::En;
    const float x = p0.x + 16.0f * s;
    float y = p0.y + 10.0f * s;
    ImFont* f = ImGui::GetFont();
    const float gr = ImGui::GetFontSize();
    d->AddText(f, gr * 0.95f, ImVec2(x, y), gold,
               en ? "ACHIEVEMENT UNLOCKED!" : "ACHIEVEMENT FREIGESCHALTET!");
    char pkt[32];
    std::snprintf(pkt, sizeof(pkt), en ? "%d pts" : "%d Pkt", k.punkte);
    const float pw = f->CalcTextSizeA(gr * 0.95f, FLT_MAX, 0.0f, pkt).x;
    d->AddText(f, gr * 0.95f, ImVec2(p1.x - 16.0f * s - pw, y), gold, pkt);
    y += gr * 1.25f;
    d->AddText(f, gr * 1.25f, ImVec2(x, y), IM_COL32(255, 255, 255, ai), k.titel.c_str());
    y += gr * 1.5f;
    d->AddText(f, gr * 0.9f, ImVec2(x, y), IM_COL32(0xc8, 0xc8, 0xd0, ai),
               k.beschreibung.c_str(), nullptr, breite - 32.0f * s);
}

void ZeichneMenue(bool* optionen, bool* kofi) {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::MenuItem(T("Optionen"))) *optionen = true;
    if (ImGui::MenuItem("Ko-fi"))    *kofi = true;

    // Build-Nummer rechts in derselben Leiste. Sie muss auf jedem
    // Bildschirmfoto stehen, damit klar ist, welcher Stand laeuft.
    // Ist das Fenster zu schmal, faellt erst die rcheevos-Fassung weg, dann
    // bleibt die Build-Angabe allein stehen.
    char rechts[128];
    std::snprintf(rechts, sizeof(rechts), "%s  \xC2\xB7  rcheevos %s",
                  buildinfo::string(), rc_version_string());
    if (theme::mono) ImGui::PushFont(theme::mono, 0.0f);
    const float platz = ImGui::GetContentRegionAvail().x - 8.0f * g_dpi;
    if (ImGui::CalcTextSize(rechts).x > platz)
        std::snprintf(rechts, sizeof(rechts), "%s", buildinfo::string());
    float breite = ImGui::CalcTextSize(rechts).x;
    if (breite > platz) breite = platz;
    // In der Menueleiste reicht der Inhaltsbereich bis unter den rechten
    // Fensterrand; ohne den Innenabstand abzuziehen, ragt der Text hinaus.
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x -
                    breite - 4.0f * g_dpi);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kSehrLeise));
    ImGui::TextUnformatted(rechts);
    ImGui::PopStyleColor();
    if (theme::mono) ImGui::PopFont();

    ImGui::EndMenuBar();
}

void ZeichneKatzeDialog(bool oeffnen) {
    const std::string titel_text = DialogTitel("über die Katze", "katze");
    const char* titel = titel_text.c_str();
    if (oeffnen) ImGui::OpenPopup("###katze");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520.0f * g_dpi, 560.0f * g_dpi), ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * g_dpi, 12.0f * g_dpi));
    if (ImGui::BeginPopupModal(titel, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        const float unten = ImGui::GetFrameHeight() + 12.0f * g_dpi;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * g_dpi, 8.0f * g_dpi));
        if (ImGui::BeginChild("katzentext",
                              ImVec2(0.0f, ImGui::GetContentRegionAvail().y - unten),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding)) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(i18n::AktuelleSprache() == i18n::Sprache::En
                                       ? kKatzeTextEn : kKatzeText);
            ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        Luft(6.0f);
        if (ImGui::Button("OK", ImVec2(-FLT_MIN, 0.0f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

void ZeichneOptionen(bool oeffnen) {
    if (oeffnen) ImGui::OpenPopup("###optionen");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(360.0f * g_dpi, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f * g_dpi, 16.0f * g_dpi));
    const std::string titel = DialogTitel("Optionen", "optionen");
    static bool fehler_hinweis = false;
    // Mit Zeiger zeigt ImGui oben rechts ein X zum Schliessen, damit niemand
    // bis zum Knopf „Schließen" ganz unten scrollen muss.
    bool offen = true;
    if (oeffnen) fehler_hinweis = false;
    if (ImGui::BeginPopupModal(titel.c_str(), &offen,
                               ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoSavedSettings)) {
        // Sprache. Wie das Farbschema: wirkt sofort, wird gleich gespeichert.
        // Nur schon geschriebene Protokollzeilen bleiben in ihrer Sprache.
        theme::Marke("Sprache / Language");
        Luft(2.0f);
        {
            const i18n::Sprache jetzt = i18n::AktuelleSprache();
            i18n::Sprache neu = jetzt;
            if (ImGui::RadioButton("Deutsch", jetzt == i18n::Sprache::De)) neu = i18n::Sprache::De;
            ImGui::SameLine(0.0f, 18.0f * g_dpi);
            if (ImGui::RadioButton("English", jetzt == i18n::Sprache::En)) neu = i18n::Sprache::En;
            if (neu != jetzt) {
                i18n::SpracheSetzen(neu);
                core::Einstellungen e;
                e.Laden();
                e.sprache = i18n::Kennung(neu);
                e.Speichern();
            }
        }
        Luft(10.0f);

        // Farbschema. Wirkt sofort und wird gleich gespeichert -- man sieht die
        // Wirkung hinter dem Dialog, ohne erst bestaetigen zu muessen.
        theme::Marke(T("Farbschema"));
        Luft(2.0f);
        for (int i = 0; i < theme::kStilAnzahl; ++i) {
            const theme::Stil s = (theme::Stil)i;
            if (ImGui::RadioButton(T(theme::StilName(s)), theme::AktuellerStil() == s) &&
                theme::AktuellerStil() != s) {
                theme::StilSetzen(s);
                core::Einstellungen e;
                e.Laden();
                e.stil = theme::StilKennung(s);
                e.Speichern();
            }
        }
        Luft(10.0f);

        // Fehler melden: oeffnet die Fehlerliste bei GitHub, siehe kIssuesUrl.
        if (ImGui::Button(T("Fehler melden"), ImVec2(-FLT_MIN, 0.0f))) {
            if (kIssuesUrl[0] != L'\0') OeffneSeite(kIssuesUrl);
            else fehler_hinweis = true;
        }
        if (fehler_hinweis && kIssuesUrl[0] == L'\0') {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kSehrLeise));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(T("Der Link zur Fehlerliste folgt, sobald das Projekt "
                                     "auf GitHub liegt."));
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }

        // Wo eine Freischaltung erscheint. Im Werkzeug braucht sie den
        // Streifen unten.
        Luft(10.0f);
        theme::Marke(T("Freischalt-Anzeige"));
        Luft(2.0f);
        {
            bool neu = g_karte_im_werkzeug;
            if (ImGui::RadioButton(T("am Bildschirmrand"), !neu)) neu = false;
            if (ImGui::RadioButton(T("im Werkzeug"), neu)) neu = true;
            if (neu != g_karte_im_werkzeug) {
                g_karte_im_werkzeug = neu;
                StreifenHoeheAnpassen(neu);
                core::Einstellungen e;
                e.Laden();
                e.anzeige = neu ? "werkzeug" : "";
                e.Speichern();
            }
        }

        // Die Freischalt-Einblendung ansehen, ohne erst ein Achievement holen zu
        // muessen. Ausdruecklich als Probe beschriftet.
        Luft(4.0f);
        if (ImGui::Button(T("Freischalt-Anzeige testen"), ImVec2(-FLT_MIN, 0.0f))) {
            app::Freischaltung probe;
            probe.titel = T("Probe-Anzeige");
            probe.beschreibung = T("So sieht eine Freischaltung aus.");
            probe.punkte = 10;
            FreischaltungZeigen(probe);
        }
        Luft(10.0f);

        // Dateien auf die Karte, während sie im Gerät steckt. Erspart das
        // Herausnehmen bei jedem Firmware-Stand. Geschrieben wird gerätseitig
        // erst in eine Zwischendatei, das Ziel wird nur bei stimmender
        // Prüfsumme ersetzt.
        theme::Marke(T("Dateien auf die SAROO-Karte"));
        Luft(2.0f);
        if (g_verbindung.DateiSendeLaeuft()) {
            char t[64];
            std::snprintf(t, sizeof(t), "%d%%", g_verbindung.DateiFortschritt());
            ImGui::ProgressBar(g_verbindung.DateiFortschritt() / 100.0f,
                               ImVec2(-FLT_MIN, 0.0f), t);
            Luft(2.0f);
            if (ImGui::Button(T("Übertragung abbrechen"), ImVec2(-FLT_MIN, 0.0f)))
                g_verbindung.DateiAbbrechen();
        } else {
            struct Ziel { const char* knopf; const char* filter; const char* pfad; };
            static const Ziel ziele[] = {
                {"Saturn-Firmware (ssfirm.bin) übertragen",
                 "ssfirm.bin\0ssfirm*.bin\0Alle Dateien\0*.*\0", "/SAROO/ssfirm.bin"},
                {"STM32-Firmware (mcuapp.bin) übertragen",
                 "mcuapp.bin\0mcuapp*.bin\0Alle Dateien\0*.*\0", "/SAROO/mcuapp.bin"},
            };
            for (const Ziel& z : ziele) {
                if (!ImGui::Button(T(z.knopf), ImVec2(-FLT_MIN, 0.0f))) continue;
                if (!g_verbindung.Laeuft()) {
                    g_state.Protokoll(T("Erst mit der SAROO verbinden."),
                                      app::Rang::Warnung);
                    continue;
                }
                const std::string quelle =
                    core::DateiWaehlen(T("Datei für die SAROO-Karte"), z.filter, "");
                if (quelle.empty()) continue;
                std::string fehler;
                if (!g_verbindung.DateiSenden(quelle, z.pfad, &fehler))
                    g_state.Protokoll(fehler, app::Rang::Warnung);
            }
            Luft(2.0f);
            Zentriert(T("Wirkt beim nächsten Einschalten. Das Gerät sollte dabei im "
                        "Startmenü stehen, nicht im Spiel."),
                      theme::kursiv, theme::kLeise);
        }

        Luft(10.0f);
        theme::Haarlinie();
        Luft(8.0f);

        // Support-Verweis und Hinweis, wie in mega-raw.
        const float b = ImGui::CalcTextSize("Support").x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             (ImGui::GetContentRegionAvail().x - b) * 0.5f);
        if (ImGui::TextLink("Support")) OeffneSeite(kSupportUrl);
        Zentriert(T(kKatzeHinweis), theme::kursiv, theme::kLeise);

        Luft(10.0f);
        bool katze = ImGui::Button(T("über die Katze"), ImVec2(-FLT_MIN, 0.0f));
        Luft(4.0f);
        if (ImGui::Button(T("Schließen"), ImVec2(-FLT_MIN, 0.0f))) {
            fehler_hinweis = false;
            ImGui::CloseCurrentPopup();
        }

        // Verschachtelt geoeffnet, damit der Optionen-Dialog darunter bleibt.
        ZeichneKatzeDialog(katze);
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

void ZeichneKofi(bool oeffnen) {
    if (oeffnen) ImGui::OpenPopup("###kofi");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(340.0f * g_dpi, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f * g_dpi, 16.0f * g_dpi));
    if (ImGui::BeginPopupModal("Ko-fi###kofi", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoSavedSettings)) {
        Zentriert(T(kKatzeHinweis), theme::kursiv);
        Luft(12.0f);
        // Kein Link im Text: Erst OK oeffnet die Seite. Wer abbricht, landet
        // nirgendwo.
        const int k = KnopfPaar("OK", T("Abbrechen"));
        if (k == 1) { OeffneSeite(kKofiUrl); ImGui::CloseCurrentPopup(); }
        if (k == 2) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

void ZeichneUpdate() {
    static bool gezeigt = false;
    const int neu = g_neuer_build.load();
    if (neu > 0 && !gezeigt) {
        gezeigt = true;
        ImGui::OpenPopup("###update");
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(380.0f * g_dpi, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f * g_dpi, 16.0f * g_dpi));
    const std::string titel = std::string(T("Update verfügbar")) + "###update";
    if (ImGui::BeginPopupModal(titel.c_str(), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoSavedSettings)) {
        char t[200];
        std::snprintf(t, sizeof(t), T("Eine neue Version ist verfügbar (Build %d, du hast Build %d)."),
                      neu, buildinfo::kBuild);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(t);
        ImGui::PopTextWrapPos();
        Luft(12.0f);
        const int k = KnopfPaar(T("Herunterladen"), T("Später"));
        if (k == 1) { OeffneSeite(kReleaseUrl); ImGui::CloseCurrentPopup(); }
        if (k == 2) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

void ZeichneKopf() {
    Luft(6.0f);
    if (g_logo) {
        const float h = 53.0f * g_dpi;
        const float w = h * (float)kLogoBreite / (float)kLogoHoehe;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             (ImGui::GetContentRegionAvail().x - w) * 0.5f);
        ImGui::Image((ImTextureID)(intptr_t)g_logo, ImVec2(w, h), ImVec2(0.0f, 0.0f),
                     ImVec2((float)kLogoBreite / kLogoTexturKante,
                            (float)kLogoHoehe / kLogoTexturKante));
    }
    Zentriert("RAW-TURN \xE2\x80\x94 \xC2\xA9 2026 Liqui", theme::klein, theme::kSehrLeise);
    Luft(8.0f);
    theme::Haarlinie();
}

void ZeichneAnmeldung() {
    GruppeBeginn(T("RetroAchievements-Login"));

    Beschriftung(T("Benutzername"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f * g_dpi);
    ImGui::InputText("##benutzer", g_state.benutzer, sizeof(g_state.benutzer));
    Trenner();

    Beschriftung(T("Passwort"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f * g_dpi);
    ImGui::InputText("##passwort", g_state.passwort, sizeof(g_state.passwort),
                     ImGuiInputTextFlags_Password);
    Trenner();

    // "###": feste Kennungen, damit ein Sprachwechsel die Elemente nicht neu anlegt.
    const std::string speichern = std::string(T("speichern")) + "###speichern";
    ImGui::Checkbox(speichern.c_str(), &g_state.zugangsdaten_speichern);
    Trenner();
    ImGui::BeginDisabled(g_sitzung.Beschaeftigt());
    if (g_state.angemeldet) {
        if (ImGui::Button(T("Abmelden"))) g_sitzung.Abmelden();
    } else {
        if (ImGui::Button(g_sitzung.Beschaeftigt() ? T("meldet an…") : T("Anmelden")))
            g_sitzung.AnmeldenStarten(g_state.benutzer, g_state.passwort);
    }
    ImGui::EndDisabled();
    Trenner();

    ImGui::AlignTextToFramePadding();
    if (g_state.angemeldet)
        theme::Zustand(theme::kSalbei, (T("angemeldet als ") + g_state.anmeldename).c_str());
    else
        Gedaempft(T("nicht angemeldet"));

    GruppeEnde();
}

void ZeichneVerbindung() {
    GruppeBeginn(T("Verbindung"));

    ImGui::BeginDisabled(g_sucht.load());
    if (g_state.verbunden) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kMessing));
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(theme::kMessing));
        if (ImGui::Button(T("Trennen"))) VerbindungUmschalten();
        ImGui::PopStyleColor(2);
    } else {
        if (ImGui::Button(g_sucht.load() ? T("sucht…") : T("SAROO verbinden")))
            VerbindungUmschalten();
    }
    ImGui::EndDisabled();
    Trenner();

    // Kein Auswahlfeld: Das Werkzeug sucht die SAROO selbst. Der gefundene
    // Anschluss steht nur zur Information da.
    ImGui::AlignTextToFramePadding();
    switch (g_state.geraet) {
        case app::State::Geraet::Getrennt:
            Gedaempft(T("nicht verbunden")); break;
        case app::State::Geraet::PortOffen:
            theme::Zustand(theme::kBernstein, T("Anschluss offen, Gerät noch nicht erkannt"));
            break;
        case app::State::Geraet::Stumm:
            theme::Zustand(theme::kRost, T("keine Antwort")); break;
        case app::State::Geraet::SarooErkannt:
            theme::Zustand(theme::kBernstein, T("SAROO erkannt, kein Hook installiert"));
            break;
        case app::State::Geraet::HookLaeuft:
            theme::Zustand(theme::kSalbei, T("SAROO verbunden, Hook läuft")); break;
        case app::State::Geraet::HookSteht:
            theme::Zustand(theme::kBernstein, T("SAROO verbunden, Bildzähler steht")); break;
    }

    if (g_state.port[0] != '\0') {
        Trenner();
        ImGui::AlignTextToFramePadding();
        if (theme::mono) ImGui::PushFont(theme::mono, 0.0f);
        Gedaempft(g_state.port);
        if (theme::mono) ImGui::PopFont();
    }

    // Welcher Firmware-Stand laeuft wirklich? Sobald das Geraet geredet hat,
    // steht es hier. Ohne diese Zeile faellt nicht auf, wenn ein frisch
    // gebauter Stand gar nicht auf der Karte gelandet ist.
    const app::State::Firmware& f = g_state.firmware;
    // Das Baudatum der App kommt nur beim Einschalten. Wer mitten im Betrieb
    // verbindet, hoert es nie -- die Patch-Zeilen dagegen kommen bei jedem
    // Spielstart. Deshalb reicht eines von beiden, um etwas zu zeigen.
    if (!f.stm32_bau.empty() || f.ra_patch) {
        Trenner();
        ImGui::AlignTextToFramePadding();
        char fw[192];
        const char* stm = f.stm32_bau.empty() ? "?" : f.stm32_bau.c_str();
        if (!f.ra_patch) {
            // Nicht behaupten, der Patch fehle: Er meldet sich erst beim
            // Spielstart und beim Dateitransfer. Im Startmenü schweigt er,
            // und Schweigen ist keine Abwesenheit.
            std::snprintf(fw, sizeof(fw), T("STM32 %s  \xC2\xB7  RA noch nicht gehört"), stm);
            Gedaempft(fw);
        } else {
            std::snprintf(fw, sizeof(fw), T("STM32 %s  \xC2\xB7  RA %s"), stm,
                          f.ra_bau.empty() ? "?" : f.ra_bau.c_str());
            Gedaempft(fw);
        }
    }
    if (f.ssfirm_bytes > 0) {
        Trenner();
        ImGui::AlignTextToFramePadding();
        char sf[96];
        std::snprintf(sf, sizeof(sf), T("ssfirm %ld Byte"), f.ssfirm_bytes);
        Gedaempft(sf);
    }

    // Erklaerung nur dann, wenn etwas zu tun ist. Laeuft alles, kostet der
    // Satz nur Hoehe, die unten fehlt.
    const bool alles_gut = g_state.geraet == app::State::Geraet::HookLaeuft;
    if (!alles_gut && !g_state.geraet_hinweis.empty()) {
        Luft(2.0f);
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertU32ToFloat4(theme::kSehrLeise));
        // Der Hinweis wird in connection.cpp als deutscher Schluessel abgelegt
        // und erst hier uebersetzt. So folgt er einem Sprachwechsel sofort.
        ImGui::TextWrapped("%s", T(g_state.geraet_hinweis.c_str()));
        ImGui::PopStyleColor();
    }

    GruppeEnde();
}

void ZeichneSpiel() {
    GruppeBeginn(T("Spiel"));

    // Kein Auswahlfeld, kein Knopf: Das laufende Spiel meldet sich selbst,
    // sobald der Hook sitzt. Hardcore ist in dieser Fassung nicht waehlbar,
    // es wird immer im Softcore-Modus gemeldet.
    ImGui::AlignTextToFramePadding();
    if (g_sitzung.Beschaeftigt()) Gedaempft(T("frage RetroAchievements\xE2\x80\xA6"));
    else Gedaempft(T("Erkennung meldet die SAROO beim Spielstart selbst"));

    Luft(2.0f);
    if (!g_state.spiel_erkannt) {
        Gedaempft(T("kein Spiel erkannt"));
    } else {
        // Name zuerst, denn er ist die Antwort auf "welches Spiel laeuft".
        // Er stammt aus dem Disc-Kopf und haengt nicht an RetroAchievements.
        if (!g_state.spiel_name.empty()) {
            if (theme::bold) ImGui::PushFont(theme::bold, 0.0f);
            ImGui::TextUnformatted(g_state.spiel_name.c_str());
            if (theme::bold) ImGui::PopFont();
            ImGui::SameLine(0.0f, 12.0f * g_dpi);
        }
        if (theme::mono) ImGui::PushFont(theme::mono, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kLeise));
        ImGui::TextUnformatted(g_state.spiel_kennung.c_str());
        ImGui::PopStyleColor();
        if (theme::mono) ImGui::PopFont();

        if (!g_state.spiel_gebiet.empty()) {
            ImGui::SameLine(0.0f, 12.0f * g_dpi);
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::ColorConvertU32ToFloat4(theme::kSehrLeise));
            ImGui::Text(T("Gebiet %s"), g_state.spiel_gebiet.c_str());
            ImGui::PopStyleColor();
        }
        if (!g_state.ra_kennt_abzug) {
            ImGui::SameLine(0.0f, 12.0f * g_dpi);
            theme::Zustand(theme::kBernstein, T("von RetroAchievements nicht unterstützt"));
        }
        // Die Pruefsumme, die die Konsole selbst geliefert hat. Sie steht hier,
        // weil sie der einzige Schluessel zu RetroAchievements ist -- gerade
        // wenn ein Abzug dort nicht bekannt ist, ist das die Information, mit
        // der sich das klaeren laesst.
        if (!g_state.spiel_md5.empty()) {
            Luft(3.0f);
            if (theme::mono) ImGui::PushFont(theme::mono, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::ColorConvertU32ToFloat4(theme::kSehrLeise));
            ImGui::Text(T("MD5 %s  (von der Konsole gemeldet)"), g_state.spiel_md5.c_str());
            ImGui::PopStyleColor();
            if (theme::mono) ImGui::PopFont();
        }
    }

    GruppeEnde();
}

// Speicherstaende. Hier werden die Slots 1 bis 4 gewaehlt. Die Schnellstaende
// X.n legt L + R + Unten am Pad an, laden lassen sie sich nur hier.
void ZeichneSpeicherstaende() {
    if (!g_state.verbunden || !g_state.spiel_erkannt) return;
    const bool frage = GruppeBeginn(T("Speicherst\xC3\xA4nde"), T("Slots lesen"));
    const bool gesperrt = g_state.hardcore;
    const bool frei = !g_state.stand_laeuft && !gesperrt;
    if (frage && !g_state.stand_laeuft)
        g_verbindung.StandAuftrag(core::kStandSlotsLesen, 0);

    const float breite = 190.0f * g_dpi;
    for (int s = 0; s < app::State::kStandSlotX0; ++s) {
        const app::State::StandSlot& z = g_state.stand_slots[s];
        ImGui::PushID(s);
        ImGui::AlignTextToFramePadding();
        if (theme::mono) ImGui::PushFont(theme::mono, 0.0f);
        ImGui::Text("%d", s + 1);
        ImGui::SameLine(0.0f, 12.0f * g_dpi);
        if (!g_state.stand_bekannt)  Gedaempft("?");
        else if (z.belegt)           ImGui::Text("%s   %s", z.zeit.c_str(), z.spielzeit.c_str());
        else                         Gedaempft(T("leer"));
        if (theme::mono) ImGui::PopFont();

        ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - breite);
        ImGui::BeginDisabled(!frei);
        if (ImGui::Button(T("Speichern"), ImVec2(92.0f * g_dpi, 0.0f)))
            g_verbindung.StandAuftrag(core::kStandSpeichern, (uint8_t)s);
        ImGui::SameLine();
        ImGui::BeginDisabled(g_state.stand_bekannt && !z.belegt);
        if (ImGui::Button(T("Laden"), ImVec2(92.0f * g_dpi, 0.0f)))
            g_verbindung.StandAuftrag(core::kStandLaden, (uint8_t)s);
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    // Schnellstaende X.n: Auswahlliste, neueste zuerst. "Speichern" legt den
    // naechsten an (wie L + R + Unten), "Laden" laedt den gewaehlten.
    {
        struct Eintrag { int slot; int folge; };
        std::vector<Eintrag> xs;
        for (int s = app::State::kStandSlotX0; s < app::State::kStandSlots; ++s)
            if (g_state.stand_slots[s].belegt)
                xs.push_back({s, g_state.stand_slots[s].folge});
        std::sort(xs.begin(), xs.end(),
                  [](const Eintrag& a, const Eintrag& b) { return a.folge > b.folge; });
        static int gewaehlt_slot = -1;
        bool gefunden = false;
        for (const Eintrag& e : xs) if (e.slot == gewaehlt_slot) gefunden = true;
        if (!gefunden) gewaehlt_slot = xs.empty() ? -1 : xs.front().slot;

        auto Beschriftung = [](int slot) {
            const app::State::StandSlot& z = g_state.stand_slots[slot];
            return "X." + std::to_string(z.folge) + "   " + z.zeit + "   " + z.spielzeit;
        };

        ImGui::PushID("x");
        ImGui::AlignTextToFramePadding();
        if (theme::mono) ImGui::PushFont(theme::mono, 0.0f);
        ImGui::TextUnformatted("X");
        ImGui::SameLine(0.0f, 12.0f * g_dpi);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - breite - 12.0f * g_dpi);
        const std::string vorschau = !g_state.stand_bekannt ? std::string("?")
                                   : gewaehlt_slot < 0      ? std::string(T("leer"))
                                                            : Beschriftung(gewaehlt_slot);
        if (ImGui::BeginCombo("##x", vorschau.c_str())) {
            for (const Eintrag& e : xs) {
                const bool sel = e.slot == gewaehlt_slot;
                if (ImGui::Selectable(Beschriftung(e.slot).c_str(), sel)) gewaehlt_slot = e.slot;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (theme::mono) ImGui::PopFont();

        ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - breite);
        ImGui::BeginDisabled(!frei);
        if (ImGui::Button(T("Speichern"), ImVec2(92.0f * g_dpi, 0.0f)))
            g_verbindung.StandAuftrag(core::kStandSpeichern, core::kStandXNeu);
        ImGui::SameLine();
        ImGui::BeginDisabled(gewaehlt_slot < 0);
        if (ImGui::Button(T("Laden"), ImVec2(92.0f * g_dpi, 0.0f)))
            g_verbindung.StandAuftrag(core::kStandLaden, (uint8_t)gewaehlt_slot);
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    Luft(2.0f);
    if (gesperrt)
        Gedaempft(T("Im Hardcore-Modus gesperrt."));
    else if (g_state.stand_laeuft)
        Gedaempft(T("Konsole arbeitet \xE2\x80\xA6 das Spiel steht so lange still."));
    else
        Gedaempft(T("L + R + Unten am Pad legt einen neuen Schnellstand X.n an (die letzten 10 bleiben)."));
    GruppeEnde();
}

void ZeichneEinrichtung() {
    if (GruppeBeginn(T("SAROO einrichten"), T("Firmware und Spiele\xE2\x80\xA6")))
        g_einrichtung.Oeffnen(g_state.rom_ordner);

    // Beschreibung in voller Breite. Der zweite Satz nennt den Weg ohne
    // Voreinrichtung: erster Spielstart erkennt und traegt den Hook ein,
    // zweiter aktiviert ihn.
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kSehrLeise));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(T("Patcht einmalig die Firmware der SAROO auf der SD-Karte. Auf "
                             "Wunsch richtet es auch den Hook f\xC3\xBCr einen kompletten "
                             "ROM-Ordner im Voraus ein. Ohne das ein Spiel zweimal starten: Beim "
                             "ersten Start wird der Hook eingetragen, beim zweiten ist er aktiv."));
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    GruppeEnde();
}

void ZeichneAchievements() {
    theme::Marke(T("Achievements"));
    if (!g_state.achievements.empty()) {
        char rechts[64];
        std::snprintf(rechts, sizeof(rechts), "%d / %d  \xC2\xB7  %d / %d",
                      g_state.freigeschaltet(), (int)g_state.achievements.size(),
                      g_state.punkte_erreicht(), g_state.punkte_gesamt());
        if (theme::mono) ImGui::PushFont(theme::mono, 0.0f);
        const float b = ImGui::CalcTextSize(rechts).x;
        ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - b);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kSehrLeise));
        ImGui::TextUnformatted(rechts);
        ImGui::PopStyleColor();
        if (theme::mono) ImGui::PopFont();
    }

    Luft(4.0f);

    float hoehe = ImGui::GetContentRegionAvail().y;
    if (hoehe < 60.0f * g_dpi) hoehe = 60.0f * g_dpi;
    if (ImGui::BeginTable("ach", 3,
            ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_SizingStretchProp, ImVec2(0.0f, hoehe))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(T("Titel"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(T("Punkte"), ImGuiTableColumnFlags_WidthFixed, 60.0f * g_dpi);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 90.0f * g_dpi);

        if (theme::klein) ImGui::PushFont(theme::klein, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kSehrLeise));
        ImGui::TableHeadersRow();
        ImGui::PopStyleColor();
        if (theme::klein) ImGui::PopFont();

        for (const app::Achievement& a : g_state.achievements) {
            ImGui::TableNextRow();
            const ImU32 farbe = a.freigeschaltet ? theme::kText : theme::kLeise;
            const ImU32 neben = a.freigeschaltet ? theme::kMessing : theme::kSehrLeise;

            ImGui::TableSetColumnIndex(0);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(farbe));
            // Ganze Zeile anklickbar: oeffnet die Seite des Achievements bei
            // RetroAchievements. Beim Ueberfahren steht die Beschreibung da.
            ImGui::PushID(a.id);
            if (ImGui::Selectable(a.titel.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                wchar_t url[96];
                std::swprintf(url, 96, L"https://retroachievements.org/achievement/%d", a.id);
                OeffneSeite(url);
            }
            if (ImGui::IsItemHovered() && !a.beschreibung.empty())
                ImGui::SetTooltip("%s", a.beschreibung.c_str());
            ImGui::PopID();
            ImGui::PopStyleColor();

            ImGui::TableSetColumnIndex(1);
            if (theme::mono) ImGui::PushFont(theme::mono, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(neben));
            char p[16]; std::snprintf(p, sizeof(p), "%d", a.punkte);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                 ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(p).x);
            ImGui::TextUnformatted(p);
            ImGui::PopStyleColor();
            if (theme::mono) ImGui::PopFont();

            ImGui::TableSetColumnIndex(2);
            if (theme::klein) ImGui::PushFont(theme::klein, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(neben));
            ImGui::TextUnformatted(a.freigeschaltet ? T("freigeschaltet") : T("gesperrt"));
            ImGui::PopStyleColor();
            if (theme::klein) ImGui::PopFont();
        }
        ImGui::EndTable();
    }
}

void ZeichneWerteUndLog() {
    // Aufteilung: Beschriftung, Tick-Zeile, ein umrandeter Kasten fuer die
    // Werte, darunter "Log:" und ein zweiter Kasten.
    theme::Marke(T("Live-Werte (letzter WATCH_REPORT):"));

    if (theme::mono) ImGui::PushFont(theme::mono, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kLeise));
    if (g_state.daten_empfangen) {
        ImGui::Text(T("tick: %u  \xC2\xB7  %.0f/s  \xC2\xB7  %d CRC-Fehler"),
                    g_state.bildzaehler, g_state.bilder_pro_sekunde, g_state.crc_fehler);
        // Die Frage hinter der Rate ist meist „lebt der Hook noch". Das
        // beantwortet die Zeit seit der letzten Wertaenderung direkt. Passt es
        // nicht mehr in die Zeile, steht es darunter statt abgeschnitten.
        char aenderung[96];
        if (g_state.sekunden_seit_aenderung < 0.0f)
            std::snprintf(aenderung, sizeof(aenderung), "%s", T("  \xC2\xB7  noch keine Änderung"));
        else
            std::snprintf(aenderung, sizeof(aenderung), T("  \xC2\xB7  Änderung vor %.0f s"),
                          g_state.sekunden_seit_aenderung);
        ImGui::SameLine(0.0f, 0.0f);
        if (ImGui::CalcTextSize(aenderung).x > ImGui::GetContentRegionAvail().x) {
            ImGui::NewLine();
            ImGui::TextUnformatted(aenderung + 6);     // ohne führenden Trenner
        } else {
            ImGui::TextUnformatted(aenderung);
        }
        // Aenderungsprotokoll: jedes Bild wird ausgewertet. Dazu, was die
        // Erfassung auf der Saturn je Bild kostet (ein Bild hat 16 700 us).
        // Messen kann das nur ein FPGA mit Zeitgeber; der offizielle meldet 0.
        if (g_state.prot_aktiv) {
            if (g_state.prot_zeit_da && g_state.prot_max_us > 0)
                ImGui::Text(T("jedes Bild  \xC2\xB7  Erfassung %u us, höchstens %u us  \xC2\xB7  %ld Lücken"),
                            g_state.prot_dauer_us, g_state.prot_max_us, g_state.prot_luecken);
            else
                ImGui::Text(T("jedes Bild  \xC2\xB7  %ld Lücken"), g_state.prot_luecken);
        } else {
            ImGui::TextUnformatted(T("Stichprobe alle 200 ms (kein Änderungsprotokoll)"));
        }
    } else {
        ImGui::TextUnformatted("tick: -");
    }
    ImGui::PopStyleColor();
    if (theme::mono) ImGui::PopFont();

    // Verhaeltnis Wertekasten 8 zu Protokoll 14: Das Protokoll bekommt den
    // groesseren Teil.
    const float rest = ImGui::GetContentRegionAvail().y;
    const float kasten = (rest - 30.0f * g_dpi) * 0.38f;
    const float h = kasten > 60.0f * g_dpi ? kasten : 60.0f * g_dpi;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(theme::kTabGrund));
    if (ImGui::BeginChild("werte", ImVec2(0, h), ImGuiChildFlags_Borders)) {
        if (!g_state.daten_empfangen) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::ColorConvertU32ToFloat4(theme::kSehrLeise));
            ImGui::TextWrapped("%s", T("Hier erscheinen die Werte aus dem Spielspeicher, sobald "
                                       "Frames vom Ger\xC3\xA4t ankommen."));
            ImGui::PopStyleColor();
        } else if (g_state.werte.empty()) {
            // Es kommen Frames, aber nur die Kennungs-Eintraege. Das ist der
            // Normalfall bei einem Spiel ohne Achievement-Set: Dann gibt es
            // keine Speicherstellen, die zu beobachten waeren.
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::ColorConvertU32ToFloat4(theme::kBernstein));
            ImGui::TextWrapped("%s", T("Keine Speicherstellen beobachtet."));
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::ColorConvertU32ToFloat4(theme::kLeise));
            ImGui::TextWrapped("%s", T(
                "Der Hook l\xC3\xA4uft und meldet die Spielkennung, aber es wurde keine "
                "Watch-Liste installiert. Das passiert, wenn es f\xC3\xBCr dieses Spiel "
                "kein Achievement-Set gibt \xE2\x80\x94 dann ist auch nichts zu "
                "beobachten. Sobald ein Spiel mit Set l\xC3\xA4uft, stehen hier seine "
                "Speicherstellen."));
            ImGui::PopStyleColor();
        } else {
            if (theme::mono) ImGui::PushFont(theme::mono, 0.0f);
            for (const app::Watchwert& w : g_state.werte) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImGui::ColorConvertU32ToFloat4(theme::kSehrLeise));
                ImGui::Text("0x%08X", w.adresse);
                ImGui::PopStyleColor();
                char v[24];
                std::snprintf(v, sizeof(v), "%04X %04X", w.wert >> 16, w.wert & 0xFFFF);
                ImGui::SameLine(ImGui::GetCursorPosX() +
                                ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(v).x);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(
                    w.geaendert ? theme::kMessing : theme::kLeise));
                ImGui::TextUnformatted(v);
                ImGui::PopStyleColor();
            }
            if (theme::mono) ImGui::PopFont();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    Luft(6.0f);
    theme::Marke(T("Log:"));

    // Das Protokoll hat in jedem Farbschema dieselben Farben und einen dunklen
    // Grund, auch im hellen Schema.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(theme::kLogGrund));
    if (ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        if (theme::mono) ImGui::PushFont(theme::mono, 0.0f);
        for (const app::Logzeile& z : g_state.log) {
            ImU32 f = theme::kLogNormal;
            if (z.rang == app::Rang::Gut)     f = theme::kLogGut;
            if (z.rang == app::Rang::Warnung) f = theme::kLogWarnung;
            if (z.rang == app::Rang::Wichtig) f = theme::kLogWichtig;
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(f));
            ImGui::TextWrapped("%s", z.text.c_str());
            ImGui::PopStyleColor();
        }
        if (theme::mono) ImGui::PopFont();
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void DrawUi() {
    // Ergebnisse der Nebenlaeufer einsammeln, bevor gezeichnet wird.
    const std::string kennung_vorher = g_state.spiel_kennung;
    static bool war_verbunden = false;
    g_verbindung.Abholen(g_state);
    SuchergebnisAbholen();

    // Spiel auf der Konsole neu gestartet: Stand der Achievements neu holen,
    // damit Aenderungen auf der RA-Webseite (etwa ein Zuruecksetzen) ankommen.
    if (g_state.spiel_neustart) {
        g_state.spiel_neustart = false;
        g_sitzung.NeuLaden();
    }

    // Verbindung weg: auch die Sitzung vergisst das Spiel, sonst bliebe die
    // Achievement-Liste eines Spiels stehen, das niemand mehr beobachtet.
    if (war_verbunden && !g_state.verbunden) {
        g_sitzung.SpielVergessen();
        g_state.achievements.clear();
        g_hook_wartet_seit = 0;
    }
    war_verbunden = g_state.verbunden;

    // Meldet sich ein anderes Spiel, wird sein Achievement-Set nachgeladen.
    // Kennung, Name und Pruefsumme kommen alle aus dem Disc-Kopf, den die
    // laufende Konsole gemeldet hat.
    //
    // Ausloeser ist die Pruefsumme, nicht nur die Kennung: Nach einem
    // Spielwechsel kann zuerst eine falsche Pruefsumme bei richtiger Kennung
    // kommen, die richtige gleich danach. Laeuft
    // noch eine Anfrage, kehrt SpielLadenStarten sofort zurueck; dann greift es
    // im naechsten Bild erneut.
    (void)kennung_vorher;
    if (!g_state.spiel_kennung.empty() && !g_state.spiel_md5.empty() &&
        g_state.spiel_md5 != g_sitzung.GefragteMd5())
        g_sitzung.SpielLadenStarten(g_state.spiel_kennung, g_state.spiel_name,
                                    g_state.spiel_md5, g_state.hardcore);

    // Mit Aenderungsprotokoll Bild fuer Bild, wie ein
    // Emulator. Die Stichprobe aus der Mailbox wird dann nicht ausgewertet,
    // sonst saehe die Laufzeit Bilder doppelt.
    const bool hardcore_wirksam = HardcoreLageBestimmen(g_state) == HardcoreLage::Wirksam;
    {
        std::vector<Connection::ProtBild> bilder = g_verbindung.ProtokollAbholen();
        const int eintraege = (int)g_state.werte.size() + 1;
        for (const Connection::ProtBild& b : bilder)
            g_sitzung.AuswertenBild(b.werte, b.luecke, eintraege, hardcore_wirksam);
    }
    if (g_state.prot_aktiv) {
        // nichts weiter: ausgewertet wurde oben
    } else
    // Auswerten nur bei einem NEUEN Bild des Hooks. Dieselben Werte mehrfach
    // durch die Laufzeit zu schicken wuerde Trefferzaehler verfaelschen.
    if (g_state.daten_empfangen &&
        (!g_je_ausgewertet || g_state.bildzaehler != g_zuletzt_ausgewertet)) {
        g_zuletzt_ausgewertet = g_state.bildzaehler;
        g_je_ausgewertet = true;
        g_sitzung.Auswerten(g_state.werte,
                            g_state.hook == app::State::Hook::Laeuft,
                            HardcoreLageBestimmen(g_state) == HardcoreLage::Wirksam);
    }

    // Wechsel der Hardcore-Lage einmal ins Protokoll, solange der Hook laeuft.
    {
        static HardcoreLage gemeldet = HardcoreLage::Aus;
        const HardcoreLage lage = (g_state.hook == app::State::Hook::Laeuft)
                                      ? HardcoreLageBestimmen(g_state) : HardcoreLage::Aus;
        if (lage != gemeldet) {
            gemeldet = lage;
            if (lage == HardcoreLage::Wirksam)
                g_state.Protokoll(T("Hardcore wirksam: Die Konsole meldet gesperrte "
                                    "Speicherstände."), app::Rang::Gut);
            else if (lage == HardcoreLage::NichtBestaetigt)
                g_state.Protokoll(T("Hardcore ist eingeschaltet, zählt aber erst, wenn die "
                                    "Konsole gesperrte Speicherstände meldet. Bis dahin werden "
                                    "Freischaltungen als Softcore gemeldet. Spiel einmal neu "
                                    "starten."), app::Rang::Warnung);
            else if (lage == HardcoreLage::StandGeladen)
                g_state.Protokoll(T("Seit dem Spielstart wurde ein Speicherstand geladen. "
                                    "Freischaltungen werden bis zum nächsten Spielstart als "
                                    "Softcore gemeldet."), app::Rang::Warnung);
        }
    }
    g_sitzung.Abholen(g_state);

    for (const app::Freischaltung& f : g_state.neue_freischaltungen) FreischaltungZeigen(f);
    g_state.neue_freischaltungen.clear();

    // Hook automatisch nachtragen.
    //
    // Lage: Das Spiel ist erkannt und sein Achievement-Set geladen, aber es
    // kommen keine Watch-Werte -- fuer dieses Spiel steht also kein Hook in der
    // saroocfg.txt. Statt den ROM-Ordner einrichten zu lassen, traegt das
    // Werkzeug ihn selbst nach. Beim naechsten Start dieses Spiels
    // ist er aktiv. Die drei Sekunden Wartezeit verhindern, dass gleich nach
    // dem Verbinden losgeschrieben wird, bevor der erste Bericht da sein kann.
    //
    // Dasselbe, wenn der Hook zwar laeuft, die Sperre der Speicherstaende aber
    // nicht zur Hardcore-Einstellung passt: Dann geht der Hook-Satz mit der
    // richtigen Sperre noch einmal raus und wirkt beim naechsten Spielstart.
    //
    // Nur wenn der Hook noch NIE Werte geliefert hat (Unbekannt), nicht wenn er
    // gerade steht: Steht er, lädt das Spiel womöglich gerade von der CD, und
    // ein Hook-Satz jetzt ließe die SAROO mitten im Spiel auf die Karte
    // schreiben.
    const bool sperre_passt = g_state.hardcore == SaturnMeldetSperre(g_state);
    const bool hook_fehlt = g_state.hook == app::State::Hook::Unbekannt;
    const bool sperre_nachtragen = g_state.hook == app::State::Hook::Laeuft && !sperre_passt;
    // Nachfassen statt einmal und nie wieder.
    //
    // Wer einen Hook-Satz im RetroAchievements-Eintrag der SAROO loescht,
    // bekommt automatisch einen neuen: Alle 30 Sekunden wird es erneut
    // versucht, solange der Hook keine Werte liefert.
    static unsigned g_hook_zuletzt = 0;
    const unsigned jetzt_ms = (unsigned)::GetTickCount();
    const bool abstand_ok = (g_hook_zuletzt == 0) || (jetzt_ms - g_hook_zuletzt >= 30000);
    if (g_state.verbunden && g_state.spiel_erkannt &&
        !g_state.achievements.empty() &&
        (hook_fehlt || sperre_nachtragen) &&
        !g_sitzung.Beschaeftigt() && abstand_ok) {
        const unsigned jetzt = jetzt_ms;
        if (g_hook_wartet_seit == 0) g_hook_wartet_seit = jetzt;
        else if (jetzt - g_hook_wartet_seit >= 3000) {
            g_hook_zuletzt = jetzt;
            // Die innere Sperre in HookNachtragen() ebenfalls loesen, sonst
            // bleibt der erneute Versuch wirkungslos.
            g_sitzung.HookNachtragenErlauben();
            // Erste Wahl: die SAROO schreibt selbst. Nur wenn die Verbindung
            // steht, sonst kommt nichts an und es geht über die Karte am PC.
            g_sitzung.HookNachtragen(
                [](const std::string& kennung, const std::string& inhalt) {
                    if (!g_verbindung.Laeuft()) return false;
                    g_verbindung.HookSenden(kennung, inhalt);
                    return true;
                },
                g_state.hardcore);
        }
    } else {
        g_hook_wartet_seit = 0;
    }

    // Vorgemerkte Hooks schreiben, sobald die Karte wieder am PC steckt.
    // Alle drei Sekunden nachsehen reicht; haeufiger waere nur Plattenzugriff
    // ohne Nutzen.
    if (g_sitzung.WartendeHooks() > 0) {
        const unsigned jetzt = (unsigned)::GetTickCount();
        if (jetzt - g_karte_geprueft >= 3000) {
            g_karte_geprueft = jetzt;
            g_sitzung.HooksNachreichen();
        }
    }

    // Ist die Freischalt-Karte im Werkzeug gewaehlt, bleibt unten ihr Streifen
    // frei. Siehe ZeichneFensterKarte.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float streifen = g_karte_im_werkzeug ? kStreifen * g_dpi : 0.0f;
    if (streifen > 0.0f)
        ImGui::GetBackgroundDrawList()->AddRectFilled(
            ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - streifen),
            ImVec2(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y),
            IM_COL32(0, 0, 0, 255));
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, std::max(vp->WorkSize.y - streifen, 200.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f * g_dpi, 0.0f));
    ImGui::Begin("##haupt", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_MenuBar);

    static bool erst = true;
    #define SCHRITT(x) do { if (erst) Log("  -> " #x); x; } while (0)

    // Menueleiste oben links, wie in mega-raw. Die Dialoge werden ausserhalb
    // der Leiste geoeffnet, damit ihre Kennungen nicht vom Menue abhaengen.
    bool optionen_auf = false, kofi_auf = false;
    SCHRITT(ZeichneMenue(&optionen_auf, &kofi_auf));
    SCHRITT(ZeichneOptionen(optionen_auf));
    SCHRITT(ZeichneKofi(kofi_auf));
    SCHRITT(ZeichneUpdate());

    // Reihenfolge nach der tatsaechlichen Arbeitsfolge: anmelden, Karte
    // einrichten, dann verbinden, dann laeuft das Spiel.
    SCHRITT(ZeichneKopf());
    SCHRITT(ZeichneAnmeldung());
    SCHRITT(ZeichneEinrichtung());
    SCHRITT(ZeichneVerbindung());
    SCHRITT(ZeichneSpiel());
    SCHRITT(ZeichneSpeicherstaende());

    Luft(12.0f);
    // Unten sollen Achievement-Liste und Protokoll mindestens 330 Punkte
    // bekommen; reicht die Hoehe dafuer nicht, waechst das Fenster.
    FensterWachsen(ImGui::GetCursorPosY() + 330.0f * g_dpi + streifen);
    float rest = ImGui::GetContentRegionAvail().y - 8.0f * g_dpi;
    if (rest < 120.0f * g_dpi) rest = 120.0f * g_dpi;

    if (ImGui::BeginTable("unten", 2,
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_SizingStretchProp, ImVec2(0.0f, rest))) {
        // Haelfte/Haelfte.
        ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("r", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableNextRow();

        const float innen = rest - 12.0f * g_dpi;

        ImGui::TableSetColumnIndex(0);
        if (ImGui::BeginChild("links", ImVec2(0.0f, innen))) SCHRITT(ZeichneAchievements());
        ImGui::EndChild();

        ImGui::TableSetColumnIndex(1);
        ImGui::Indent(14.0f * g_dpi);
        if (ImGui::BeginChild("rechts", ImVec2(0.0f, innen))) SCHRITT(ZeichneWerteUndLog());
        ImGui::EndChild();
        ImGui::Unindent(14.0f * g_dpi);

        ImGui::EndTable();
    }

    if (erst) Log("  -> alles gezeichnet");
    erst = false;
    #undef SCHRITT

    ImGui::End();
    ImGui::PopStyleVar();

    g_einrichtung.Zeichnen(g_state, g_sitzung.Client(),
                           g_sitzung.IndexVeraenderlich(), g_dpi);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    // Nur eine Instanz. Zwei offene Fenster konkurrieren um denselben
    // Anschluss, und dann ist unklar, welches was anzeigt. Ein
    // zweiter Start holt das laufende Fenster nach vorn und endet.
    const HANDLE einzeln = ::CreateMutexW(nullptr, TRUE, L"RawTurnEinzelneInstanz");
    if (einzeln && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND vorhanden = ::FindWindowW(L"SaturnRaMonitor", nullptr)) {
            if (::IsIconic(vorhanden)) ::ShowWindow(vorhanden, SW_RESTORE);
            ::SetForegroundWindow(vorhanden);
        }
        ::CloseHandle(einzeln);
        return 0;
    }

    LogOeffnen();
    ::SetUnhandledExceptionFilter(AufAbsturz);
    Log("start");
    {
        // Sprache so frueh wie moeglich: Schon die erste Fehlermeldung soll in
        // der gewaehlten Sprache erscheinen.
        core::Einstellungen e;
        e.Laden();
        i18n::SpracheSetzen(i18n::AusKennung(e.sprache.c_str()));
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
    // Katze als Programmsymbol, Ressource 1 aus app.rc. Gross fuer Alt+Tab und
    // Taskleiste, klein fuer die Titelleiste.
    wc.hIcon = (HICON)::LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                   ::GetSystemMetrics(SM_CXICON),
                                   ::GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)::LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                     ::GetSystemMetrics(SM_CXSMICON),
                                     ::GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"SaturnRaMonitor";
    ::RegisterClassExW(&wc);

    // Fenstertitel wie in mega-raw: Name und Build-Nummer.
    wchar_t titel[64];
    std::swprintf(titel, 64, L"RAW-TURN  (Build %d)", buildinfo::kBuild);

    // 870 statt 810 hoch: Menueleiste und Portrait kosten rund 60 Pixel, die
    // sonst unten bei Achievements und Protokoll fehlen wuerden.
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, titel,
                                WS_OVERLAPPEDWINDOW, 100, 40, 880, 870,
                                nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    g_fenster = hwnd;

    if (!CreateGlContext(hwnd)) {
        ::MessageBoxA(nullptr, T("OpenGL-Kontext konnte nicht angelegt werden."),
                      "RAW-TURN", MB_ICONERROR);
        ::DestroyWindow(hwnd);
        return 1;
    }

    {
        const char* ver = (const char*)::glGetString(GL_VERSION);
        const char* ren = (const char*)::glGetString(GL_RENDERER);
        char t[256];
        std::snprintf(t, sizeof(t), "opengl %s / %s",
                      ver ? ver : "(nichts)", ren ? ren : "(nichts)");
        Log(t);
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);
    Log("fenster und opengl stehen");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    Log("imgui-kontext angelegt");

    g_dpi = (float)::GetDpiForWindow(hwnd) / 96.0f;
    if (g_dpi < 0.5f || g_dpi > 4.0f) g_dpi = 1.0f;

    {
        // Zuletzt gewaehltes Farbschema. Apply() setzt es dann mit.
        core::Einstellungen e;
        e.Laden();
        theme::StilSetzen(theme::StilAusKennung(e.stil.c_str()));
        g_karte_im_werkzeug = e.anzeige == "werkzeug";
    }

    // Startgroesse 880 x 860 logische Punkte, mit Freischalt-Karte im Werkzeug
    // plus deren Streifen, hoechstens so viel, wie der Arbeitsbereich des
    // Bildschirms hergibt. Kommt Inhalt dazu, waechst das Fenster mit, siehe
    // FensterWachsen.
    {
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        if (::GetMonitorInfoW(::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            const RECT& a = mi.rcWork;
            const float streifen = g_karte_im_werkzeug ? kStreifen : 0.0f;
            int b = (int)(880.0f * g_dpi), h = (int)((860.0f + streifen) * g_dpi);
            if (b > a.right - a.left)  b = a.right - a.left;
            if (h > a.bottom - a.top)  h = a.bottom - a.top;
            const int x = a.left + ((a.right - a.left) - b) / 2;
            ::SetWindowPos(hwnd, nullptr, x, a.top, b, h, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    theme::Apply(g_dpi);
    Log("schriften und farben gesetzt");
    UpdatePruefen();

    ImGui_ImplWin32_InitForOpenGL(hwnd);
    ImGui_ImplOpenGL2_Init();
    Log("anbindungen bereit");
    LogoHochladen();
    Log(g_logo ? "logo hochgeladen" : "logo fehlt");

    // Erste Protokollzeilen: Build-Kennung, damit auf jedem Bildschirmfoto
    // steht, welcher Stand laeuft -- und ein ehrlicher Hinweis auf den Stand
    // der Arbeit, statt einen fertigen Eindruck vorzutaeuschen.
    {
        char t[160];
        std::snprintf(t, sizeof(t), "RAW-TURN \xE2\x80\x94 %s", buildinfo::string());
        g_state.Protokoll(t, app::Rang::Wichtig);
    }
    {
        char t[220];
        std::snprintf(t, sizeof(t),
            T("Zuletzt eingerichtet: %d Spiel(e). Erkannt wird trotzdem aus dem "
              "laufenden Spiel, nicht aus dieser Liste."),
            (int)g_sitzung.Index().Anzahl());
        g_state.Protokoll(t);
    }
    PortsAuflisten(false);

    bool erstes_bild = true;
    bool running = true;
    while (running) {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        if (erstes_bild) Log("erstes bild beginnt");
        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawUi();
        ZeichneFensterKarte();
        ImGui::Render();
        if (erstes_bild) { Log("erstes bild gezeichnet"); erstes_bild = false; }

        const ImVec4 g = ImGui::ColorConvertU32ToFloat4(theme::kGrund);
        ::glClearColor(g.x, g.y, g.z, 1.0f);
        ::glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        ::SwapBuffers(g_dc);
    }

    Log("beenden");
    g_verbindung.Beenden();
    g_sitzung.Beenden();
    if (g_suchthread.joinable()) g_suchthread.join();
    popup::Beenden();
    if (g_logo) ::glDeleteTextures(1, &g_logo);
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DestroyGlContext(hwnd);
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, inst);
    Log("sauber beendet");
    LogSchliessen();
    return 0;
}
