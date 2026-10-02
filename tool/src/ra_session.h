// Bindeglied zwischen Oberflaeche, RetroAchievements und der Auswertung.
//
// Alles, was auf das Netz wartet, laeuft in Nebenlaeufern; die Oberflaeche
// fragt einmal je Bild mit Abholen() nach. Die eigentliche Bedingungspruefung
// macht die rcheevos-Laufzeit, nicht eigener Code.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app_state.h"
#include "core/game_index.h"
#include "core/ra.h"
#include "core/setup.h"

struct rc_runtime_t;
struct rc_runtime_event_t;

class RaSession {
public:
    RaSession();
    ~RaSession();

    void AnmeldenStarten(const std::string& benutzer, const std::string& passwort);
    void Abmelden();
    bool Beschaeftigt() const { return arbeitet_.load(); }

    // Wird aufgerufen, sobald das laufende Spiel sich selbst gemeldet hat.
    // Kennung, Titel und Pruefsumme stammen alle aus dem Disc-Kopf, den der
    // Hook aus dem Arbeitsspeicher der Konsole mitliest. Damit wird bei
    // RetroAchievements direkt nachgefragt -- kein oertlicher Abgleich.
    void SpielLadenStarten(const std::string& kennung, const std::string& titel,
                           const std::string& md5, bool hardcore);
    // Mit welcher Pruefsumme zuletzt gefragt wurde. Weicht die gemeldete ab,
    // ruft das Hauptprogramm SpielLadenStarten erneut auf: Nach einem
    // Spielwechsel kann die richtige Pruefsumme erst nach einer falschen
    // kommen, bei gleicher Kennung.
    const std::string& GefragteMd5() const { return gefragte_md5_; }
    // Liste und Freischaltstand erneut von RetroAchievements holen, etwa nach
    // einem Neustart des Spiels oder einem Zuruecksetzen auf der Webseite.
    // Das Hauptprogramm fragt im naechsten Bild
    // neu, weil die gemeldete Pruefsumme dann nicht mehr die gefragte ist.
    void NeuLaden() { if (!arbeitet_.load()) gefragte_md5_.clear(); }

    // Hook fuer das laufende Spiel nachtragen, ohne Abbilddatei.
    //
    // Gedacht fuer den Fall „Spiel erkannt, aber kein Hook aktiv": Beim ersten
    // Start eines Spiels traegt das Werkzeug den Hook selbst in saroocfg.txt
    // ein, beim zweiten Start ist er da. Passiert je Spiel hoechstens einmal.
    // `senden` bekommt die fertigen M_-Zeilen und bringt sie zum Gerät. Ist es
    // leer oder liefert false, wird auf den Weg über die SD-Karte am PC
    // zurückgefallen.
    //
    // `sperre` sperrt SAVE und LOAD im Ingame-Menü (Hardcore). Wird mit einer
    // anderen Sperre erneut gebraucht, weil Hardcore umgeschaltet wurde, geht
    // der Hook-Satz noch einmal raus.
    void HookNachtragen(const std::function<bool(const std::string& kennung,
                                                 const std::string& inhalt)>& senden,
                        bool sperre);
    bool HookSchonNachgetragen(bool sperre) const {
        return hook_nachgetragen_ && gesendete_sperre_ == sperre;
    }
    // Erlaubt ein erneutes Nachtragen desselben Satzes.
    //
    // Ohne das ginge ein Hook-Satz nur einmal je Sitzung und Spiel raus. Wer
    // ihn im RetroAchievements-Eintrag der SAROO loescht, muesste sonst das
    // Werkzeug neu starten.
    void HookNachtragenErlauben() { hook_nachgetragen_ = false; }

    // Vorgemerkte Hooks schreiben, sobald die saroocfg.txt wieder erreichbar
    // ist. Beim Spielen steckt die SD-Karte in der SAROO, dann existiert das
    // Laufwerk am PC nicht und es kann gar nicht geschrieben werden. Statt
    // aufzugeben wird gewartet. Gefahrlos oft aufrufbar.
    void HooksNachreichen();
    int WartendeHooks() const { return (int)wartende_.size(); }

