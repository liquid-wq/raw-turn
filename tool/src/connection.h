// Serieller Live-Betrieb in einem eigenen Thread.
//
// Die Oberflaeche darf nicht blockieren, also laeuft das Lesen nebenher und
// legt Ergebnisse unter einer Sperre ab. `Abholen()` wird einmal je Bild vom
// Zeichen-Thread aufgerufen und uebertraegt alles in den app::State.
//
// Diagnose eingebaut: Solange kein gueltiger Frame ankam, wird alle paar
// Sekunden gemeldet, WAS am Anschluss ankommt. Ohne das sehen zwei voellig
// verschiedene Fehler gleich aus -- "Anschluss, Rate oder Verkabelung falsch"
// (gar keine Bytes) und "Gerät redet, aber der Hook sendet nicht" (Bytes, aber
// keine Frames, weil die SAROO auf derselben Leitung Klartext ausgibt).
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "app_state.h"
#include "core/protocol.h"
#include "core/serial.h"

class Connection {
public:
    ~Connection();

    bool Starten(const std::string& anschluss, int baud, std::string* fehler);
    void Beenden();
    bool Laeuft() const { return laeuft_.load(); }

    // Vom Zeichen-Thread, einmal je Bild.
    void Abholen(app::State& state);

    // Aenderungsprotokoll: ein vollstaendiges Bild der
    // Konsole mit allen Werten, die sich darin geaendert haben, schon auf
    // Saturn-Adressen umgesetzt. `luecke`: davor fehlen Bilder.
    struct ProtBild {
        uint32_t bild = 0;
        bool luecke = false;
        std::vector<std::pair<uint32_t, uint32_t>> werte;   // Adresse, Wert
    };
    // Alle seit dem letzten Aufruf fertigen Bilder, in Reihenfolge.
    std::vector<ProtBild> ProtokollAbholen();
    // Kam in den letzten zwei Sekunden ein Protokollrahmen?
    bool ProtokollAktiv();

    // Einen Hook-Satz zum Gerät schicken, damit die SAROO ihn selbst auf ihre
    // SD-Karte schreibt. Beim Spielen steckt die Karte in der SAROO, der PC
    // kommt gar nicht heran — deshalb dieser Weg.
    //
    // Der Lese-Thread übernimmt das Senden, damit nichts zwischen die Frames
    // gerät. Rückmeldung kommt später über das Protokoll.
    void HookSenden(const std::string& kennung, const std::string& inhalt);
    bool HookSendeLaeuft() const { return hook_sendet_.load(); }

    // Speicherstand-Auftrag an die Saturn (core::kStand...). Der Lese-Thread
    // sendet ihn; die Antwort kommt über das Protokoll und steht danach im
    // app::State (stand_*).
    void StandAuftrag(uint8_t vorgang, uint8_t slot);

    // Goldblitz am Fernseher starten (core::GoldblitzFolge). Der Lese-Thread
    // sendet die Stufen im Takt; ein erneuter Aufruf beginnt von vorn.
    void GoldblitzStarten();

    // Eine Datei auf die SD-Karte der SAROO schieben, während sie im Gerät
    // steckt. `zielpfad` ist der Pfad auf der Karte, etwa "/SAROO/ssfirm.bin".
    //
    // Die Datei wird hier ganz eingelesen; der Lese-Thread schickt sie danach
    // blockweise und wartet je Block die Quittung des Geräts ab. Das dauert bei
    // einer halben Megabyte großen Datei etwa eine halbe Minute.
    //
    // false, wenn die Datei nicht lesbar ist oder schon ein Transfer läuft.
    bool DateiSenden(const std::string& quelle, const std::string& zielpfad,
                     std::string* fehler);
    bool DateiSendeLaeuft() const { return datei_sendet_.load(); }
    // 0 bis 100, nur währenddessen aussagekräftig.
    int  DateiFortschritt() const { return datei_prozent_.load(); }
    // Bricht einen laufenden Transfer ab. Das Gerät räumt seine Zwischendatei
    // beim nächsten Versuch selbst weg, die Zieldatei bleibt unberührt.
    void DateiAbbrechen();

private:
    void Schleife(std::string anschluss, int baud);
    void Melden(std::string text, app::Rang rang = app::Rang::Normal);

