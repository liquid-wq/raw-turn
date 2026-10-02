#include "unlock_popup.h"

#include <algorithm>
#include <windows.h>
#include <objidl.h>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include <mmsystem.h>

#include <cmath>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace popup {
namespace {

using namespace Gdiplus;

// Zeitachse in Millisekunden, wie in MEGA-RAW.
constexpr double kEinMs = 620.0;
constexpr double kStehMs = 4200.0;
constexpr double kAusMs = 480.0;
constexpr double kGesamtMs = kEinMs + kStehMs + kAusMs;

// Sichtbare Karte plus unsichtbarer Platz links fuer das Ueberschwingen der
// Einfahrkurve, sonst wird das Nachfedern an der Fensterkante abgeschnitten.
constexpr double kKarteW = 420.0;
constexpr double kReserve = 56.0;
constexpr double kH = 132.0;
constexpr double kPi = 3.14159265358979;

const Color kGold(255, 0xf5, 0xc2, 0x42);

struct Funke { double winkel, tempo, groesse, phase; };

struct Eintrag {
    std::wstring titel, beschreibung;
    int punkte = 0;
    std::string badge_png;
    bool englisch = false;
};

ULONG_PTR g_gdiplus = 0;
HWND g_fenster = nullptr;
float g_s = 1.0f;
double g_zeit = 0.0;
DWORD g_start = 0;
bool g_laeuft = false;
Eintrag g_aktuell;
Bitmap* g_badge = nullptr;
std::vector<Funke> g_funken;
std::deque<Eintrag> g_warteschlange;

constexpr UINT_PTR kTakt = 1;

std::wstring Breit(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

double AusMitNachfedern(double t) {
    const double s = 1.70158;
    t -= 1.0;
    return t * t * ((s + 1.0) * t + s) + 1.0;
}

void RundesRechteck(GraphicsPath& p, const RectF& r, REAL radius) {
    const REAL d = radius * 2.0f;
    p.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    p.CloseFigure();
}

Bitmap* BadgeLaden(const std::string& png) {
    if (png.empty()) return nullptr;
    HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, png.size());
    if (!h) return nullptr;
    std::memcpy(::GlobalLock(h), png.data(), png.size());
    ::GlobalUnlock(h);
    IStream* strom = nullptr;
    if (FAILED(::CreateStreamOnHGlobal(h, TRUE, &strom))) { ::GlobalFree(h); return nullptr; }

    Bitmap* roh = Bitmap::FromStream(strom);
    Bitmap* kopie = nullptr;
    if (roh && roh->GetLastStatus() == Ok) {
        // In eine eigene Bitmap umkopieren; die dekodierte haengt am Strom.
        kopie = new Bitmap(128, 128, PixelFormat32bppPARGB);
        Graphics g(kopie);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.DrawImage(roh, 0, 0, 128, 128);
    }
    delete roh;
    strom->Release();
    return kopie;
}

void Pokal(Graphics& g, const RectF& r) {
    SolidBrush grund(Color(255, 0x14, 0x18, 0x1e));
    Pen rand(kGold, 2.0f * g_s);
    g.FillEllipse(&grund, r);
    g.DrawEllipse(&rand, r);

    SolidBrush gold(kGold);
    const REAL w = r.Width, h = r.Height;
    const RectF kelch(r.X + w * 0.32f, r.Y + h * 0.24f, w * 0.36f, h * 0.30f);
    const REAL mitte = kelch.X + kelch.Width / 2.0f;
    PointF ecken[4] = {
        PointF(kelch.X, kelch.Y),
        PointF(kelch.X + kelch.Width, kelch.Y),
        PointF(mitte + kelch.Width * 0.18f, kelch.Y + kelch.Height),
        PointF(mitte - kelch.Width * 0.18f, kelch.Y + kelch.Height),
    };
    g.FillPolygon(&gold, ecken, 4);
    g.FillRectangle(&gold, RectF(r.X + w / 2.0f - w * 0.03f, r.Y + h * 0.54f, w * 0.06f, h * 0.10f));
    g.FillRectangle(&gold, RectF(r.X + w * 0.34f, r.Y + h * 0.64f, w * 0.32f, h * 0.07f));
    g.DrawArc(&rand, RectF(r.X + w * 0.20f, r.Y + h * 0.24f, w * 0.16f, h * 0.20f), 90.0f, 180.0f);
    g.DrawArc(&rand, RectF(r.X + w * 0.64f, r.Y + h * 0.24f, w * 0.16f, h * 0.20f), -90.0f, 180.0f);
}

void Text(Graphics& g, const std::wstring& s, const RectF& r, REAL pt, bool fett,
          const Color& farbe, StringAlignment ausrichtung = StringAlignmentNear) {
    Font schrift(L"Segoe UI", pt * 96.0f / 72.0f * g_s, fett ? FontStyleBold : FontStyleRegular,
                 UnitPixel);
    StringFormat format;
    format.SetAlignment(ausrichtung);
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetTrimming(StringTrimmingEllipsisCharacter);
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush pinsel(farbe);
    g.DrawString(s.c_str(), -1, &schrift, r, &format, &pinsel);
}

// Ein Bild der Animation zeichnen; liefert die Deckkraft 0..1.
double Malen(Graphics& g) {
    const double t = g_zeit;
    const REAL s = g_s;

    double vor = 0.0, deckkraft = 1.0;
    if (t < kEinMs) {
        vor = AusMitNachfedern(t / kEinMs);
    } else if (t < kEinMs + kStehMs) {
        vor = 1.0;
    } else {
        const double q = std::min(1.0, (t - kEinMs - kStehMs) / kAusMs);
        vor = 1.0 - q * q * q;
        deckkraft = 1.0 - q;
    }
    g.TranslateTransform((REAL)((1.0 - vor) * (kKarteW + 40.0)) * s, 0.0f);

    const RectF karte((REAL)(kReserve + 6.0) * s, 6.0f * s,
                      (REAL)(kKarteW - 12.0) * s, (REAL)(kH - 12.0) * s);
    const REAL cx = karte.X + karte.Width / 2.0f;
    const REAL cy = karte.Y + karte.Height / 2.0f;

    // Funken, nur waehrend des Erscheinens
    if (t < kEinMs + 500.0) {
        const double leben = std::min(1.0, t / (kEinMs + 500.0));
        for (const Funke& f : g_funken) {
            const double r = leben * 120.0 * f.tempo * s;
            const REAL px = (REAL)(cx - 120.0 * s + std::cos(f.winkel) * r);
            const REAL py = (REAL)(cy + std::sin(f.winkel) * r * 0.6);
            const double a = std::max(0.0, std::min(1.0, (1.0 - leben) * (0.5 + f.phase * 0.5)));
            SolidBrush b(Color((BYTE)(a * 255.0), 255, 200, 60));
            const REAL gr = (REAL)f.groesse * s;
            g.FillEllipse(&b, px - gr, py - gr, gr * 2.0f, gr * 2.0f);
        }
    }

    // Karte
    GraphicsPath pfad;
    RundesRechteck(pfad, karte, 14.0f * s);
    LinearGradientBrush grund(PointF(karte.X, karte.Y),
                              PointF(karte.X + karte.Width, karte.Y + karte.Height),
                              Color(255, 0x1c, 0x20, 0x28), Color(255, 0x0d, 0x10, 0x16));
    g.FillPath(&grund, &pfad);
    Pen rand(kGold, 2.0f * s);
    g.DrawPath(&rand, &pfad);

    // Glanz, der einmal ueber die Karte wandert
    if (t > 260.0 && t < 1500.0) {
        const double q = (t - 260.0) / 1240.0;
        const REAL gx = (REAL)(karte.X + q * karte.Width * 1.5 - karte.Width * 0.25);
        LinearGradientBrush glanz(PointF(gx - 60.0f * s, 0.0f), PointF(gx + 60.0f * s, 0.0f),
                                  Color(0, 255, 255, 255), Color(0, 255, 255, 255));
        Color farben[3] = {Color(0, 255, 255, 255), Color(46, 255, 245, 200), Color(0, 255, 255, 255)};
        REAL lagen[3] = {0.0f, 0.5f, 1.0f};
        glanz.SetInterpolationColors(farben, lagen, 3);
        Region alt;
        g.GetClip(&alt);
        g.SetClip(&pfad);
        g.FillRectangle(&glanz, karte);
        g.SetClip(&alt);
    }

    // Badge links, pulsiert leicht
    const double puls = 1.0 + 0.05 * std::sin(t / 220.0);
    const REAL bs = (REAL)(68.0 * puls) * s;
    const PointF bm(karte.X + 56.0f * s, cy);
    const RectF brect(bm.X - bs / 2.0f, bm.Y - bs / 2.0f, bs, bs);

    GraphicsPath schein;
    schein.AddEllipse(bm.X - bs * 0.9f, bm.Y - bs * 0.9f, bs * 1.8f, bs * 1.8f);
    PathGradientBrush sch(&schein);
    sch.SetCenterColor(Color(90, 0xf5, 0xc2, 0x42));
    Color aussen(0, 0xf5, 0xc2, 0x42);
    int anzahl = 1;
    sch.SetSurroundColors(&aussen, &anzahl);
    g.FillPath(&sch, &schein);

    if (g_badge) {
        GraphicsPath kreis;
        kreis.AddEllipse(brect);
        Region alt;
        g.GetClip(&alt);
        g.SetClip(&kreis);
        g.DrawImage(g_badge, brect);
        g.SetClip(&alt);
        Pen ring(kGold, 2.0f * s);
        g.DrawEllipse(&ring, brect);
    } else {
        Pokal(g, brect);
    }

    // Texte
    const REAL lx = karte.X + 108.0f * s;
    const REAL lw = karte.Width - 122.0f * s;
    Text(g, g_aktuell.englisch ? L"ACHIEVEMENT UNLOCKED!" : L"ACHIEVEMENT FREIGESCHALTET!",
         RectF(lx, karte.Y + 16.0f * s, lw, 16.0f * s), 9.0f, true, kGold);
    Text(g, g_aktuell.titel, RectF(lx, karte.Y + 36.0f * s, lw, 26.0f * s), 14.0f, true,
         Color(255, 255, 255, 255));
    Text(g, g_aktuell.beschreibung, RectF(lx, karte.Y + 64.0f * s, lw, 18.0f * s), 9.0f, false,
         Color(255, 0xb8, 0xc0, 0xcc));

    // Punkteleiste, laeuft waehrend der Standzeit voll
    const RectF leiste(lx, karte.Y + karte.Height - 26.0f * s, lw, 10.0f * s);
    GraphicsPath lp;
    RundesRechteck(lp, leiste, 5.0f * s);
    SolidBrush leer(Color(255, 0x2a, 0x2f, 0x38));
    g.FillPath(&leer, &lp);
    const double fuell = std::max(0.0, std::min(1.0, (t - kEinMs) / 900.0));
    if (fuell > 0.02) {
        const RectF voll(leiste.X, leiste.Y, (REAL)(leiste.Width * fuell), leiste.Height);
        GraphicsPath vp;
        RundesRechteck(vp, voll, std::min(5.0f * s, voll.Width / 2.0f));
        LinearGradientBrush lg(PointF(voll.X, 0.0f), PointF(voll.X + voll.Width + 1.0f, 0.0f),
                               kGold, Color(255, 0xff, 0xe6, 0x9a));
        g.FillPath(&lg, &vp);
    }
    if (g_aktuell.punkte > 0) {
        wchar_t p[32];
        ::swprintf(p, 32, L"%d %ls", g_aktuell.punkte, g_aktuell.englisch ? L"pts" : L"Pkt");
        Text(g, p, RectF(lx, leiste.Y - 16.0f * s, lw, 14.0f * s), 8.0f, true, kGold,
             StringAlignmentFar);
    }

    return deckkraft;
}

void Darstellen() {
    const int w = (int)std::lround((kKarteW + kReserve) * g_s);
    const int h = (int)std::lround(kH * g_s);

    Bitmap bild(w, h, PixelFormat32bppPARGB);
    double deckkraft;
    {
        Graphics g(&bild);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.Clear(Color(0, 0, 0, 0));
        deckkraft = Malen(g);
    }

    HDC bildschirm = ::GetDC(nullptr);
    HDC speicher = ::CreateCompatibleDC(bildschirm);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = ::CreateDIBSection(bildschirm, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib && bits) {
        BitmapData daten;
        Rect r(0, 0, w, h);
        if (bild.LockBits(&r, ImageLockModeRead, PixelFormat32bppPARGB, &daten) == Ok) {
            for (int y = 0; y < h; ++y)
                std::memcpy((BYTE*)bits + y * w * 4, (BYTE*)daten.Scan0 + y * daten.Stride,
                            (size_t)w * 4);
            bild.UnlockBits(&daten);
        }
        HGDIOBJ alt = ::SelectObject(speicher, dib);

        RECT flaeche;
        ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &flaeche, 0);
        POINT pos{flaeche.right - w - (int)(24 * g_s), flaeche.bottom - h - (int)(24 * g_s)};
        SIZE groesse{w, h};
        POINT quelle{0, 0};
        BLENDFUNCTION mischung{AC_SRC_OVER, 0,
                               (BYTE)std::lround(std::max(0.0, std::min(1.0, deckkraft)) * 255.0),
                               AC_SRC_ALPHA};
        ::UpdateLayeredWindow(g_fenster, bildschirm, &pos, &groesse, speicher, &quelle, 0,
                              &mischung, ULW_ALPHA);
        ::SelectObject(speicher, alt);
        ::DeleteObject(dib);
    }
    ::DeleteDC(speicher);
    ::ReleaseDC(nullptr, bildschirm);
}

