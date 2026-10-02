// Der Zustand, den die Oberflaeche anzeigt.
//
// WICHTIG: Hier stehen keine Beispieldaten. Alles ist leer, bis die
// Kernschicht es fuellt. Eine Oberflaeche, die erfundene Werte zeigt, als
// waeren sie echt ("Verbunden mit COM3", "angemeldet als ..."), fuehrt in die
// Irre.
//
// Diese Struktur ist bewusst frei von Oberflaechen-Bibliothek. Sie ist die
// Naht, an der spaeter der echte Protokoll-, Mailbox- und RA-Teil andockt,
// nach derselben Regel wie in mega-raw.
#pragma once

#include <string>
#include <vector>

namespace app {

struct Achievement {
    int id = 0;
    std::string titel;
    std::string beschreibung;   // Tooltip in der Liste
    int punkte = 0;
    bool freigeschaltet = false;
};

// Frisch freigeschaltet, fuer die Einblendung. Das Badge ist das rohe PNG,
// bereits geladen; leer, wenn das Laden nicht geklappt hat.
struct Freischaltung {
    std::string titel;
    std::string beschreibung;
    int punkte = 0;
    std::string badge_png;
};

struct Watchwert {
    unsigned adresse = 0;
    unsigned wert = 0;
    bool geaendert = false;
};

enum class Rang { Normal, Gut, Warnung, Wichtig };

struct Logzeile {
    std::string text;
    Rang rang = Rang::Normal;
};

struct State {
    // --- Anmeldung ---
    char benutzer[64] = "";
    char passwort[64] = "";
    bool zugangsdaten_speichern = false;
    bool angemeldet = false;
    std::string anmeldename;

    // --- Verbindung ---
    //
    // Vier Stufen statt eines Ja/Nein. Ein offener Anschluss beweist gar
    // nichts: Ein USB-TTL-Adapter laesst sich immer oeffnen, auch wenn nichts
    // daran haengt. Erst SAROO-Klartext beweist das Geraet, erst Frames
    // beweisen den Hook.
    enum class Geraet {
        Getrennt,
        PortOffen,      // Anschluss offen, aber noch nichts gehoert
        Stumm,          // offen, nichts kommt -- Konsole aus oder falsche Leitung
        SarooErkannt,   // SAROO redet, aber keine Frames: Hook fehlt
        HookLaeuft,     // Frames kommen an
        HookSteht,      // Frames kommen, Bildzaehler steht
    } geraet = Geraet::Getrennt;

    char port[32] = "";
    char baud[16] = "1000000";
    bool verbunden = false;          // Anschluss offen, nicht mehr
    std::string geraet_hinweis;      // Klartext zum aktuellen Zustand
    std::string saroo_probe;         // letzter Klartextschnipsel vom Geraet
    std::vector<std::string> gefundene_ports;

    // Hook-Zustand. Unbekannt, solange keine Frames ankommen.
    enum class Hook { Unbekannt, Laeuft, Steht } hook = Hook::Unbekannt;

    // --- Welcher Firmware-Stand läuft gerade wirklich? ---
    //
    // Alles hieraus liest die Kernschicht aus der Klartextausgabe der SAROO
    // mit. Ohne diese Anzeige steht es nur im durchlaufenden Protokoll, und
    // dort fällt nicht auf, wenn ein Stand gar nicht auf der Karte gelandet
    // ist.
    //
    // Leer heißt immer "noch nicht gehört", nie "nicht vorhanden".
    struct Firmware {
        std::string stm32_bau;      // Baudatum der laufenden App, "SSMaster start!"
        std::string ra_bau;         // Baudatum unseres Patches, aus den ra:-Zeilen
        bool ra_patch = false;      // unser Patch meldet sich überhaupt
        long ssfirm_bytes = 0;      // Größe der geladenen Saturn-Firmware
        std::string spiel_abschnitt;  // Abschnitt aus saroocfg.txt, den das Gerät zog
        int hook_paare = -1;        // M_-Paare aus dem Hook-Satz, -1 = unbekannt
    } firmware;

    // --- Spiel ---
    bool hardcore = false;
    bool spiel_erkannt = false;
    std::string spiel_name;       // Klarname von RA, sonst Titel aus dem Disc-Kopf
    std::string spiel_kennung;    // Produktnummer + Version
    std::string spiel_gebiet;
    // Von der Konsole gerechnet, nicht aus einer Datei: MD5 ueber die 512 Byte
    // Disc-Kopf, die das laufende Spiel selbst gemeldet hat.
    std::string spiel_md5;
    bool ra_kennt_abzug = false;

    // --- Einrichtung ---
    std::string rom_ordner;
    int eingerichtet = 0;
    int gefunden = 0;
    int ohne_set = 0;

    // --- Live ---
    bool daten_empfangen = false;
    unsigned bildzaehler = 0;
    float bilder_pro_sekunde = 0.0f;          // gemittelt ueber fuenf Sekunden
    float sekunden_seit_aenderung = -1.0f;    // -1: noch nie ein Wert geaendert
    int crc_fehler = 0;

    // Aenderungsprotokoll: Wird Bild fuer Bild
    // ausgewertet? Dazu die Dauer der Erfassung auf der Saturn je Bild.
    bool prot_aktiv = false;
    bool prot_zeit_da = false;
    unsigned prot_dauer_us = 0;
    unsigned prot_max_us = 0;
    long prot_luecken = 0;

    // --- Speicherstände ---
    //
    // Slot 0..3 werden hier gewählt, 4..13 sind die fortlaufenden
    // Schnellstände "X.n" (L + R + Unten). Leer, bis die Konsole die Übersicht
    // geschickt hat.
    struct StandSlot {
        bool belegt = false;
        std::string zeit;          // "TT.MM.JJJJ  hh:mm", leer wenn frei
        int folge = 0;             // Schnellstand: Nummer n in "X.n"
        std::string spielzeit;     // "h:mm:ss" seit dem Spielstart
    };
    static constexpr int kStandSlots = 14;
    static constexpr int kStandSlotX0 = 4;
    StandSlot stand_slots[kStandSlots];
    bool stand_bekannt = false;    // Übersicht schon einmal empfangen
    bool stand_laeuft = false;     // Auftrag unterwegs, Antwort steht aus

    // Einmalig gesetzt, wenn das Spiel auf der Konsole neu gestartet wurde;
    // das Hauptprogramm holt dann den Stand der Achievements neu und loescht es.
    bool spiel_neustart = false;

    // Statuswort der Saturn-Firmware zur Hardcore-Sperre, siehe
    // core/mailbox.h. Kommt als eigener Watch-Eintrag und steht deshalb nicht
    // in `werte`. Solange es fehlt, gilt die Sperre als nicht bestaetigt.
    bool saturn_status_da = false;
    unsigned saturn_status = 0;

    std::vector<Achievement> achievements;
    std::vector<Freischaltung> neue_freischaltungen;   // die Oberflaeche leert die Liste
    std::vector<Watchwert> werte;
    std::vector<Logzeile> log;

    void Protokoll(std::string text, Rang rang = Rang::Normal) {
        log.push_back({std::move(text), rang});
    }

    int freigeschaltet() const {
        int n = 0;
        for (const Achievement& a : achievements) if (a.freigeschaltet) ++n;
        return n;
    }
    int punkte_erreicht() const {
        int n = 0;
        for (const Achievement& a : achievements) if (a.freigeschaltet) n += a.punkte;
        return n;
    }
    int punkte_gesamt() const {
        int n = 0;
        for (const Achievement& a : achievements) n += a.punkte;
        return n;
    }
};

}  // namespace app
