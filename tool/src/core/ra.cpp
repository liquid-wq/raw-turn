#include "core/ra.h"

#include <cstdio>
#include <cstring>

#include "core/http.h"
#include "core/i18n.h"

extern "C" {
#include "rc_api_request.h"
#include "rc_api_runtime.h"
#include "rc_api_user.h"
#include "rc_hash.h"
#include "rc_consoles.h"
}

namespace core {
namespace {

// RA fuehrt Achievements in Kategorien. 3 ist der Kernsatz; alles andere sind
// inoffizielle Eintraege, die nicht mitzaehlen.
constexpr uint32_t kKategorieKern = 3;

// "https://retroachievements.org/dorequest.php" -> Host und Pfad.
bool ZerlegeUrl(const char* url, std::string* host, std::string* pfad) {
    if (!url) return false;
    std::string u = url;
    const std::string schema = "https://";
    if (u.rfind(schema, 0) == 0) u = u.substr(schema.size());
    else if (u.rfind("http://", 0) == 0) u = u.substr(7);
    const size_t schr = u.find('/');
    if (schr == std::string::npos) { *host = u; *pfad = "/"; }
    else { *host = u.substr(0, schr); *pfad = u.substr(schr); }
    return !host->empty();
}

// Eine rcheevos-Anfrage absetzen. Gibt den Antwortkoerper zurueck.
bool Absetzen(const rc_api_request_t& anfrage, std::string* koerper,
              int* status, std::string* fehler) {
    std::string host, pfad;
    if (!ZerlegeUrl(anfrage.url, &host, &pfad)) {
        if (fehler) *fehler = T("Ungültige Server-Adresse.");
        return false;
    }
    const std::string inhalt = anfrage.post_data ? anfrage.post_data : "";
    HttpAntwort a = HttpPost(host, pfad, inhalt);

    // Auch bei 4xx liefert RA eine auswertbare Fehlerbeschreibung mit, die
    // rcheevos lesen kann. Deshalb nur bei leerem Koerper abbrechen.
    if (a.koerper.empty()) {
        if (fehler) {
            *fehler = a.fehler.empty() ? T("Keine Antwort vom Server.") : a.fehler;
            if (a.status == 429) *fehler = T("Anfragelimit erreicht, später erneut versuchen.");
        }
        return false;
    }
    *koerper = std::move(a.koerper);
    if (status) *status = a.status;
    return true;
}

// Antwort so verpacken, wie rcheevos sie erwartet.
rc_api_server_response_t Verpacken(const std::string& koerper, int status) {
    rc_api_server_response_t s = {};
    s.body = koerper.c_str();
    s.body_length = koerper.size();
    s.http_status_code = status;
    return s;
}

bool PruefeAntwort(const rc_api_response_t& r, int rc, std::string* fehler) {
    if (rc != RC_OK) {
        if (fehler) *fehler = rc_error_str(rc);
        return false;
    }
    if (!r.succeeded) {
        if (fehler) *fehler = r.error_message ? r.error_message
                                              : T("Der Server hat die Anfrage abgelehnt.");
        return false;
    }
    return true;
}

}  // namespace

bool RaClient::Anmelden(const std::string& benutzer, const std::string& passwort,
                        std::string* fehler) {
    rc_api_login_request_t anfrage = {};
    anfrage.username = benutzer.c_str();
    anfrage.password = passwort.c_str();

    rc_api_request_t r = {};
    int rc = rc_api_init_login_request(&r, &anfrage);
    if (rc != RC_OK) { if (fehler) *fehler = rc_error_str(rc); return false; }

    std::string koerper;
    int status = 0;
    const bool ok = Absetzen(r, &koerper, &status, fehler);
    rc_api_destroy_request(&r);
    if (!ok) return false;

    rc_api_login_response_t antwort = {};
    const rc_api_server_response_t serverantwort = Verpacken(koerper, status);
    rc = rc_api_process_login_server_response(&antwort, &serverantwort);
    if (!PruefeAntwort(antwort.response, rc, fehler)) {
        rc_api_destroy_login_response(&antwort);
        return false;
    }
    benutzer_ = antwort.username ? antwort.username : benutzer;
    token_ = antwort.api_token ? antwort.api_token : "";
    rc_api_destroy_login_response(&antwort);

    if (token_.empty()) {
        if (fehler) *fehler = T("Anmeldung ohne Zugangsmarke, das sollte nicht vorkommen.");
        return false;
    }
    return true;
}

bool RaClient::SpielAusHash(const std::string& md5, uint32_t* spielkennung,
                            std::string* fehler) {
    if (spielkennung) *spielkennung = 0;

    rc_api_resolve_hash_request_t anfrage = {};
    anfrage.username = benutzer_.c_str();
    anfrage.api_token = token_.c_str();
    anfrage.game_hash = md5.c_str();

    rc_api_request_t r = {};
    int rc = rc_api_init_resolve_hash_request(&r, &anfrage);
    if (rc != RC_OK) { if (fehler) *fehler = rc_error_str(rc); return false; }

    std::string koerper;
    int status = 0;
    const bool ok = Absetzen(r, &koerper, &status, fehler);
    rc_api_destroy_request(&r);
    if (!ok) return false;

    rc_api_resolve_hash_response_t antwort = {};
    const rc_api_server_response_t serverantwort = Verpacken(koerper, status);
    rc = rc_api_process_resolve_hash_server_response(&antwort, &serverantwort);
    if (!PruefeAntwort(antwort.response, rc, fehler)) {
        rc_api_destroy_resolve_hash_response(&antwort);
        return false;
    }
    if (spielkennung) *spielkennung = antwort.game_id;
    rc_api_destroy_resolve_hash_response(&antwort);
    return true;
}

bool RaClient::SpielLaden(uint32_t spielkennung, const std::string& hash,
                          RaSpiel* aus, std::string* fehler) {
    rc_api_fetch_game_data_request_t anfrage = {};
    anfrage.username = benutzer_.c_str();
    anfrage.api_token = token_.c_str();
    anfrage.game_id = spielkennung;
    if (!hash.empty()) anfrage.game_hash = hash.c_str();

    rc_api_request_t r = {};
    int rc = rc_api_init_fetch_game_data_request(&r, &anfrage);
    if (rc != RC_OK) { if (fehler) *fehler = rc_error_str(rc); return false; }

    std::string koerper;
    int status = 0;
    const bool ok = Absetzen(r, &koerper, &status, fehler);
    rc_api_destroy_request(&r);
    if (!ok) return false;

    rc_api_fetch_game_data_response_t antwort = {};
    const rc_api_server_response_t serverantwort = Verpacken(koerper, status);
    rc = rc_api_process_fetch_game_data_server_response(&antwort, &serverantwort);
    if (!PruefeAntwort(antwort.response, rc, fehler)) {
        rc_api_destroy_fetch_game_data_response(&antwort);
        return false;
    }

    aus->id = antwort.id;
    aus->titel = antwort.title ? antwort.title : "";
    aus->hash = hash;
    aus->achievements.clear();
    aus->geliefert = (int)antwort.num_achievements;
    aus->inoffiziell = 0;
    for (uint32_t i = 0; i < antwort.num_achievements; ++i) {
        const rc_api_achievement_definition_t& a = antwort.achievements[i];
        if (a.category != kKategorieKern) { ++aus->inoffiziell; continue; }
        RaAchievement e;
        e.id = a.id;
        e.titel = a.title ? a.title : "";
        e.beschreibung = a.description ? a.description : "";
        e.definition = a.definition ? a.definition : "";
        e.badge = a.badge_name ? a.badge_name : "";
        e.punkte = (int)a.points;
        aus->achievements.push_back(std::move(e));
    }
    rc_api_destroy_fetch_game_data_response(&antwort);
    return true;
}

bool RaClient::SitzungStarten(RaSpiel* spiel, bool hardcore, std::string* fehler) {
    rc_api_start_session_request_t anfrage = {};
    anfrage.username = benutzer_.c_str();
    anfrage.api_token = token_.c_str();
    anfrage.game_id = spiel->id;
    if (!spiel->hash.empty()) anfrage.game_hash = spiel->hash.c_str();
    anfrage.hardcore = hardcore ? 1 : 0;

    rc_api_request_t r = {};
    int rc = rc_api_init_start_session_request(&r, &anfrage);
    if (rc != RC_OK) { if (fehler) *fehler = rc_error_str(rc); return false; }

    std::string koerper;
    int status = 0;
    const bool ok = Absetzen(r, &koerper, &status, fehler);
    rc_api_destroy_request(&r);
    if (!ok) return false;

    rc_api_start_session_response_t antwort = {};
    const rc_api_server_response_t serverantwort = Verpacken(koerper, status);
    rc = rc_api_process_start_session_server_response(&antwort, &serverantwort);
    if (!PruefeAntwort(antwort.response, rc, fehler)) {
        rc_api_destroy_start_session_response(&antwort);
        return false;
    }

    // Nur Eintraege uebernehmen, die es im geladenen Satz auch gibt. Die
    // Sitzungsantwort enthaelt auch zurueckgezogene und fremde Kennungen --
    // genau daran lag der Fehler "17 Achievements, 1 bereits freigeschaltet",
    // waehrend alle Zeilen auf gesperrt standen.
    const rc_api_unlock_entry_t* liste =
        hardcore ? antwort.hardcore_unlocks : antwort.unlocks;
    const uint32_t anzahl =
        hardcore ? antwort.num_hardcore_unlocks : antwort.num_unlocks;
    for (uint32_t i = 0; i < anzahl; ++i)
        for (RaAchievement& a : spiel->achievements)
            if (a.id == liste[i].achievement_id) { a.freigeschaltet = true; break; }

    rc_api_destroy_start_session_response(&antwort);
    return true;
}

bool RaClient::Freischalten(uint32_t achievement_id, const std::string& hash,
                            bool hardcore, std::string* fehler) {
    rc_api_award_achievement_request_t anfrage = {};
    anfrage.username = benutzer_.c_str();
    anfrage.api_token = token_.c_str();
    anfrage.achievement_id = achievement_id;
    anfrage.hardcore = hardcore ? 1 : 0;
    if (!hash.empty()) anfrage.game_hash = hash.c_str();

    rc_api_request_t r = {};
    int rc = rc_api_init_award_achievement_request(&r, &anfrage);
    if (rc != RC_OK) { if (fehler) *fehler = rc_error_str(rc); return false; }

    std::string koerper;
    int status = 0;
    const bool ok = Absetzen(r, &koerper, &status, fehler);
    rc_api_destroy_request(&r);
    if (!ok) return false;

    rc_api_award_achievement_response_t antwort = {};
    const rc_api_server_response_t serverantwort = Verpacken(koerper, status);
    rc = rc_api_process_award_achievement_server_response(&antwort, &serverantwort);
    const bool gut = PruefeAntwort(antwort.response, rc, fehler);
    rc_api_destroy_award_achievement_response(&antwort);
    return gut;
}

bool RaClient::Ping(uint32_t spielkennung, const std::string& hash, bool hardcore) {
    rc_api_ping_request_t anfrage = {};
    anfrage.username = benutzer_.c_str();
    anfrage.api_token = token_.c_str();
    anfrage.game_id = spielkennung;
    if (!hash.empty()) anfrage.game_hash = hash.c_str();
    anfrage.hardcore = hardcore ? 1 : 0;

    rc_api_request_t r = {};
    if (rc_api_init_ping_request(&r, &anfrage) != RC_OK) return false;
    std::string koerper;
    int status = 0;
    const bool ok = Absetzen(r, &koerper, &status, nullptr);
    rc_api_destroy_request(&r);
    return ok;
}

std::string SaturnHash(const std::string& pfad) {
    char hash[33] = {};
    if (!rc_hash_generate_from_file(hash, RC_CONSOLE_SATURN, pfad.c_str())) return {};
    return hash;
}

}  // namespace core
