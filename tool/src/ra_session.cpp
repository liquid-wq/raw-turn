#include "ra_session.h"

#include <cstdio>
#include <cstring>

#include <windows.h>   // GetModuleFileNameA fuer ra_definitionen.txt

#include "core/http.h"
#include "core/i18n.h"
#include "core/setup.h"

extern "C" {
#include "rc_runtime.h"
#include "rc_runtime_types.h"
#include "rc_consoles.h"
}

// Siehe RaSession::Ereignis: rc_runtime reicht dort keine Benutzerdaten durch.
static RaSession* g_aktive_sitzung = nullptr;

namespace {

// Physische SH-2-Adresse -> RA-Adresse.
//
// Die Zuordnung stammt aus rcheevos selbst (rc_console_memory_regions fuer
// RC_CONSOLE_SATURN): RA 0x000000-0x0FFFFF liegt physisch ab 0x00200000
// (Work RAM Low), RA 0x100000-0x1FFFFF ab 0x06000000 (Work RAM High).
bool NachRaAdresse(uint32_t phys, uint32_t* ra) {
    const rc_memory_regions_t* mr = rc_console_memory_regions(RC_CONSOLE_SATURN);
    if (!mr) return false;
    for (uint32_t i = 0; i < mr->num_regions; ++i) {
        const rc_memory_region_t& r = mr->region[i];
        const uint32_t laenge = r.end_address - r.start_address + 1;
        if (phys >= r.real_address && phys < r.real_address + laenge) {
            *ra = r.start_address + (phys - r.real_address);
            return true;
        }
    }
    return false;
}

}  // namespace

RaSession::RaSession() {
    index_.Laden(core::StandardIndexPfad());
    einstellungen_.Laden();
}

void RaSession::SpielVergessen() {
    LaufzeitAbbauen();
    spiel_ = core::RaSpiel();
    spiel_geladen_ = false;
    geladene_kennung_.clear();
    gefragte_md5_.clear();
    hook_nachgetragen_ = false;
    gesendete_sperre_ = false;
    std::lock_guard<std::mutex> g(sperre_);
    spiel_neu_ = true;
    erkannt_neu_ = true;
    erkannt_name_.clear();
    erkannt_gebiete_.clear();
    erkannt_ra_ = false;
}

