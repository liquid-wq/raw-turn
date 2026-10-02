// RetroAchievements-Anbindung.
//
// Nutzt die Anfrage- und Antwortfunktionen von rcheevos (rc_api_*). Damit
// entfallen URL-Bau und JSON-Parser vollstaendig, und das Verhalten bleibt
// automatisch mit dem gleich, was RetroAchievements selbst pflegt.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace core {

struct RaAchievement {
    uint32_t id = 0;
    std::string titel;
    std::string beschreibung;
    std::string definition;    // MemAddr, wird an die Laufzeit gegeben
    std::string badge;         // Bildname, https://media.retroachievements.org/Badge/<badge>.png
    int punkte = 0;
    bool freigeschaltet = false;
};

struct RaSpiel {
    uint32_t id = 0;
    std::string titel;
    std::string hash;
    std::vector<RaAchievement> achievements;
    // Wie viele RetroAchievements insgesamt geliefert hat, und wie viele davon
    // nicht im Kernsatz stehen. Ohne diese beiden Zahlen sieht ein Spiel ohne
    // Achievements genauso aus wie eines, dessen Satz nur inoffiziell ist.
    int geliefert = 0;
    int inoffiziell = 0;
};

class RaClient {
public:
    bool Anmelden(const std::string& benutzer, const std::string& passwort,
                  std::string* fehler);
    void Abmelden() { benutzer_.clear(); token_.clear(); }
    bool Angemeldet() const { return !token_.empty(); }
    const std::string& Benutzer() const { return benutzer_; }

    // MD5 -> Spielkennung. 0 und kein Fehler heisst: RA kennt den Abzug nicht.
    bool SpielAusHash(const std::string& md5, uint32_t* spielkennung,
                      std::string* fehler);

    bool SpielLaden(uint32_t spielkennung, const std::string& hash,
                    RaSpiel* aus, std::string* fehler);

    // Markiert in `spiel` die bereits freigeschalteten Achievements.
    bool SitzungStarten(RaSpiel* spiel, bool hardcore, std::string* fehler);

    bool Freischalten(uint32_t achievement_id, const std::string& hash,
                      bool hardcore, std::string* fehler);

    bool Ping(uint32_t spielkennung, const std::string& hash, bool hardcore);

private:
    std::string benutzer_;
    std::string token_;
};

// MD5 eines Disc-Abbilds nach den Regeln von RetroAchievements, berechnet von
// rcheevos selbst. Leerer String bei Fehler.
std::string SaturnHash(const std::string& pfad);

}  // namespace core