void Ton() {
    wchar_t pfad[MAX_PATH];
    const DWORD n = ::GetModuleFileNameW(nullptr, pfad, MAX_PATH);
    std::wstring wav(pfad, n);
    const size_t trenner = wav.find_last_of(L"\\/");
    wav = (trenner == std::wstring::npos ? L"" : wav.substr(0, trenner + 1)) + L"achievement.wav";
    if (::GetFileAttributesW(wav.c_str()) != INVALID_FILE_ATTRIBUTES) {
        ::PlaySoundW(wav.c_str(), nullptr, SND_FILENAME | SND_ASYNC);
        return;
    }
    // Ohne WAV-Datei dieselbe Tonfolge wie MEGA-RAW.
    std::thread([]() {
        static const int melodie[][2] = {{587, 140}, {659, 140}, {698, 140}, {784, 140},
                                         {880, 320}, {1175, 200}, {880, 160}, {698, 140},
                                         {784, 140}, {880, 420}};
        for (const auto& note : melodie) ::Beep((DWORD)note[0], (DWORD)note[1]);
    }).detach();
}

void Starten(Eintrag e) {
    g_aktuell = std::move(e);
    delete g_badge;
    g_badge = BadgeLaden(g_aktuell.badge_png);

    std::mt19937 zufall((unsigned)::GetTickCount());
    std::uniform_real_distribution<double> v(0.0, 1.0);
    g_funken.clear();
    for (int i = 0; i < 34; ++i)
        g_funken.push_back({v(zufall) * 2.0 * kPi, v(zufall) * 2.2 + 0.6, v(zufall) * 2.2 + 1.0,
                            v(zufall)});

    g_zeit = 0.0;
    g_start = ::GetTickCount();
    g_laeuft = true;
    Darstellen();
    ::ShowWindow(g_fenster, SW_SHOWNOACTIVATE);
    ::SetTimer(g_fenster, kTakt, 16, nullptr);
    Ton();
}