void RaSession::HookNachtragen(
    const std::function<bool(const std::string&, const std::string&)>& senden,
    bool sperre) {
    if (HookSchonNachgetragen(sperre)) return;
    if (!spiel_geladen_ || geladene_kennung_.empty()) return;
    const bool erneut = hook_nachgetragen_;
    hook_nachgetragen_ = true;          // auch bei Misserfolg nicht wiederholen
    gesendete_sperre_ = sperre;

    // Alles, was dafür nötig ist, liegt schon vor: Kennung und Prüfsumme von
    // der Konsole, der Achievement-Satz von RetroAchievements. Keine
    // Abbilddatei, kein ROM-Ordner.
    const core::SetupErgebnis e = core::HookFuerSpiel(
        geladene_kennung_, spiel_.titel, spiel_.hash, spiel_.id, spiel_.achievements,
        sperre);
    if (!e.installierbar) {
        Melden(T("Hook konnte nicht erzeugt werden: ") + std::string(T(e.status.c_str())) +
               (e.hinweis.empty() ? "" : " — " + e.hinweis), app::Rang::Warnung);
        return;
    }

    // Bevorzugter Weg: Das Gerät schreibt selbst. Beim Spielen steckt die
    // SD-Karte in der SAROO, der PC kommt gar nicht heran — nur die Firmware.
    if (senden) {
        const std::string inhalt = core::HookDateiInhalt(e);
        if (senden(geladene_kennung_, inhalt)) {
            char t[300];
            if (erneut)
                std::snprintf(t, sizeof(t), "%s",
                    sperre ? T("Hardcore: Speicherstände werden gesperrt. Das wirkt "
                               "ab dem nächsten Start des Spiels.")
                           : T("Speicherstände werden freigegeben. Das wirkt ab dem "
                               "nächsten Start des Spiels."));
            else
                std::snprintf(t, sizeof(t),
                    T("Hook für %s wird an die SAROO geschickt: %d Watch-Adressen, "
                      "%d Zeilen. Sie legt ihn selbst auf ihrer Karte ab."),
                    e.name.c_str(), e.watch, (int)e.zeilen.size());
            Melden(t, app::Rang::Wichtig);
            return;
        }
    }

    // Rückfallebene: über die Karte am PC.
    einstellungen_.Laden();             // kann zwischendurch gesetzt worden sein
    if (einstellungen_.saroocfg.empty()) {
        Melden(T("Hook könnte jetzt nachgetragen werden, aber der Pfad zur "
                 "saroocfg.txt ist nicht hinterlegt. Einmal über „Firmware und "
                 "Spiele“ die Datei auswählen, danach geht es von selbst."),
               app::Rang::Warnung);
        return;
    }

    // Nicht erreichbar heisst nicht gescheitert. Der fertige Hook wird
    // vorgemerkt und geschrieben, sobald die Karte wieder am PC steckt.
    if (!core::CfgErreichbar(einstellungen_.saroocfg)) {
        wartende_.push_back(e);
        wartend_gemeldet_ = false;
        char t[512];
        std::snprintf(t, sizeof(t),
            T("Hook für %s ist fertig, aber noch nicht geschrieben. %s Er wird "
              "automatisch eingetragen, sobald die Karte am PC steckt."),
            e.name.c_str(), core::CfgGrund(einstellungen_.saroocfg).c_str());
        Melden(t, app::Rang::Warnung);
        return;
    }

    std::string sicherung, fehler;
    if (!core::SchreibeSaroocfg(einstellungen_.saroocfg, {e}, &sicherung, &fehler)) {
        wartende_.push_back(e);
        wartend_gemeldet_ = false;
        Melden(T("saroocfg.txt nicht geschrieben: ") + fehler +
               T(" Der Hook bleibt vorgemerkt."), app::Rang::Warnung);
        return;
    }

    char t[320];
    std::snprintf(t, sizeof(t),
        T("Hook für %s nachgetragen: %d Watch-Adressen in %s. Er wirkt beim "
          "NÄCHSTEN Start dieses Spiels, weil die SAROO die Zeilen nur beim Laden "
          "ausführt. Also einmal zurück ins Menü und das Spiel neu starten."),
        e.name.c_str(), e.watch, einstellungen_.saroocfg.c_str());
    Melden(t, app::Rang::Wichtig);
}

void RaSession::HooksNachreichen() {
    if (wartende_.empty()) return;
    if (!core::CfgErreichbar(einstellungen_.saroocfg)) {
        if (!wartend_gemeldet_) {
            wartend_gemeldet_ = true;
            char t[300];
            std::snprintf(t, sizeof(t),
                T("%d Hook(s) warten aufs Schreiben. %s"),
                (int)wartende_.size(), core::CfgGrund(einstellungen_.saroocfg).c_str());
            Melden(t, app::Rang::Warnung);
        }
        return;
    }

    std::string sicherung, fehler;
    if (!core::SchreibeSaroocfg(einstellungen_.saroocfg, wartende_, &sicherung, &fehler)) {
        Melden(T("saroocfg.txt weiterhin nicht geschrieben: ") + fehler, app::Rang::Warnung);
        return;
    }

    std::string namen;
    for (const core::SetupErgebnis& e : wartende_) {
        if (!namen.empty()) namen += ", ";
        namen += e.name.empty() ? e.kennung : e.name;
    }
    const int n = (int)wartende_.size();
    wartende_.clear();
    wartend_gemeldet_ = false;

    char t[400];
    std::snprintf(t, sizeof(t),
        T("%d vorgemerkte(r) Hook(s) jetzt eingetragen: %s. Sicherung: %s. "
          "Karte zurück in die SAROO, dann läuft der Hook ab dem nächsten Start "
          "dieser Spiele."),
        n, namen.c_str(), sicherung.c_str());
    Melden(t, app::Rang::Wichtig);
}

RaSession::~RaSession() { Beenden(); }

