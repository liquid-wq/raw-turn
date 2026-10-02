// Nachschlagetabelle Spielkennung -> RA-Spiel.
//
// Wird beim Einrichten des ROM-Ordners nebenbei gefuellt. Damit laesst sich
// allein aus der Kennung, die das laufende Spiel selbst meldet, das richtige
// Achievement-Set laden -- ohne Disc-Abbild auszuwaehlen und ohne im Betrieb
// auf die SD-Karte zuzugreifen. Dateiformat JSON.
#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace core {

struct IndexEintrag {
    uint32_t gameid = 0;        // 0 = RA kennt diesen Abzug nicht
    std::string name;           // RA-Klarname, sonst Titel aus dem Disc-Kopf
    std::string titel;          // immer aus dem Disc-Kopf
    std::string md5;
    std::string abbild;
    std::string gebiete;
    int achievements = 0;
};

class GameIndex {
public:
    bool Laden(const std::string& pfad);
    bool Speichern(const std::string& pfad) const;

    const IndexEintrag* Suchen(const std::string& kennung) const;
    // Gibt true zurueck, wenn die Kennung neu war.
    bool Setzen(const std::string& kennung, const IndexEintrag& e);

    size_t Anzahl() const { return eintraege_.size(); }
    const std::map<std::string, IndexEintrag>& Alle() const { return eintraege_; }

private:
    std::map<std::string, IndexEintrag> eintraege_;
};

// Fester Datenordner unter %LOCALAPPDATA%\ra-saturn, unabhaengig davon, wo
// die Exe liegt.
std::string DatenOrdner();
std::string StandardIndexPfad();
std::string EinstellungsPfad();

// Einfache Einstellungen, Format "schluessel=wert" je Zeile.
struct Einstellungen {
    std::string rom_ordner;
    std::string saroocfg;
    std::string benutzer;
    std::string port;
    std::string stil;        // Farbschema, siehe theme.h; leer = Voreinstellung
    std::string sprache;     // "de" oder "en", siehe core/i18n.h; leer = Deutsch
    std::string goldblitz;   // "0" = aus; leer oder "1" = an
    std::string anzeige;     // Freischaltung: "werkzeug" = im Fenster; leer = Bildschirmrand

    bool Laden(const std::string& pfad = EinstellungsPfad());
    bool Speichern(const std::string& pfad = EinstellungsPfad()) const;
};

}  // namespace core
