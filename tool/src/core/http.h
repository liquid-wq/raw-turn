// HTTPS ueber WinHTTP.
//
// Absichtlich klein gehalten: Die RetroAchievements-Anbindung braucht genau
// GET und POST mit Formularinhalt. Keine Fremdbibliothek, kein eigener
// TLS-Stapel -- WinHTTP gehoert zu Windows.
//
// Das Gegenstueck in mega-raw heisst ra_network und wurde fuer den ESP32
// gegen esp_http_client getauscht. Dieselbe Naht wird hier eingehalten: Nur
// diese Datei kennt die Plattform, alles darueber nicht.
#pragma once

#include <string>

namespace core {

struct HttpAntwort {
    bool ok = false;
    int status = 0;
    std::string koerper;
    std::string fehler;
    int wiederholen_nach = 0;   // Sekunden, aus Retry-After bei 429
};

// POST mit application/x-www-form-urlencoded.
HttpAntwort HttpPost(const std::string& host, const std::string& pfad,
                     const std::string& formular, int timeout_ms = 12000);

// Einfaches GET, etwa fuer Badge-Bilder.
HttpAntwort HttpGet(const std::string& host, const std::string& pfad,
                    int timeout_ms = 8000);

// Anhaengen eines Formularfelds, korrekt kodiert.
void FormularFeld(std::string& formular, const std::string& name,
                  const std::string& wert);

std::string UrlKodieren(const std::string& s);

}  // namespace core