void RaSession::Beenden() {
    if (thread_.joinable()) thread_.join();
    LaufzeitAbbauen();
}

void RaSession::Melden(std::string text, app::Rang rang) {
    std::lock_guard<std::mutex> g(sperre_);
    ausgabe_.push_back({std::move(text), rang});
}

// ---------------------------------------------------------------------------

void RaSession::AnmeldenStarten(const std::string& benutzer, const std::string& passwort) {
    if (arbeitet_.load()) return;
    if (benutzer.empty() || passwort.empty()) {
        Melden(T("Benutzername und Passwort eingeben."), app::Rang::Warnung);
        return;
    }
    arbeitet_ = true;
    Melden(T("Melde an…"));
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread([this, benutzer, passwort]() {
        std::string fehler;
        if (client_.Anmelden(benutzer, passwort, &fehler)) {
            Melden(T("Angemeldet als ") + client_.Benutzer(), app::Rang::Gut);
            std::lock_guard<std::mutex> g(sperre_);
            anmeldung_neu_ = true;
        } else {
            Melden(T("Anmeldung fehlgeschlagen: ") + fehler, app::Rang::Warnung);
        }
        arbeitet_ = false;
    });
}

void RaSession::Abmelden() {
    client_.Abmelden();
    LaufzeitAbbauen();
    spiel_geladen_ = false;
    geladene_kennung_.clear();
    gefragte_md5_.clear();
    spiel_ = core::RaSpiel();
    Melden(T("Abgemeldet."));
    std::lock_guard<std::mutex> g(sperre_);
    anmeldung_neu_ = true;
    spiel_neu_ = true;
}

void RaSession::SpielLadenStarten(const std::string& kennung,
                                  const std::string& titel,
                                  const std::string& md5, bool hardcore) {
    if (arbeitet_.load()) return;         // laeuft noch: im naechsten Bild erneut
    if (md5.empty()) return;
    if (md5 == gefragte_md5_) return;
    gefragte_md5_ = md5;

    // Schritt 1: Identitaet melden. Sie kommt vollstaendig aus dem Disc-Kopf,
    // den das laufende Spiel gemeldet hat, und haengt nicht an
    // RetroAchievements. Das passiert IMMER, auch ohne Anmeldung und auch wenn
    // RA den Abzug nicht kennt.
    {
        std::lock_guard<std::mutex> g(sperre_);
        erkannt_name_ = titel;
        erkannt_gebiete_.clear();
        erkannt_ra_ = false;
        erkannt_neu_ = true;
    }
    Melden(T("Spiel läuft: ") + (titel.empty() ? kennung : titel) +
           "  [" + kennung + "]", app::Rang::Wichtig);

    if (!client_.Angemeldet()) {
        Melden(T("Zum Auswerten erst bei RetroAchievements anmelden. "
                 "Prüfsumme steht bereit: ") + md5, app::Rang::Warnung);
        return;
    }

    arbeitet_ = true;
    hardcore_ = hardcore;
    geladene_kennung_ = kennung;     // nicht bei jedem Bericht wiederholen
    Melden(T("Frage RetroAchievements nach Prüfsumme ") + md5 + "…");

    if (thread_.joinable()) thread_.join();
    thread_ = std::thread([this, kennung, titel, md5, hardcore]() {
        core::RaSpiel spiel;
        std::string fehler;

        // Genau der Weg aus mega-raw: Die Konsole liefert die Prüfsumme, der
        // PC fragt damit direkt bei RetroAchievements nach. Keine oertliche
        // Sammlung, keine Nachschlagetabelle, keine Abbilddatei.
        uint32_t gid = 0;
        if (!client_.SpielAusHash(md5, &gid, &fehler)) {
            Melden(T("Nachfrage bei RetroAchievements fehlgeschlagen: ") + fehler,
                   app::Rang::Warnung);
            arbeitet_ = false;
            return;
        }
        if (gid == 0) {
            char t[320];
            std::snprintf(t, sizeof(t),
                T("RetroAchievements kennt diesen Abzug nicht (Prüfsumme %s). "
                  "Live-Werte laufen weiter, es gibt nur nichts auszuwerten."),
                md5.c_str());
            Melden(t, app::Rang::Warnung);
            arbeitet_ = false;
            return;
        }
        {
            std::lock_guard<std::mutex> g(sperre_);
            erkannt_ra_ = true;
            erkannt_neu_ = true;
        }

        if (!client_.SpielLaden(gid, md5, &spiel, &fehler)) {
            Melden(T("Achievement-Set nicht ladbar: ") + fehler, app::Rang::Warnung);
            arbeitet_ = false;
            return;
        }
        if (!client_.SitzungStarten(&spiel, hardcore, &fehler))
            Melden(T("Sitzung nicht gestartet: ") + fehler, app::Rang::Warnung);

        int frei = 0;
        for (const core::RaAchievement& a : spiel.achievements)
            if (a.freigeschaltet) ++frei;

        {
            std::lock_guard<std::mutex> g(sperre_);
            spiel_ = std::move(spiel);
            spiel_geladen_ = true;
            geladene_kennung_ = kennung;
            spiel_neu_ = true;
        }
        LaufzeitAufbauen();

        char t[220];
        std::snprintf(t, sizeof(t), T("%s geladen: %d Achievements, %d bereits freigeschaltet."),
                      spiel_.titel.c_str(), (int)spiel_.achievements.size(), frei);
        Melden(t, app::Rang::Wichtig);

        // Ein leerer Kernsatz hat zwei sehr verschiedene Ursachen. Ohne diese
        // Unterscheidung sieht „RetroAchievements hat fuer dieses Spiel noch
        // keine Achievements" genauso aus wie „es gibt welche, aber sie sind
        // noch nicht offiziell freigegeben".
        if (spiel_.achievements.empty()) {
            char u[300];
            if (spiel_.inoffiziell > 0)
                std::snprintf(u, sizeof(u),
                    T("RetroAchievements liefert %d Achievement(s), aber alle sind "
                      "inoffiziell und zählen nicht. Deshalb gibt es nichts "
                      "auszuwerten und keine Watch-Adressen zu beobachten."),
                    spiel_.inoffiziell);
            else
                std::snprintf(u, sizeof(u), "%s",
                    T("Für dieses Spiel gibt es bei RetroAchievements noch keine "
                      "Achievements. Erkennung und Live-Werte laufen trotzdem, es "
                      "gibt nur nichts auszuwerten."));
            Melden(u, app::Rang::Warnung);
        }
        arbeitet_ = false;
    });
}

