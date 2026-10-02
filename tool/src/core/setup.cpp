#include "core/setup.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

#include "core/i18n.h"
#include "core/mailbox.h"
#include "md5.h"

namespace {
// Uebersetztes Format mit einem Text fuellen.
std::string FmtS(const char* de_format, const std::string& a,
                 const std::string& b = std::string(), const std::string& c = std::string()) {
    char t[640];
    std::snprintf(t, sizeof(t), T(de_format), a.c_str(), b.c_str(), c.c_str());
    return t;
}
}  // namespace

extern "C" {
#include "rc_runtime.h"
#include "rc_runtime_types.h"
#include "rc_consoles.h"
}

namespace core {

// Schalter zur Fehlersuche, siehe ZeilenBauen(). Standard aus.
bool g_ohne_vektorzeile = false;
bool g_hook_im_wram = false;
SatzTeil g_satz_teil = SatzTeil::Vollstaendig;
bool g_mailbox_verlegt = false;
bool g_nur_zuhoeren = false;

uint32_t MailboxBasis() { return g_mailbox_verlegt ? kMailboxBasisAlt : kMailboxBasis; }
uint32_t HookBasis() {
    if (g_hook_im_wram) return kHookBasisWram;
    return MailboxBasis() + (uint32_t)kCodeOffset;
}
namespace {

// --- Hook-Code ------------------------------------------------------------
//
// Neun Worte SH-2-Code: kuenstlicher Rahmen, Sprung zum BIOS-Handler fuer
// V-Blank-OUT, danach zurueck. Die Watch-Werte schreibt die Saturn-Firmware
// selbst (ra_ingame.c). Im Normalfall (SatzTeil::Vollstaendig) geht dieser
// Code gar nicht in den Hook-Satz; er bleibt fuer die Stufen zur Fehlersuche
// und muss zu hook_aktuell in ra_ingame.c passen.
//
// Die drei letzten Worte sind Literale: Adresse des Nachlaufs (Ladeadresse
// + 0x10), ungecachte Mailbox-Basis und BIOS-Handler fuer V-Blank-OUT.
const uint32_t kHookWorte[] = {
    0x2f060002u, 0x2f06d004u, 0x2f06d005u, 0x402b50f2u,
    0x60f6002bu, 0x00090009u, 0x023f1010u, 0x223f0000u,
    0x06000846u,
};
constexpr int kHookNachlaufWort = 6;    // kHookBasis + 0x10
constexpr uint32_t kHookNachlaufOffset = 0x10u;
constexpr int kHookMboxWort = 7;        // 0x223f0000
constexpr int kHookHandlerWort = 8;

// ss_config_put() prueft keine Grenzen. Platz ist zwischen SYSINFO+0x0100 und
// den Kategorienamen bei SYSINFO+0x0E80 (Firm_MCU saturn_utils.c, Firm_Saturn
// main.c), also 0xD80 Byte = 432 Adress-/Wert-Paare samt Endmarke. Ein
// Ueberlauf traefe still die Kategorienamen. Etwas Reserve fuer eigene Zeilen
// in saroocfg.txt.
constexpr int kMaxPaare = 420;

const char* kMarkeAnfang = "# >>> ra-saturn";
const char* kMarkeEnde   = "# <<< ra-saturn";

std::string Zeile(uint32_t adresse, uint32_t wert) {
    char t[32];
    std::snprintf(t, sizeof(t), "M_%08x=%08x", adresse, wert);
    return t;
}

std::string Trimme(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

std::string Feld(const std::string& kopf, size_t off, size_t laenge) {
    if (off + laenge > kopf.size()) return {};
    return Trimme(kopf.substr(off, laenge));
}

const struct { char c; const char* name; } kGebiete[] = {
    {'J', TK("Japan")}, {'T', TK("Asien")}, {'U', TK("USA")}, {'E', TK("Europa")},
    {'B', TK("Brasilien")}, {'K', TK("Korea")}, {'A', TK("Asien (NTSC)")},
    {'L', TK("Lateinamerika")},
};

// Byte-Offset des Spurbeginns. Fuer .iso/.bin immer 0; bei .cue wird die in
// der ersten FILE-Zeile genannte Datei genommen.
bool SpurDatei(const std::string& pfad, std::string* bin, long* offset) {
    *bin = pfad;
    *offset = 0;
    std::string klein = pfad;
    for (char& c : klein) c = (char)::tolower((unsigned char)c);
    if (klein.size() < 4 || klein.substr(klein.size() - 4) != ".cue") return true;

    std::ifstream f(pfad);
    if (!f) return false;
    std::string zeile, name;
    while (std::getline(f, zeile)) {
        const size_t a = zeile.find('"');
        if (zeile.find("FILE") != std::string::npos && a != std::string::npos) {
            const size_t b = zeile.find('"', a + 1);
            if (b != std::string::npos) { name = zeile.substr(a + 1, b - a - 1); break; }
        }
    }
    if (name.empty()) return false;
    const size_t schr = pfad.find_last_of("\\/");
    *bin = (schr == std::string::npos) ? name : pfad.substr(0, schr + 1) + name;
    return true;
}

}  // namespace

bool LiesDiscKopf(const std::string& pfad, DiscKopf* aus, std::string* fehler,
                  std::string* rohkopf) {
    std::string bin;
    long spur = 0;
    if (!SpurDatei(pfad, &bin, &spur)) {
        if (fehler) *fehler = T("Die in der .cue genannte Datei wurde nicht gefunden.");
        return false;
    }
    std::ifstream f(bin, std::ios::binary);
    if (!f) {
        if (fehler) *fehler = T("Datei nicht lesbar.");
        return false;
    }

    // Sektor-Kopfgroessen der drei ueblichen Formate. Die Saturn-Kennung im
    // Kopf selbst dient als Entscheidungskriterium.
    const int kandidaten[] = {0, 16, 24};
    std::string kopf;
    bool gefunden = false;
    for (int k : kandidaten) {
        f.clear();
        f.seekg(spur + k, std::ios::beg);
        kopf.assign(512, '\0');
        f.read(&kopf[0], 512);
        if (f.gcount() < 512) continue;
        if (kopf.compare(0, 16, "SEGA SEGASATURN ") == 0) { gefunden = true; break; }
    }
    if (!gefunden) {
        if (fehler) *fehler = T("Kein gültiger Saturn-Kopf gefunden. Falsches Abbild, "
                                "falsche Spur, oder ein unübliches Sektorformat.");
        return false;
    }

    if (rohkopf) *rohkopf = kopf;
    return DeuteDiscKopf((const uint8_t*)kopf.data(), aus);
}

bool DeuteDiscKopf(const uint8_t* roh512, DiscKopf* aus) {
    if (!roh512 || !aus) return false;
    const std::string kopf((const char*)roh512, (size_t)kDiscKopfLaenge);
    if (kopf.compare(0, 16, "SEGA SEGASATURN ") != 0) return false;

    aus->produktnummer = Feld(kopf, 0x20, 10);
    aus->version       = Feld(kopf, 0x2A, 6);
    aus->kennung       = kopf.substr(0x20, 16);
    aus->titel         = Feld(kopf, 0x60, 112);
    aus->hersteller    = Feld(kopf, 0x10, 16);
    aus->datum         = Feld(kopf, 0x30, 8);
    aus->gebiete.clear();

    const std::string roh = Feld(kopf, 0x40, 10);
    for (char c : roh) {
        for (const auto& g : kGebiete)
            if (g.c == c) {
                if (!aus->gebiete.empty()) aus->gebiete += ", ";
                aus->gebiete += T(g.name);
            }
    }
    return true;
}

std::string HashAusKopf(const uint8_t* roh512) {
    if (!roh512) return {};
    md5_state_t st;
    md5_byte_t digest[16];
    md5_init(&st);
    md5_append(&st, (const md5_byte_t*)roh512, kDiscKopfLaenge);
    md5_finish(&st, digest);
    char hex[33];
    for (int i = 0; i < 16; ++i) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return hex;
}

std::vector<std::string> FindeAbbilder(const std::string& ordner, bool rekursiv) {
    std::vector<std::string> aus;
    std::vector<std::string> offen{ordner};

    while (!offen.empty()) {
        const std::string d = offen.back();
        offen.pop_back();

        std::vector<std::string> dateien;
        std::set<std::string> cue_staemme;

        WIN32_FIND_DATAA fd;
        HANDLE h = ::FindFirstFileA((d + "\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const std::string n = fd.cFileName;
            if (n == "." || n == "..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (rekursiv) offen.push_back(d + "\\" + n);
                continue;
            }
            dateien.push_back(n);
            std::string klein = n;
            for (char& c : klein) c = (char)::tolower((unsigned char)c);
            if (klein.size() > 4 && klein.substr(klein.size() - 4) == ".cue") {
                std::string stamm = klein.substr(0, klein.size() - 4);
                const size_t tr = stamm.find(" (track");
                if (tr != std::string::npos) stamm.resize(tr);
                cue_staemme.insert(Trimme(stamm));
            }
        } while (::FindNextFileA(h, &fd));
        ::FindClose(h);

        for (const std::string& n : dateien) {
            std::string klein = n;
            for (char& c : klein) c = (char)::tolower((unsigned char)c);
            const size_t punkt = klein.find_last_of('.');
            if (punkt == std::string::npos) continue;
            const std::string endung = klein.substr(punkt);
            if (endung != ".cue" && endung != ".iso" && endung != ".bin" && endung != ".img")
                continue;
            if (endung != ".cue") {
                std::string stamm = klein.substr(0, punkt);
                const size_t tr = stamm.find(" (track");
                if (tr != std::string::npos) stamm.resize(tr);
                if (cue_staemme.count(Trimme(stamm))) continue;   // Spur einer .cue
            }
            aus.push_back(d + "\\" + n);
        }
    }
    std::sort(aus.begin(), aus.end());
    return aus;
}

namespace {

// Wie viele Bytes ein Speicherzugriff dieser Groesse liest.
int BytesJeGroesse(uint8_t groesse) {
    switch (groesse) {
        case RC_MEMSIZE_16_BITS: case RC_MEMSIZE_16_BITS_BE:
            return 2;
        case RC_MEMSIZE_24_BITS: case RC_MEMSIZE_24_BITS_BE:
            return 3;
        case RC_MEMSIZE_32_BITS: case RC_MEMSIZE_32_BITS_BE:
        case RC_MEMSIZE_FLOAT: case RC_MEMSIZE_FLOAT_BE:
        case RC_MEMSIZE_MBF32: case RC_MEMSIZE_MBF32_LE:
        case RC_MEMSIZE_DOUBLE32: case RC_MEMSIZE_DOUBLE32_BE:
            return 4;
        default:
            return 1;
    }
}

// Jedes gelesene Byte aufnehmen, nicht nur die Startadresse. Ein 16- oder
// 32-Bit-Wert, der ueber eine Wortgrenze reicht, fehlte sonst zur Haelfte.
void SammleAusOperand(const rc_operand_t& op, std::set<uint32_t>* aus) {
    switch (op.type) {
        case RC_OPERAND_ADDRESS:
        case RC_OPERAND_DELTA:
        case RC_OPERAND_PRIOR:
        case RC_OPERAND_BCD:
        case RC_OPERAND_INVERTED:
            if (op.value.memref) {
                const uint32_t a = op.value.memref->address;
                const int n = BytesJeGroesse(op.value.memref->value.size);
                for (int i = 0; i < n; ++i) aus->insert(a + (uint32_t)i);
            }
            break;
        default: break;
    }
}

void SammleAusCondset(const rc_condset_t* cs, std::set<uint32_t>* aus) {
    for (; cs; cs = cs->next)
        for (const rc_condition_t* c = cs->conditions; c; c = c->next) {
            SammleAusOperand(c->operand1, aus);
            SammleAusOperand(c->operand2, aus);
        }
}

// RA-Adresse -> physische SH-2-Adresse, aus rcheevos' eigener Tabelle.
bool NachPhysisch(uint32_t ra, uint32_t* phys) {
    const rc_memory_regions_t* mr = rc_console_memory_regions(RC_CONSOLE_SATURN);
    if (!mr) return false;
    for (uint32_t i = 0; i < mr->num_regions; ++i) {
        const rc_memory_region_t& r = mr->region[i];
        if (ra >= r.start_address && ra <= r.end_address) {
            *phys = r.real_address + (ra - r.start_address);
            return true;
        }
    }
    return false;
}

}  // namespace

std::vector<uint32_t> WatchAdressen(const std::vector<RaAchievement>& achievements,
                                    int* nicht_abbildbar) {
    if (nicht_abbildbar) *nicht_abbildbar = 0;

    rc_runtime_t rt;
    rc_runtime_init(&rt);

    std::set<uint32_t> ra_adressen;
    for (const RaAchievement& a : achievements) {
        if (a.definition.empty()) { if (nicht_abbildbar) ++*nicht_abbildbar; continue; }
        if (rc_runtime_activate_achievement(&rt, a.id, a.definition.c_str(), nullptr, 0) != RC_OK) {
            if (nicht_abbildbar) ++*nicht_abbildbar;
            continue;
        }
    }
    for (uint32_t i = 0; i < rt.trigger_count; ++i) {
        const rc_trigger_t* t = rt.triggers[i].trigger;
        if (!t) continue;
        SammleAusCondset(t->requirement, &ra_adressen);
        SammleAusCondset(t->alternative, &ra_adressen);
    }
    rc_runtime_destroy(&rt);

    // Auf 4-Byte-Worte runden und zusammenfassen. Der Hook liest jeden Eintrag
    // als ganzes Wort; eine unausgerichtete Adresse waere ein SH-2-Busfehler.
    // Die Vertauschung der Bytepaare (siehe ra_session.cpp, Auswerten) bleibt
    // innerhalb eines 16-Bit-Worts und aendert das 4-Byte-Wort nicht.
    std::set<uint32_t> worte;
    for (uint32_t ra : ra_adressen) {
        uint32_t phys = 0;
        if (!NachPhysisch(ra, &phys)) continue;
        worte.insert(phys & ~3u);
    }
    return std::vector<uint32_t>(worte.begin(), worte.end());
}

// Aus einem Achievement-Satz die kompletten M_-Zeilen erzeugen. Gemeinsame
// Mitte von VerarbeiteAbbild (Abbilddatei vorhanden) und HookFuerSpiel (nur
// das, was die laufende Konsole gemeldet hat).
static void ZeilenBauen(const std::vector<RaAchievement>& achievements,
                        SetupErgebnis* e, bool sperre) {
    int nicht_abbildbar = 0;
    std::vector<uint32_t> watch = WatchAdressen(achievements, &nicht_abbildbar);

    e->watch = (int)watch.size();
    e->achievements = (int)achievements.size();

    // In die Mailbox gehen die Watch-Adressen des Achievement-Sets und, als
    // letzter Eintrag, das Statuswort der Hardcore-Sperre. Die Frage „welches
    // Spiel laeuft" wird hier nicht beantwortet: Das meldet die Firmware im
    // Betrieb selbst (GAME_INFO).
    watch.push_back(MailboxBasis() + kStatusOffset);
    std::string wfehler;
    const SatzTeil teil = g_satz_teil;
    // Stufe "Leer": Die Datei existiert und wird beim Spielstart gelesen,
    // enthaelt aber keine M_-Zeile.
    const bool mit_daten = (teil != SatzTeil::NurCode) && teil != SatzTeil::Leer;
    // Code und Vektorzeile nur noch in den Messstufen, siehe SatzTeil.
    const bool mit_code  = (teil == SatzTeil::MitHookCode ||
                            teil == SatzTeil::OhneVektor ||
                            teil == SatzTeil::NurCode);
    const bool mit_vektor = mit_code && teil != SatzTeil::OhneVektor && !g_ohne_vektorzeile;

    std::vector<std::string> zeilen;
    const std::vector<std::string> wz = WatchZeilen(watch, MailboxBasis(), &wfehler);
    if (wz.empty()) {
        e->status = TK("zu viele Adressen");
        e->hinweis = wfehler;
        return;
    }
    if (mit_daten) {
        zeilen.push_back(MagicZeile(MailboxBasis()));
        zeilen.push_back(SperrZeile(sperre, MailboxBasis()));
        // Bildzaehler auf null; nach dem Einschalten steht dort sonst
        // uninitialisiertes SDRAM.
        zeilen.push_back(Zeile(MailboxBasis() + 0x08u, 0u));

        // Spielkennung dazu, damit die Firmware Reste eines anderen Spiels
        // erkennt und verwirft. Siehe kKennungOffset in mailbox.h.
        {
            char k[kKennungLaenge];
            std::memset(k, ' ', sizeof(k));
            const std::string& q = e->kennung;
            for (int i = 0; i < kKennungLaenge && i < (int)q.size(); ++i) k[i] = q[(size_t)i];
            for (int i = 0; i < kKennungLaenge; i += 4) {
                const uint32_t w = ((uint32_t)(unsigned char)k[i] << 24) |
                                   ((uint32_t)(unsigned char)k[i + 1] << 16) |
                                   ((uint32_t)(unsigned char)k[i + 2] << 8) |
                                    (uint32_t)(unsigned char)k[i + 3];
                zeilen.push_back(Zeile(MailboxBasis() + (uint32_t)(kKennungOffset + i), w));
            }
        }
        if (teil == SatzTeil::ZaehlerNull) {
            // Eintraege bleiben stehen, nur der Zaehler wird null: Der Hook
            // laeuft, kopiert aber keinen einzigen Wert. Die erste Zeile aus
            // WatchZeilen ist der Zaehler.
            zeilen.push_back(Zeile(MailboxBasis() + kEintragZaehlOffset, 0u));
            zeilen.insert(zeilen.end(), wz.begin() + 1, wz.end());
        } else {
            zeilen.insert(zeilen.end(), wz.begin(), wz.end());
        }
    }

    // Hook-Code, Mailbox-Basis und Handler eingesetzt.
    const uint32_t basis = HookBasis();
    if (mit_code) {
        for (int i = 0; i < (int)(sizeof(kHookWorte) / sizeof(kHookWorte[0])); ++i) {
            uint32_t w = kHookWorte[i];
            if (i == kHookNachlaufWort) w = basis + kHookNachlaufOffset;
            if (i == kHookMboxWort)    w = MailboxBasis() | kUngecacht;
            if (i == kHookHandlerWort) w = kBiosHandler;
            zeilen.push_back(Zeile(basis + (uint32_t)i * 4, w));
        }
    }
    // Der Vektor zuletzt: erst wenn der Code vollstaendig steht.
    if (mit_vektor)
        zeilen.push_back(Zeile(kVektorAdresse, basis));

    e->zeilen = std::move(zeilen);
    e->paare = (int)e->zeilen.size();
    if (e->paare > kMaxPaare) {
        e->status = TK("Config-Puffer zu klein");
        char t[128];
        std::snprintf(t, sizeof(t), T("%d Paare, der Puffer fasst etwa %d."),
                      e->paare, kMaxPaare);
        e->hinweis = t;
        e->zeilen.clear();
        return;
    }

    e->installierbar = true;
    if (!watch.empty()) {
        e->status = TK("bereit");
        if (nicht_abbildbar > 0) {
            char t[160];
            std::snprintf(t, sizeof(t),
                T("%d Achievement(s) nutzen nicht abbildbare Merkmale und werden nie feuern."),
                nicht_abbildbar);
            e->hinweis = t;
        }
    } else {
        e->status = TK("bereit, ohne Achievements");
    }
}

SetupErgebnis VerarbeiteAbbild(const std::string& pfad, RaClient& client,
                               bool nur_mit_set) {
    SetupErgebnis e;
    e.abbild = pfad;

    std::string fehler;
    if (!LiesDiscKopf(pfad, &e.kopf, &fehler)) {
        e.status = TK("kein Saturn-Abbild");
        e.hinweis = fehler;
        return e;
    }
    e.kennung = e.kopf.kennung;
    e.name = e.kopf.titel;
    e.md5 = SaturnHash(pfad);

    std::vector<RaAchievement> achievements;
    if (client.Angemeldet() && !e.md5.empty()) {
        uint32_t gid = 0;
        if (client.SpielAusHash(e.md5, &gid, &fehler) && gid != 0) {
            RaSpiel spiel;
            if (client.SpielLaden(gid, e.md5, &spiel, &fehler)) {
                e.gameid = gid;
                if (!spiel.titel.empty()) e.name = spiel.titel;
                achievements = spiel.achievements;
                e.inoffiziell = spiel.inoffiziell;
            } else {
                e.hinweis = T("Achievement-Set nicht ladbar: ") + fehler;
            }
        } else if (!fehler.empty()) {
            e.hinweis = fehler;
        }
    }
    e.achievements = (int)achievements.size();

    if (achievements.empty() && nur_mit_set) {
        e.status = TK("kein Achievement-Set");
        return e;
    }

    // Ohne Sperre: Die Einrichtung kennt die Hardcore-Einstellung nicht. Laeuft
    // das Spiel im Hardcore-Modus, schickt das Werkzeug den Hook-Satz mit Sperre
    // nach, sobald das Statuswort die fehlende Sperre meldet.
    ZeilenBauen(achievements, &e, false);

    // Feinschliff der Statusmeldung, nur fuer den Weg ueber die Abbilddatei.
    if (e.installierbar && e.watch == 0) {
        if (e.gameid && e.inoffiziell > 0) {
            // Der haeufigste Stolperstein: Auf der RA-Seite sieht das Spiel
            // aus, als haette es ein Set. Es ist nur noch nicht freigegeben,
            // und inoffizielle Achievements zaehlen nicht.
            e.status = TK("nur inoffizielles Set");
            char t[220];
            std::snprintf(t, sizeof(t),
                T("%s: RetroAchievements liefert %d Achievement(s), aber alle sind "
                  "inoffiziell und zählen nicht. Der Hook wird trotzdem installiert."),
                e.name.c_str(), e.inoffiziell);
            e.hinweis = t;
        } else if (e.gameid) {
            e.hinweis = FmtS("%s ist bei RetroAchievements bekannt, hat aber kein "
                             "Set. Der Hook wird trotzdem installiert.", e.name);
        } else {
            e.status = TK("bereit, RA-Hash unbekannt");
            e.hinweis = FmtS("%s — Titel und Kennung stammen aus dem Disc-Kopf "
                             "(Gebiet %s). RetroAchievements kennt genau diesen Abzug "
                             "nicht, Prüfsumme %s. Ein anderer Abzug desselben Spiels "
                             "kann funktionieren. Der Hook wird trotzdem installiert.",
                             e.name,
                             e.kopf.gebiete.empty() ? std::string(T("unbekannt"))
                                                    : e.kopf.gebiete,
                             e.md5);
        }
    }
    return e;
}

SetupErgebnis HookFuerSpiel(const std::string& kennung, const std::string& name,
                            const std::string& md5, uint32_t gameid,
                            const std::vector<RaAchievement>& achievements,
                            bool sperre) {
    SetupErgebnis e;
    e.kennung = kennung;
    e.name = name;
    e.md5 = md5;
    e.gameid = gameid;
    ZeilenBauen(achievements, &e, sperre);
    return e;
}

std::string HookDateiInhalt(const SetupErgebnis& e) {
    std::string titel;
    for (char c : e.name) {
        if ((unsigned char)c < 0x20) continue;      // keine Zeilenumbrueche o. Ae.
        titel += c;
    }
    const size_t kMaxTitel = 60;
    if (titel.size() > kMaxTitel) {
        size_t n = kMaxTitel;
        while (n > 0 && ((unsigned char)titel[n] & 0xC0) == 0x80) --n;
        titel.resize(n);
    }

    std::string inhalt;
    if (!titel.empty()) inhalt = "; " + titel + "\n";
    for (const std::string& z : e.zeilen) { inhalt += z; inhalt += "\n"; }
    return inhalt;
}

// ---------------------------------------------------------------------------

namespace {

struct CfgBlock {
    std::string kennung;              // leer = Praeambel
    std::vector<std::string> zeilen;
};

// Zwei Abschnittskennungen vergleichen wie die Firmware es tut.
//
// gameid_match() in saturn_utils.c zerlegt beide an den Leerzeichen in
// Produktnummer und Version und vergleicht die Teile einzeln. "T-11304H  V3.08"
// und "T-11304H   V3.08 " sind dort dasselbe Spiel. Wer hier stur Zeichen für
// Zeichen vergleicht, findet einen vorhandenen Abschnitt nicht wieder und legt
// einen zweiten an -- und die Firmware nimmt nur den ERSTEN passenden. Der
// Hook landete dann in einem Abschnitt, der nie gelesen wird.
}  // namespace

bool KennungGleich(const std::string& a, const std::string& b) {
    auto teile = [](const std::string& s, std::string* id, std::string* ver) {
        const std::string t = Trimme(s);
        const size_t leer = t.find(' ');
        if (leer == std::string::npos) { *id = t; ver->clear(); return; }
        *id = t.substr(0, leer);
        *ver = Trimme(t.substr(leer));
        const size_t weiter = ver->find(' ');
        if (weiter != std::string::npos) *ver = ver->substr(0, weiter);
    };
    std::string ia, va, ib, vb;
    teile(a, &ia, &va);
    teile(b, &ib, &vb);
    return ia == ib && va == vb;
}

namespace {

bool GehoertUns(const std::string& zeile) {
    const std::string s = Trimme(zeile);
    if (s.size() < 3 || s[0] != 'M' || s[1] != '_') return false;
    const uint32_t adresse = (uint32_t)std::strtoul(s.substr(2, 8).c_str(), nullptr, 16);
    const uint32_t m = kMailboxBasis;
    if (adresse >= m && adresse < m + kCodeOffset) return true;                    // Mailbox
    if (adresse >= kHookBasis && adresse < kHookBasis + 0x100) return true;        // Hook-Code
    if (adresse == kVektorAdresse) return true;                                     // Vektor
    return false;
}

}  // namespace

bool CfgErreichbar(const std::string& pfad) {
    if (pfad.empty()) return false;
    const DWORD attr = ::GetFileAttributesA(pfad.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// Warum die Datei nicht erreichbar ist, im Klartext.
//
// „nicht lesbar" allein hilft nicht weiter: Der haeufigste Fall
// ist, dass die SD-Karte gerade in der SAROO steckt und das Laufwerk am PC
// deshalb nicht existiert. Das ist kein Fehler, sondern der Normalfall waehrend
// des Spielens -- es muss nur dastehen.
std::string CfgGrund(const std::string& pfad) {
    if (pfad.empty()) return T("Es ist kein Pfad zur saroocfg.txt hinterlegt.");

    const DWORD code = (::GetFileAttributesA(pfad.c_str()) == INVALID_FILE_ATTRIBUTES)
                           ? ::GetLastError() : 0;
    std::string laufwerk;
    if (pfad.size() >= 2 && pfad[1] == ':') laufwerk = pfad.substr(0, 2);

    switch (code) {
        case 0:
            return FmtS("%s ist da, lässt sich aber nicht öffnen. Schreibschutz "
                        "am Kartenleser oder die Datei ist gerade in Benutzung?", pfad);
        case ERROR_FILE_NOT_FOUND:
            return FmtS("In %s gibt es diese Datei nicht: %s",
                        laufwerk.empty() ? std::string(T("dem Ordner")) : laufwerk, pfad);
        case ERROR_PATH_NOT_FOUND:
        case ERROR_NOT_READY:
        case ERROR_INVALID_DRIVE:
        case ERROR_BAD_NETPATH:
            return FmtS("Laufwerk %s ist nicht da. Die SD-Karte steckt vermutlich in der "
                        "SAROO — während des Spielens ist das normal.",
                        laufwerk.empty() ? std::string("?") : laufwerk);
        case ERROR_ACCESS_DENIED:
            return FmtS("Zugriff auf %s verweigert.", pfad);
        default: {
            char t[200];
            std::snprintf(t, sizeof(t), T("%s ist nicht erreichbar (Windows-Fehler %lu)."),
                          pfad.c_str(), (unsigned long)code);
            return t;
        }
    }
}

bool SchreibeSaroocfg(const std::string& pfad,
                      const std::vector<SetupErgebnis>& fertige,
                      std::string* sicherungspfad, std::string* fehler) {
    std::vector<CfgBlock> bloecke;
    bloecke.push_back({});
    {
        std::ifstream f(pfad, std::ios::binary);
        if (!f) { if (fehler) *fehler = CfgGrund(pfad); return false; }
        std::string zeile;
        while (std::getline(f, zeile)) {
            if (!zeile.empty() && zeile.back() == '\r') zeile.pop_back();
            const std::string t = Trimme(zeile);
            if (t.size() >= 2 && t.front() == '[' && t.back() == ']') {
                CfgBlock b;
                b.kennung = t.substr(1, t.size() - 2);
                bloecke.push_back(std::move(b));
            } else {
                bloecke.back().zeilen.push_back(zeile);
            }
        }
    }

    SYSTEMTIME st;
    ::GetLocalTime(&st);
    char stempel[32];
    std::snprintf(stempel, sizeof(stempel), "%04d-%02d-%02d %02d:%02d",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);

    for (const SetupErgebnis& e : fertige) {
        if (!e.installierbar) continue;

        CfgBlock* ziel = nullptr;
        for (CfgBlock& b : bloecke)
            if (!b.kennung.empty() && KennungGleich(b.kennung, e.kennung)) {
                ziel = &b;
                break;
            }
        if (!ziel) {
            CfgBlock b;
            // Ohne nachlaufende Leerzeichen geschrieben. Die Firmware trimmt
            // ohnehin, und so ist die Datei von Hand lesbar.
            b.kennung = Trimme(e.kennung);
            bloecke.push_back(std::move(b));
            ziel = &bloecke.back();
        }

        // Eigene Zeilen entfernen. Ein frueher erzeugter Markerblock als
        // Ganzes, damit die Kommentarzeile nicht stehenbleibt und sich bei
        // jedem Lauf vermehrt. Zusaetzlich einzelne M_-Zeilen in unseren
        // Adressbereichen, das raeumt von Hand eingetragene Altbestaende ab.
        std::vector<std::string> behalten;
        bool im_block = false;
        for (const std::string& z : ziel->zeilen) {
            const std::string t = Trimme(z);
            if (!im_block && t.rfind(kMarkeAnfang, 0) == 0) { im_block = true; continue; }
            if (im_block) { if (t.rfind(kMarkeEnde, 0) == 0) im_block = false; continue; }
            if (GehoertUns(z)) continue;
            behalten.push_back(z);
        }
        while (!behalten.empty() && Trimme(behalten.back()).empty()) behalten.pop_back();

        if (!behalten.empty()) behalten.push_back("");
        behalten.push_back(std::string(kMarkeAnfang) +
                           ": automatisch erzeugt, nicht von Hand aendern (" + stempel + ")");
        {
            char t[220];
            std::snprintf(t, sizeof(t), "# %s -- %d Achievements, %d Watch-Adressen",
                          e.name.c_str(), e.achievements, e.watch);
            behalten.push_back(t);
        }
        for (const std::string& z : e.zeilen) behalten.push_back(z);
        behalten.push_back(kMarkeEnde);
        behalten.push_back("");
        ziel->zeilen = std::move(behalten);
    }

    // Sicherung vor dem Schreiben.
    char sicherung[MAX_PATH];
    std::snprintf(sicherung, sizeof(sicherung), "%s.%04d%02d%02d_%02d%02d%02d.bak",
                  pfad.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    if (!::CopyFileA(pfad.c_str(), sicherung, FALSE)) {
        if (fehler) *fehler = T("Sicherung konnte nicht angelegt werden, es wurde nichts geschrieben.");
        return false;
    }
    if (sicherungspfad) *sicherungspfad = sicherung;

    std::ofstream f(pfad, std::ios::binary);
    if (!f) { if (fehler) *fehler = T("saroocfg.txt nicht beschreibbar."); return false; }
    for (const CfgBlock& b : bloecke) {
        if (!b.kennung.empty()) f << "[" << b.kennung << "]\r\n";
        for (const std::string& z : b.zeilen) f << z << "\r\n";
    }
    return true;
}

}  // namespace core