    // Verbindung weg: Spiel und Achievements vergessen, damit nichts
    // stehenbleibt, was nicht mehr gilt.
    void SpielVergessen();

    // Einmal je empfangenem Bericht: Werte uebernehmen und auswerten.
    void Auswerten(const std::vector<app::Watchwert>& werte, bool hook_laeuft,
                   bool hardcore);

    // Aenderungsprotokoll: genau ein Bild der Konsole, mit den Werten, die
    // sich darin geaendert haben (Saturn-Adresse, Wert). Wertet genau einmal
    // aus, wie ein Emulator nach jedem Bild. Nach einer Luecke wird erst
    // wieder ausgewertet, wenn alle Eintraege aufgefrischt sind
    // (`eintraege`), und die Laufzeit beginnt neu -- siehe ra_session.cpp.
    void AuswertenBild(const std::vector<std::pair<uint32_t, uint32_t>>& werte,
                       bool luecke, int eintraege, bool hardcore);

    void Abholen(app::State& state);
    void Beenden();

    const core::GameIndex& Index() const { return index_; }

    // Fuer das Einrichtungsfenster: es benutzt denselben angemeldeten Zugang.
    // Der Index ist nur noch ein Protokoll darueber, WAS eingerichtet wurde --
    // zur Erkennung wird er nicht mehr gelesen, die macht das laufende Spiel
    // selbst.
    core::RaClient& Client() { return client_; }
    core::GameIndex& IndexVeraenderlich() { return index_; }

private:
    void Melden(std::string text, app::Rang rang = app::Rang::Normal);
    void LaufzeitAufbauen();
    void BedingungenAblegen();          // ra_bedingungen.txt, nach jeder Auswertung
    void LaufzeitAbbauen();
    // Signaturen von rcheevos vorgegeben: Der Lese-Rueckruf liefert den Wert
    // direkt (little-endian zusammengesetzt), der Ereignis-Rueckruf bekommt
    // GAR KEINE Benutzerdaten -- deshalb der Umweg ueber eine aktive Sitzung.
    static uint32_t Lesen(uint32_t adresse, uint32_t bytes, void* ud);
    static void Ereignis(const rc_runtime_event_t* e);
    void Freischalten(uint32_t achievement_id);

    core::RaClient client_;
    core::GameIndex index_;
    core::Einstellungen einstellungen_;
    core::RaSpiel spiel_;
    rc_runtime_t* laufzeit_ = nullptr;

    std::map<uint32_t, uint8_t> schatten_;   // RA-Adresse -> Byte
    // Ein Saturn-Wort in den Schatten, mit der Vertauschung aus Auswerten.
    void SchattenSetzen(uint32_t saturn_adresse, uint32_t wert);
    // Bilder, die nach einer Luecke noch nicht ausgewertet werden.
    int sperr_bilder_ = 0;
    bool hardcore_ = false;
    bool spiel_geladen_ = false;
    std::string geladene_kennung_;
    // Pruefsumme der letzten Nachfrage. Neu gefragt wird, sobald die Konsole
    // eine andere meldet -- nicht nur bei neuer Kennung. Siehe GefragteMd5().
    std::string gefragte_md5_;
    bool hook_nachgetragen_ = false;
    bool gesendete_sperre_ = false;
    // Fertige Hook-Zeilen, die noch nicht geschrieben werden konnten.
    std::vector<core::SetupErgebnis> wartende_;
    bool wartend_gemeldet_ = false;

    std::thread thread_;
    std::atomic<bool> arbeitet_{false};

    std::mutex sperre_;
    std::vector<app::Logzeile> ausgabe_;
    bool anmeldung_neu_ = false;
    bool spiel_neu_ = false;

    // Identitaet des laufenden Spiels, unabhaengig von RetroAchievements.
    // Sie stammt aus dem Disc-Kopf und wird IMMER angezeigt, auch wenn RA den
    // Abzug nicht kennt oder niemand angemeldet ist.
    bool erkannt_neu_ = false;
    std::string erkannt_name_;
    std::string erkannt_gebiete_;
    bool erkannt_ra_ = false;
    // Freischaltungen samt geladenem Badge, bereit fuer die Einblendung.
    std::vector<app::Freischaltung> einblenden_;
};