// ---------------------------------------------------------------------------

void RaSession::LaufzeitAbbauen() {
    if (!laufzeit_) return;
    rc_runtime_destroy(laufzeit_);
    delete laufzeit_;
    laufzeit_ = nullptr;
    schatten_.clear();
}

// Die Definitionen aller Achievements neben das Programm schreiben,
// ra_definitionen.txt, bei jedem geladenen Spiel neu. Ohne sie laesst sich
// nicht nachvollziehen, warum ein Achievement nicht ausloest: welche Adresse,
// welche Groesse, welcher Vergleich.
static void DefinitionenAblegen(const core::RaSpiel& spiel) {
    char pfad[MAX_PATH];
    const DWORD n = ::GetModuleFileNameA(nullptr, pfad, MAX_PATH);
    std::string p(pfad, n);
    const size_t schr = p.find_last_of("\\/");
    p = (schr == std::string::npos ? std::string() : p.substr(0, schr + 1)) +
        "ra_definitionen.txt";
    FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "%s  (RA %u, %s)\n", spiel.titel.c_str(), spiel.id, spiel.hash.c_str());
    for (const core::RaAchievement& a : spiel.achievements)
        std::fprintf(f, "%u\t%s\t%s\t%s\n", a.id, a.titel.c_str(), a.beschreibung.c_str(),
                     a.definition.c_str());
    std::fclose(f);
}

