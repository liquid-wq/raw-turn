#include "theme.h"

#include <windows.h>

#include "core/i18n.h"   // nur TK(): markiert die Schemanamen fuer die Uebersetzung

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace theme {

ImFont* ui = nullptr;
ImFont* bold = nullptr;
ImFont* klein = nullptr;
ImFont* mono = nullptr;
ImFont* kursiv = nullptr;

ImU32 kGrund, kFeld, kLinie, kFein, kText, kLeise, kSehrLeise, kMessing;
ImU32 kSalbei, kBernstein, kRost, kKarte, kKartenrand, kMarke, kTabGrund, kLogGrund;
bool  gMarkeMonoGross = false;

namespace {

// Schriften aus dem Windows-Ordner. Sie liegen auf jedem System, deshalb muss
// nichts mitgeliefert werden -- das Programm bleibt eine einzelne Datei.
// Die Reihenfolge ist eine Vorzugsliste; die erste vorhandene gewinnt.
const char* const kSans[]      = { "segoeui.ttf", "tahoma.ttf", "arial.ttf" };
const char* const kSansHalb[]  = { "seguisb.ttf", "segoeuib.ttf", "arialbd.ttf" };
const char* const kMono[]      = { "CascadiaMono.ttf", "consola.ttf", "cour.ttf" };
const char* const kKursiv[]    = { "segoeuii.ttf", "ariali.ttf" };

std::string Pfad(const char* datei) {
    char win[MAX_PATH] = {};
    ::GetWindowsDirectoryA(win, MAX_PATH);
    return std::string(win) + "\\Fonts\\" + datei;
}

ImFont* Lade(const char* const* kandidaten, int anzahl, float groesse) {
    ImGuiIO& io = ImGui::GetIO();
    for (int i = 0; i < anzahl; ++i) {
        const std::string p = Pfad(kandidaten[i]);
        if (::GetFileAttributesA(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (ImFont* f = io.Fonts->AddFontFromFileTTF(p.c_str(), groesse)) return f;
    }
    return nullptr;  // Aufrufer faellt auf die Standardschrift zurueck
}

// --- Paletten -------------------------------------------------------------
//
// `karte` 0 heisst: Gruppen nur durch Haarlinien getrennt, wie
// bisher. Die Zustandsfarben gut/Warnung/Fehler sind je Schema angepasst,
// damit sie auch auf hellem Grund lesbar bleiben.
struct Palette {
    const char* kennung;
    const char* name;
    unsigned grund, flaeche, karte, kartenrand, feld, rand, fein, text, leise, sehrleise,
             marke, akzent, knopf, gut, warnung, fehler, tabgrund, loggrund;
    bool mono_gross;
};

const Palette kPaletten[kStilAnzahl] = {
    {"heute", TK("Heute"),
     0x0d0e10, 0x191b20, 0,        0x24262c, 0x191b20, 0x24262c, 0x1c1e23, 0xecece9, 0x8b8b85, 0x5e5e59,
     0x8b8b85, 0xc8a765, 0x191b20, 0x83a78c, 0xc39352, 0xbb6a63, 0x0d0e10, 0x0d0e10, false},
    {"graphit", TK("Graphit"),
     0x16181c, 0x20232a, 0x1c1f24, 0x2d3139, 0x272b33, 0x434954, 0x2d3139, 0xeeece6, 0xb5b2a9, 0x908d85,
     0xc9c5bb, 0xd9b46e, 0x2b3038, 0x93bb9c, 0xd0a060, 0xc77a72, 0x191b20, 0x0d0e10, false},
    {"saturn-grau", TK("Saturn-Grau"),
     0x1c2029, 0x262b36, 0x212632, 0x353d4c, 0x2d3441, 0x4b5669, 0x353d4c, 0xe8ecf3, 0xadb6c6, 0x8791a4,
     0xc3cad7, 0x7ea6f5, 0x303848, 0x8fc3a0, 0xd0a060, 0xd08078, 0x1f242e, 0x0d0e10, false},
    {"bernstein", TK("Bernstein"),
     0x14120e, 0x1e1b15, 0x1a1712, 0x322b1f, 0x25201a, 0x50452f, 0x322b1f, 0xf1e7d3, 0xbdac8d, 0x938467,
     0xc8a765, 0xe3b45f, 0x2b251c, 0x9dbd92, 0xd9a55a, 0xc9786c, 0x17150f, 0x0f0d0a, true},
    {"hell", TK("Hell"),
     0xe7e9ed, 0xf6f7f9, 0xf6f7f9, 0xcdd2da, 0xffffff, 0x9ea6b3, 0xcdd2da, 0x15181d, 0x454c57, 0x5f6672,
     0x2f353e, 0x7a5716, 0xffffff, 0x2f6b44, 0x8a5a14, 0x9c3a32, 0xffffff, 0x111317, false},
};

Stil gAktuell = Stil::Heute;

ImU32 Farbe(unsigned rgb) {
    return IM_COL32((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, 0xff);
}

// Mischen fuer Hover- und Druckzustaende: ein Stueck Richtung Textfarbe. So
// passt es auf dunklem wie auf hellem Grund, ohne je Schema eigene Werte.
ImVec4 Mische(ImU32 a, ImU32 b, float t) {
    const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a);
    const ImVec4 y = ImGui::ColorConvertU32ToFloat4(b);
    return ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t,
                  x.z + (y.z - x.z) * t, 1.0f);
}

}  // namespace

const char* StilName(Stil s)    { return kPaletten[(int)s].name; }
const char* StilKennung(Stil s) { return kPaletten[(int)s].kennung; }
Stil AktuellerStil()            { return gAktuell; }

Stil StilAusKennung(const char* kennung) {
    if (kennung)
        for (int i = 0; i < kStilAnzahl; ++i)
            if (std::strcmp(kPaletten[i].kennung, kennung) == 0) return (Stil)i;
    return Stil::Heute;
}

void StilSetzen(Stil stil) {
    gAktuell = stil;
    const Palette& p = kPaletten[(int)stil];

    kGrund      = Farbe(p.grund);
    kFeld       = Farbe(p.feld);
    kLinie      = Farbe(p.rand);
    kFein       = Farbe(p.fein);
    kText       = Farbe(p.text);
    kLeise      = Farbe(p.leise);
    kSehrLeise  = Farbe(p.sehrleise);
    kMessing    = Farbe(p.akzent);
    kSalbei     = Farbe(p.gut);
    kBernstein  = Farbe(p.warnung);
    kRost       = Farbe(p.fehler);
    kKarte      = p.karte ? Farbe(p.karte) : 0;
    kKartenrand = Farbe(p.kartenrand);
    kMarke      = Farbe(p.marke);
    kTabGrund   = Farbe(p.tabgrund);
    kLogGrund   = Farbe(p.loggrund);
    gMarkeMonoGross = p.mono_gross;

    if (!ImGui::GetCurrentContext()) return;

    auto C = [](ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); };
    const ImU32 flaeche = Farbe(p.flaeche);
    const ImU32 knopf   = Farbe(p.knopf);

    ImVec4* f = ImGui::GetStyle().Colors;
    f[ImGuiCol_WindowBg]            = C(kGrund);
    f[ImGuiCol_ChildBg]             = C(kGrund);
    f[ImGuiCol_PopupBg]             = C(flaeche);
    f[ImGuiCol_Border]              = C(kLinie);
    f[ImGuiCol_BorderShadow]        = ImVec4(0, 0, 0, 0);
    f[ImGuiCol_Text]                = C(kText);
    f[ImGuiCol_TextDisabled]        = C(kSehrLeise);
    f[ImGuiCol_FrameBg]             = C(kFeld);
    f[ImGuiCol_FrameBgHovered]      = Mische(kFeld, kText, 0.06f);
    f[ImGuiCol_FrameBgActive]       = Mische(kFeld, kText, 0.11f);
    f[ImGuiCol_Button]              = C(knopf);
    f[ImGuiCol_ButtonHovered]       = Mische(knopf, kText, 0.08f);
    f[ImGuiCol_ButtonActive]        = Mische(knopf, kText, 0.14f);
    f[ImGuiCol_Header]              = Mische(kGrund, kText, 0.06f);
    f[ImGuiCol_HeaderHovered]       = Mische(kGrund, kText, 0.10f);
    f[ImGuiCol_HeaderActive]        = Mische(kGrund, kText, 0.14f);
    f[ImGuiCol_Separator]           = C(kFein);
    f[ImGuiCol_SeparatorHovered]    = C(kLinie);
    f[ImGuiCol_SeparatorActive]     = C(kMessing);
    f[ImGuiCol_CheckMark]           = C(kMessing);
    f[ImGuiCol_SliderGrab]          = C(kMessing);
    f[ImGuiCol_SliderGrabActive]    = C(kMessing);
    f[ImGuiCol_ScrollbarBg]         = ImVec4(0, 0, 0, 0);
    f[ImGuiCol_ScrollbarGrab]       = C(kFein);
    f[ImGuiCol_ScrollbarGrabHovered]= C(kLinie);
    f[ImGuiCol_ScrollbarGrabActive] = C(kSehrLeise);
    // „Heute" bleibt exakt wie bisher: Kopfzeile auf Grund, kraeftigere Linie.
    f[ImGuiCol_TableHeaderBg]       = C(p.karte ? flaeche : kGrund);
    f[ImGuiCol_TableBorderStrong]   = C(p.karte ? kFein : kLinie);
    f[ImGuiCol_TableBorderLight]    = C(kFein);
    f[ImGuiCol_TableRowBg]          = C(kTabGrund);
    f[ImGuiCol_TableRowBgAlt]       = C(kTabGrund);
    f[ImGuiCol_TextSelectedBg]      = ImVec4(C(kMessing).x, C(kMessing).y, C(kMessing).z, 0.25f);
    f[ImGuiCol_PlotHistogram]       = C(kMessing);
    f[ImGuiCol_NavCursor]           = C(kMessing);
    f[ImGuiCol_MenuBarBg]           = C(flaeche);
    f[ImGuiCol_TitleBg]             = C(flaeche);
    f[ImGuiCol_TitleBgActive]       = C(flaeche);
    f[ImGuiCol_TitleBgCollapsed]    = C(flaeche);
    f[ImGuiCol_ModalWindowDimBg]    = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
    f[ImGuiCol_TextLink]            = C(kMessing);
}

void Apply(float dpi_scale) {
    if (dpi_scale < 0.5f) dpi_scale = 1.0f;

    ui    = Lade(kSans,     IM_ARRAYSIZE(kSans),     15.0f * dpi_scale);
    bold  = Lade(kSansHalb, IM_ARRAYSIZE(kSansHalb), 15.0f * dpi_scale);
    klein = Lade(kSans,     IM_ARRAYSIZE(kSans),     12.0f * dpi_scale);
    mono  = Lade(kMono,     IM_ARRAYSIZE(kMono),     13.0f * dpi_scale);
    kursiv = Lade(kKursiv,  IM_ARRAYSIZE(kKursiv),   15.0f * dpi_scale);
    if (ui) ImGui::GetIO().FontDefault = ui;

    ImGuiStyle& s = ImGui::GetStyle();
    // Knapp halten. Der obere Teil sind vier Gruppen; wird jede davon zu hoch,
    // bleibt fuer Achievements, Live-Werte und Protokoll nichts uebrig.
    s.WindowPadding     = ImVec2(0, 0);
    s.FramePadding      = ImVec2(9, 3);
    s.ItemSpacing       = ImVec2(10, 4);
    s.ItemInnerSpacing  = ImVec2(6, 4);
    s.CellPadding       = ImVec2(8, 3);
    s.ScrollbarSize     = 11.0f;
    s.GrabMinSize       = 9.0f;

    // Kanten zurueckhaltend: kleine Radien, duenne Linien.
    s.WindowBorderSize  = 0.0f;
    s.ChildBorderSize   = 1.0f;
    s.FrameBorderSize   = 1.0f;
    s.PopupBorderSize   = 1.0f;
    s.WindowRounding    = 0.0f;
    s.ChildRounding     = 4.0f;
    s.FrameRounding     = 4.0f;
    s.GrabRounding      = 3.0f;
    s.PopupRounding     = 4.0f;
    s.ScrollbarRounding = 6.0f;

    s.ScaleAllSizes(dpi_scale);

    StilSetzen(gAktuell);
}

// ------------------------------------------------------------------------

void Marke(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kMarke));
    if (gMarkeMonoGross) {
        // Wie auf einem Messgeraet: Monoschrift, Grossbuchstaben. Die
        // Beschriftungen sind reines ASCII, toupper genuegt.
        std::string gross(text);
        for (char& c : gross) c = (char)std::toupper((unsigned char)c);
        if (mono) ImGui::PushFont(mono, 0.0f);
        ImGui::TextUnformatted(gross.c_str());
        if (mono) ImGui::PopFont();
    } else {
        ImGui::TextUnformatted(text);
    }
    ImGui::PopStyleColor();
}

void Haarlinie() {
    ImGui::PushStyleColor(ImGuiCol_Separator, ImGui::ColorConvertU32ToFloat4(kFein));
    ImGui::Separator();
    ImGui::PopStyleColor();
}

void Feld(const char* beschriftung, const char* wert, ImFont* wertschrift) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kLeise));
    ImGui::TextUnformatted(beschriftung);
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 8.0f);
    if (wertschrift) ImGui::PushFont(wertschrift, 0.0f);
    ImGui::TextUnformatted(wert);
    if (wertschrift) ImGui::PopFont();
}

void Zustand(ImU32 farbe, const char* text) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    const float r = 3.0f * (ImGui::GetFontSize() / 16.0f);
    dl->AddCircleFilled(ImVec2(p.x + r + 1.0f, p.y + h * 0.5f), r, farbe);
    ImGui::Dummy(ImVec2(r * 2.0f + 6.0f, h));
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(farbe));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

}  // namespace theme