    // Eine vollstaendig eingesammelte Frame-Gruppe uebernehmen: Werte
    // veroeffentlichen und Stillstand fortschreiben.
    void GruppeUebernehmen(long frames_ok, long crc_fehler);

    // Ein GAME_INFO-Teil ist angekommen. Sobald alle vier da sind, entstehen
    // daraus Titel, Kennung, Gebiet und RA-Pruefsumme.
    void GameInfoTeil(int offset, const std::vector<uint8_t>& daten);

    // Datensaetze eines Protokollrahmens einsortieren. Lese-Thread.
    void ProtSaetze(bool luecke, const std::vector<core::ProtSatz>& saetze);

    // Aenderungsprotokoll. prot_offen_ und die drei danach gehoeren dem
    // Lese-Thread, der Rest steht unter sperre_.
    std::vector<std::pair<uint16_t, uint32_t>> prot_offen_;   // Eintrag, Wert
    bool prot_luecke_ = true;
    bool prot_bild_bekannt_ = false;
    uint32_t prot_letztes_bild_ = 0;
    std::vector<ProtBild> prot_fertig_;
    std::vector<uint32_t> prot_werte_;     // je Eintrag der zuletzt gemeldete Wert
    unsigned long prot_zuletzt_ = 0;       // GetTickCount, 0 = nie
    uint32_t prot_dauer_us_ = 0, prot_max_us_ = 0;
    bool prot_zeit_da_ = false;
    long prot_luecken_ = 0;

    std::thread thread_;
    std::atomic<bool> laeuft_{false};
    std::atomic<bool> abbruch_{false};

    std::mutex sperre_;
    std::vector<app::Logzeile> ausgabe_;
    std::vector<core::WatchEntry> letzte_;
    std::vector<core::WatchEntry> vorletzte_;

    // Sammelstelle fuer mehrteilige Berichte. Ein UART-Frame fasst 27
    // Eintraege, der Disc-Kopf allein braucht 128 -- die Firmware verteilt
    // einen Schnappschuss deshalb auf mehrere Frames mit gleichem FRAME_COUNT.
    // Nur der Lese-Thread ruehrt diese drei an, sie brauchen keine Sperre.
    std::vector<core::WatchEntry> sammler_;
    uint32_t sammel_zaehler_ = 0;
    bool sammel_bekannt_ = false;
    uint32_t letzter_zaehler_ = 0;
    bool zaehler_bekannt_ = false;
    bool erste_werte_gemeldet_ = false;

    // Auftrag an den Lese-Thread: diesen Hook-Satz senden.
    std::atomic<bool> hook_sendet_{false};
    std::string hook_kennung_;
    std::string hook_inhalt_;
    bool hook_auftrag_ = false;
    // Spiel auf der Konsole neu gestartet (Bildzaehler faengt wieder klein an).
    bool spiel_neustart_ = false;

    // Speicherstaende: Auftrag unter sperre_, Antwort ebenso.
    bool stand_auftrag_ = false;
    uint8_t stand_vorgang_ = 0, stand_slot_ = 0;
    bool stand_neu_ = false;
    core::StandAntwort stand_antwort_;
    bool stand_bekannt_ = false;
    unsigned long stand_gesendet_ = 0;    // GetTickCount, 0 = nichts offen

    // Goldblitz: Startzeit (GetTickCount, 0 = keiner). Nur der Lese-Thread
    // fuehrt blitz_lauf_ und blitz_gesendet_.
    std::atomic<unsigned long> blitz_start_{0};
    unsigned long blitz_lauf_ = 0;
    int blitz_gesendet_ = -1;

    // Dateitransfer zum Geraet. Auftrag und Fortschritt liegen unter sperre_
    // bzw. als Atomic; der Ablauf selbst gehoert allein dem Lese-Thread, damit
    // kein zweiter Schreiber zwischen die Rahmen geraet.
    enum class DateiStand { Aus, Anmelden, WarteBegin, Block, WarteBlock,
                            WarteEnde };
    std::atomic<bool> datei_sendet_{false};
    std::atomic<int>  datei_prozent_{0};
    std::atomic<bool> datei_abbruch_{false};
    std::vector<uint8_t> datei_inhalt_;   // unter sperre_ uebergeben
    std::string datei_ziel_;
    std::string datei_quelle_;
    bool datei_auftrag_ = false;          // unter sperre_