void RaSession::LaufzeitAufbauen() {
    LaufzeitAbbauen();
    DefinitionenAblegen(spiel_);
    laufzeit_ = new rc_runtime_t();
    rc_runtime_init(laufzeit_);

    int aktiv = 0, abgelehnt = 0;
    for (const core::RaAchievement& a : spiel_.achievements) {
        if (a.freigeschaltet) continue;          // nichts doppelt vergeben
        if (a.definition.empty()) { ++abgelehnt; continue; }
        const int rc = rc_runtime_activate_achievement(
            laufzeit_, a.id, a.definition.c_str(), nullptr, 0);
        if (rc == RC_OK) ++aktiv; else ++abgelehnt;
    }
    if (abgelehnt > 0) {
        char t[160];
        std::snprintf(t, sizeof(t),
                      T("%d Achievement(s) nutzen Merkmale, die hier nicht abbildbar sind, "
                        "und werden nie feuern."), abgelehnt);
        Melden(t, app::Rang::Warnung);
    }
}

uint32_t RaSession::Lesen(uint32_t adresse, uint32_t bytes, void* ud) {
    RaSession* self = (RaSession*)ud;
    // rcheevos erwartet den Wert little-endian zusammengesetzt, unabhaengig
    // davon, wie die Konsole ihn ablegt. Der Schatten haelt einzelne Bytes in
    // der Reihenfolge des Saturn-Speichers, die Zusammensetzung passiert hier.
    uint32_t wert = 0;
    for (uint32_t i = 0; i < bytes && i < 4; ++i) {
        const auto it = self->schatten_.find(adresse + i);
        // Nicht beobachtete Adressen liefern 0: Der Hook sendet nur, was in
        // der Watch-Liste steht.
        const uint8_t b = (it == self->schatten_.end()) ? 0 : it->second;
        wert |= (uint32_t)b << (8 * i);
    }
    return wert;
}

void RaSession::Ereignis(const rc_runtime_event_t* e) {
    // rc_runtime reicht dem Ereignis-Rueckruf keine Benutzerdaten durch,
    // deshalb die aktive Sitzung ueber eine Dateivariable. Unkritisch, weil
    // do_frame nur aus einem Thread aufgerufen wird.
    if (!g_aktive_sitzung || !e) return;
    if (e->type == RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED)
        g_aktive_sitzung->Freischalten(e->id);
}

void RaSession::Freischalten(uint32_t achievement_id) {
    std::string titel, beschreibung, badge;
    int punkte = 0;
    for (core::RaAchievement& a : spiel_.achievements)
        if (a.id == achievement_id) {
            // Schon freigeschaltet: nichts melden, nichts einblenden.
            if (a.freigeschaltet) return;
            a.freigeschaltet = true;
            titel = a.titel;
            beschreibung = a.beschreibung;
            badge = a.badge;
            punkte = a.punkte;
            break;
        }

    char t[220];
    std::snprintf(t, sizeof(t), T("Freigeschaltet: %s, %d Punkte"), titel.c_str(), punkte);
    Melden(t, app::Rang::Gut);

    {
        std::lock_guard<std::mutex> g(sperre_);
        spiel_neu_ = true;
    }

    // Badge nebenher laden; die Einblendung kommt, sobald es da ist oder das
    // Laden aufgegeben hat. Ohne Badge zeichnet sie einen Pokal.
    std::thread([this, titel, beschreibung, badge, punkte]() {
        app::Freischaltung f;
        f.titel = titel;
        f.beschreibung = beschreibung;
        f.punkte = punkte;
        if (!badge.empty()) {
            core::HttpAntwort a = core::HttpGet("media.retroachievements.org",
                                                "/Badge/" + badge + ".png");
            if (a.ok) f.badge_png = std::move(a.koerper);
        }
        std::lock_guard<std::mutex> g(sperre_);
        einblenden_.push_back(std::move(f));
    }).detach();

    // Melden laeuft nebenher, damit die Auswertung nicht auf das Netz wartet.
    const std::string hash = spiel_.hash;
    const bool hardcore = hardcore_;
    std::thread([this, achievement_id, hash, hardcore, titel]() {
        std::string fehler;
        if (!client_.Freischalten(achievement_id, hash, hardcore, &fehler))
            Melden(T("Freischaltung nicht gemeldet (") + titel + "): " + fehler,
                   app::Rang::Warnung);
    }).detach();
}