LRESULT CALLBACK Fensterfunktion(HWND h, UINT nachricht, WPARAM w, LPARAM l) {
    if (nachricht == WM_TIMER && w == kTakt) {
        g_zeit = (double)(::GetTickCount() - g_start);
        if (g_zeit >= kGesamtMs) {
            ::KillTimer(h, kTakt);
            ::ShowWindow(h, SW_HIDE);
            g_laeuft = false;
            if (!g_warteschlange.empty()) {
                Eintrag naechster = std::move(g_warteschlange.front());
                g_warteschlange.pop_front();
                Starten(std::move(naechster));
            }
        } else {
            Darstellen();
        }
        return 0;
    }
    return ::DefWindowProcW(h, nachricht, w, l);
}

bool Vorbereiten() {
    if (g_fenster) return true;
    if (!g_gdiplus) {
        GdiplusStartupInput eingabe;
        if (GdiplusStartup(&g_gdiplus, &eingabe, nullptr) != Ok) return false;
    }
    WNDCLASSW klasse{};
    klasse.lpfnWndProc = Fensterfunktion;
    klasse.hInstance = ::GetModuleHandleW(nullptr);
    klasse.lpszClassName = L"RawTurnFreischaltung";
    ::RegisterClassW(&klasse);
    g_fenster = ::CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        klasse.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, klasse.hInstance,
        nullptr);
    return g_fenster != nullptr;
}

}  // namespace

void Zeigen(const app::Freischaltung& f, bool englisch, float skalierung) {
    if (!Vorbereiten()) return;
    g_s = skalierung > 0.5f ? skalierung : 1.0f;

    Eintrag e;
    e.titel = Breit(f.titel);
    e.beschreibung = Breit(f.beschreibung);
    e.punkte = f.punkte;
    e.badge_png = f.badge_png;
    e.englisch = englisch;

    if (g_laeuft) g_warteschlange.push_back(std::move(e));
    else Starten(std::move(e));
}

void Beenden() {
    if (g_fenster) {
        ::KillTimer(g_fenster, kTakt);
        ::DestroyWindow(g_fenster);
        g_fenster = nullptr;
    }
    delete g_badge;
    g_badge = nullptr;
    if (g_gdiplus) {
        GdiplusShutdown(g_gdiplus);
        g_gdiplus = 0;
    }
}

}  // namespace popup