    DateiStand datei_stand_ = DateiStand::Aus;
    std::vector<uint8_t> datei_daten_;    // Arbeitskopie, nur Lese-Thread
    std::string datei_pfad_;              // Ziel auf der Karte
    std::string datei_von_;               // gewaehlte Quelldatei, fuer die Meldung
    size_t datei_ab_ = 0;                 // schon gesendete Bytes
    uint32_t datei_summe_ = 0;
    uint8_t datei_seq_ = 0;
    unsigned long datei_gewartet_ = 0;    // GetTickCount beim letzten Senden
    unsigned long datei_beginn_ = 0;

    // Den Transfer beenden und melden. Laeuft im Lese-Thread.
    void DateiEnde(bool gut, const std::string& text);
    // Den Transfer einen Schritt weiterbringen. Laeuft im Lese-Thread.
    void DateiSchritt(core::SerialPort& port);
    // Quittung des Geraets zu einem Dateirahmen auswerten.
    void DateiAck(uint8_t auf_typ, uint8_t status);
    // Alles aus dem Disc-Kopf, den die Firmware von sich aus schickt. Keine
    // Datei, kein Hook, keine Config-Zeile. `md5_` ist die RA-Pruefsumme.
    core::DiscKopfSammler kopf_;
    std::string kennung_;
    std::string titel_;
    std::string gebiete_;
    std::string md5_;
    bool kennung_neu_ = false;
    // Nach einem Spielwechsel: Berichte erst uebernehmen, wenn sich der
    // Bildzaehler bewegt. Siehe GruppeUebernehmen.
    bool nach_wechsel_ = false;
    bool wechsel_zaehler_bekannt_ = false;
    uint32_t wechsel_zaehler_ = 0;
    uint32_t bildzaehler_ = 0;
    bool daten_ = false;
    long frames_ok_ = 0;
    long crc_fehler_ = 0;
    // Nur WATCH_REPORTs. Sie allein beweisen einen laufenden Hook; GAME_INFO
    // schickt die Firmware auch ohne jeden Hook.
    long watch_frames_ = 0;
    int stillstand_ = 0;
    int crc_gemeldet_ = 0;                // nur der Lese-Thread
    // Rahmen, die ein Neustart des Geraets zerrissen hat. Werden vom
    // CRC-Zaehler abgezogen, siehe GruppeUebernehmen.
    long crc_neustart_ = 0;
    int groesse_gemeldet_ = 0;            // unter sperre_
    int kleiner_folge_ = 0;               // unter sperre_, siehe GruppeUebernehmen
    unsigned long zaehler_wechsel_ = 0;   // GetTickCount beim letzten neuen Bildzaehler
    double bilder_pro_sekunde_ = 0.0;

    // Proben fuer die gemittelte Bildrate: eine je Sekunde, die letzten fuenf.
    static constexpr int kRateProben = 6;   // 6 Proben = 5 Sekunden Abstand
    struct RateProbe { unsigned long zeit; uint32_t bild; };
    RateProbe rate_proben_[kRateProben] = {};
    int rate_kopf_ = 0;
    int rate_anzahl_ = 0;
    // Zeitpunkt der letzten Aenderung eines beobachteten Werts, 0 = noch nie.
    unsigned long letzte_aenderung_ = 0;
    bool getrennt_gemeldet_ = false;
    // Gab es in dieser Programmsitzung schon eine Verbindung? Nur dann ist
    // „Verbindung verloren" eine wahre Aussage.
    bool jemals_verbunden_ = false;

    // Welcher Firmware-Stand laeuft wirklich? Aus der Klartextausgabe der
    // SAROO mitgelesen. Der Zeilenpuffer gehoert dem Lese-Thread, das Ergebnis
    // steht unter sperre_.
    void KlartextZeile(const std::string& zeile);
    void KlartextStrom(const std::string& roh);
    std::string klartext_rest_;          // nur der Lese-Thread
    bool bootrom_erwartet_ = false;      // "Size" gleich danach gehoert zur ssfirm
    app::State::Firmware firmware_;      // unter sperre_

    // Zustand des Geraets, nicht nur des Anschlusses.
    app::State::Geraet geraet_ = app::State::Geraet::Getrennt;
    std::string hinweis_;
    std::string probe_;      // letzter Klartextschnipsel, zur Anzeige
    long rohbytes_ = 0;
};