void RaSession::Auswerten(const std::vector<app::Watchwert>& werte, bool hook_laeuft,
                          bool hardcore) {
    hardcore_ = hardcore;
    if (!laufzeit_ || !spiel_geladen_) return;

    // Fail closed: Solange der vom Hook gepflegte Bildzaehler steht, sind die
    // Werte nicht frisch. Eingefrorene Werte gegen Bedingungen zu pruefen
    // koennte sonst zu falschen Freischaltungen fuehren.
    if (!hook_laeuft) return;

    for (const app::Watchwert& w : werte)
        SchattenSetzen(w.adresse, w.wert);

    g_aktive_sitzung = this;
    rc_runtime_do_frame(laufzeit_, &RaSession::Ereignis, &RaSession::Lesen, this, nullptr);
    g_aktive_sitzung = nullptr;
    BedingungenAblegen();
}

// Aenderungsprotokoll, ein Bild. Bedingungen mit Wechseln von einem Bild zum
// naechsten (Delta, Trefferzaehler) brauchen jedes Bild; Stichproben treffen
// sie nur zufaellig.
//
// Nach einer Luecke fehlen Bilder. Die Werte davor und danach als
// aufeinanderfolgend zu werten, koennte einen Wechsel vortaeuschen, den es so
// nie gab. Deshalb: Werte weiter uebernehmen, aber erst auswerten, wenn die
// Saturn alle Eintraege einmal aufgefrischt hat (8 je Bild), und dann die
// Laufzeit zuruecksetzen. rc_runtime_reset setzt jedes Achievement auf
// "wartend": Es loest erst aus, nachdem seine Bedingung einmal falsch war.
void RaSession::AuswertenBild(const std::vector<std::pair<uint32_t, uint32_t>>& werte,
                              bool luecke, int eintraege, bool hardcore) {
    hardcore_ = hardcore;
    if (!laufzeit_ || !spiel_geladen_) return;

    for (const auto& w : werte) SchattenSetzen(w.first, w.second);

    if (luecke) sperr_bilder_ = eintraege / 8 + 2;
    if (sperr_bilder_ > 0) {
        if (--sperr_bilder_ == 0) rc_runtime_reset(laufzeit_);
        return;
    }

    g_aktive_sitzung = this;
    rc_runtime_do_frame(laufzeit_, &RaSession::Ereignis, &RaSession::Lesen, this, nullptr);
    g_aktive_sitzung = nullptr;

    // Die Zustandsdatei nicht 60-mal je Sekunde schreiben.
    static unsigned long abgelegt = 0;
    if (::GetTickCount() - abgelegt > 500) {
        abgelegt = ::GetTickCount();
        BedingungenAblegen();
    }
}

void RaSession::SchattenSetzen(uint32_t saturn_adresse, uint32_t wert) {
    {
        uint32_t ra = 0;
        if (!NachRaAdresse(saturn_adresse, &ra)) return;
        // RetroAchievements sieht den Saturn-Speicher mit vertauschten Bytes in
        // jedem 16-Bit-Wort (so stellt ihn der Emulator bereit, an dem die Sets
        // entstehen). Beispiel Sonic Jam: verlangt 0xX102ff8 = 0x813C0603;
        // das Saturn-Wort bei 0x06002FF8 lautet 0x0603813C, Bytes 06 03 81 3C.
        // Nur mit vertauschten Paaren (03 06 3C 81) ergibt die
        // Little-Endian-Lesung von rcheevos den verlangten Wert.
        //
        // Saturn-Wort b0 b1 b2 b3 (b0 hoechstwertig) -> RA-Bytes b1 b0 b3 b2.
        const uint8_t b0 = (uint8_t)(wert >> 24), b1 = (uint8_t)(wert >> 16);
        const uint8_t b2 = (uint8_t)(wert >> 8),  b3 = (uint8_t)wert;
        schatten_[ra + 0] = b1;
        schatten_[ra + 1] = b0;
        schatten_[ra + 2] = b3;
        schatten_[ra + 3] = b2;
    }
}

// Nach jeder Auswertung den Zustand aller Achievements neben das Programm
// schreiben, ra_bedingungen.txt (ueberschrieben). Je Achievement Zustand und
// je Bedingung der Hauptgruppe: Adresse, gelesener Wert, vorheriger Wert,
// Vergleich, erfuellt. Damit ist sichtbar, WORAN ein Achievement scheitert.
void RaSession::BedingungenAblegen() {
    char pfad[MAX_PATH];
    const DWORD n = ::GetModuleFileNameA(nullptr, pfad, MAX_PATH);
    std::string p(pfad, n);
    const size_t schr = p.find_last_of("\\/");
    p = (schr == std::string::npos ? std::string() : p.substr(0, schr + 1)) +
        "ra_bedingungen.txt";
    FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) return;
    static const char* kZustand[] = {"INAKTIV", "WARTET", "AKTIV", "PAUSE", "RESET",
                                     "AUSGELOEST", "BEREIT", "AUS"};
    for (uint32_t i = 0; i < laufzeit_->trigger_count; ++i) {
        const rc_trigger_t* t = laufzeit_->triggers[i].trigger;
        if (!t) continue;
        std::string titel;
        for (const core::RaAchievement& a : spiel_.achievements)
            if (a.id == laufzeit_->triggers[i].id) { titel = a.titel; break; }
        std::fprintf(f, "%u %s  [%s]\n", laufzeit_->triggers[i].id, titel.c_str(),
                     t->state < 8 ? kZustand[t->state] : "?");
        for (const rc_condset_t* cs = t->requirement; cs; cs = nullptr)
            for (const rc_condition_t* c = cs->conditions; c; c = c->next) {
                const rc_operand_t& o = c->operand1;
                uint32_t adr = 0, wert = 0, vorher = 0;
                if (o.value.memref && o.type != RC_OPERAND_CONST) {
                    adr = o.value.memref->address;
                    wert = o.value.memref->value.value;
                    vorher = o.value.memref->value.prior;
                }
                const uint32_t soll = (c->operand2.type == RC_OPERAND_CONST)
                                      ? c->operand2.value.num : 0;
                std::fprintf(f, "   %s adr %06X  wert %u  vorher %u  op %u  soll %u  %s\n",
                             o.type == RC_OPERAND_DELTA ? "d" : " ", adr, wert, vorher,
                             (unsigned)c->oper, soll, (c->is_true & 1) ? "WAHR" : "falsch");
            }
    }
    std::fclose(f);
}

// ---------------------------------------------------------------------------

void RaSession::Abholen(app::State& state) {
    std::lock_guard<std::mutex> g(sperre_);

    for (app::Logzeile& z : ausgabe_) state.log.push_back(std::move(z));
    ausgabe_.clear();

    if (erkannt_neu_) {
        erkannt_neu_ = false;
        state.spiel_erkannt = true;
        state.spiel_name = erkannt_name_;
        state.spiel_gebiet = erkannt_gebiete_;
        state.ra_kennt_abzug = erkannt_ra_;
        if (!erkannt_ra_) state.achievements.clear();
    }

    if (anmeldung_neu_) {
        anmeldung_neu_ = false;
        state.angemeldet = client_.Angemeldet();
        state.anmeldename = client_.Benutzer();
        if (!state.angemeldet) { state.achievements.clear(); state.spiel_name.clear(); }
    }

    if (spiel_neu_) {
        spiel_neu_ = false;
        state.achievements.clear();
        for (const core::RaAchievement& a : spiel_.achievements) {
            app::Achievement e;
            e.id = (int)a.id;
            e.titel = a.titel;
            e.beschreibung = a.beschreibung;
            e.punkte = a.punkte;
            e.freigeschaltet = a.freigeschaltet;
            state.achievements.push_back(std::move(e));
        }
        if (spiel_geladen_) {
            state.spiel_name = spiel_.titel;
            state.spiel_erkannt = true;
            state.ra_kennt_abzug = true;
        }
    }
    for (app::Freischaltung& f : einblenden_) state.neue_freischaltungen.push_back(std::move(f));
    einblenden_.clear();
}
