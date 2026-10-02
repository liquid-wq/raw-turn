// UART-Binaerprotokoll Saturn-Hook <-> SAROO-Firmware <-> PC.
//
// Format: Magic, TYPE, SEQ, LEN, Nutzdaten, CRC-8. Big-Endian auf dem Draht.
//
// KEINE Oberflaechen-Bibliothek. Diese Schicht laesst sich unveraendert auf
// anderen Zielen uebersetzen -- dieselbe Regel, die sich in mega-raw beim
// Sprung auf den ESP32 ausgezahlt hat.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace core {

constexpr uint8_t kMagic1 = 0xA5;
constexpr uint8_t kMagic2 = 0x5A;
constexpr int kMaxPayload = 250;
constexpr int kHeaderLen  = 5;   // MAGIC1, MAGIC2, TYPE, SEQ, LEN
constexpr int kCrcLen     = 1;

// Version 2: WATCH_REPORT traegt den FRAME_COUNT der Mailbox im Kopf.
constexpr uint8_t kProtocolVersion = 2;

// 4 Byte FRAME_COUNT am Anfang jedes WATCH_REPORT-Payloads.
//
// Warum noetig: SEQ erzeugt die STM32 als Absender und zaehlt weiter, auch
// wenn der Hook auf der Saturn laengst steht. Nur ein vom Hook selbst
// gepflegter Zaehler macht einen toten Hook von einem unveraenderten
// Spielzustand unterscheidbar.
constexpr int kWatchReportHeaderLen = 4;

constexpr uint8_t kTypeWatchReport = 0x01;
constexpr uint8_t kTypeHeartbeat   = 0x02;
constexpr uint8_t kTypeHello       = 0x03;

// GAME_INFO: die 512 Byte Disc-Kopf des laufenden Spiels.
//
// Sie kommen NICHT ueber die Mailbox und haengen an keinem Hook. Die Firmware
// liest den Kopf beim Laden des Abbilds ohnehin von der SD-Karte, um die
// Spielkennung daraus zu ziehen, und schickt ihn seither einfach mit. Deshalb
// braucht die Erkennung keine Voraberrichtung: kein M_-Eintrag, keine
// Abbilddatei, keine Nachschlagetabelle. Aus diesen Bytes rechnet der PC Titel
// UND RA-Pruefsumme selbst, denn die RA-Pruefsumme fuer Saturn ist MD5 ueber
// genau diese 512 Byte.
//
// LEN ist ein Byte, also hoechstens 250 Nutzbytes je Frame. Der Kopf kommt
// darum in vier Teilen: Payload = OFFSET (2 Byte, big-endian) + 128 Datenbytes.
constexpr uint8_t kTypeGameInfo    = 0x04;

// Hook-Satz zum Gerät schicken, damit die SAROO ihn selbst auf ihre SD-Karte
// schreibt.
//
// Warum überhaupt: Beim Spielen steckt die Karte in der SAROO, der PC kommt
// nicht heran. Die Firmware legt den Satz in `/SAROO/rahook_<Kennung>.txt` ab
// und hängt seine M_-Zeilen beim nächsten Start dieses Spiels an den
// Konfigurationspuffer an. saroocfg.txt bleibt unangetastet.
//
// Ablauf: BEGIN (Kennung + Gesamtlänge), beliebig viele DATA (Offset + Bytes),
// END (Länge + Bytesumme). Das Gerät antwortet auf BEGIN und END mit ACK.
constexpr uint8_t kTypeHookBegin   = 0x10;
constexpr uint8_t kTypeHookData    = 0x11;
constexpr uint8_t kTypeHookEnd     = 0x12;
constexpr uint8_t kTypeHookAck     = 0x13;

// Goldblitz am Fernseher bei einer Freischaltung. Derzeit nicht benutzt: Die
// Firmware dieser Fassung wertet den Rahmen nicht aus.
//
// Payload: ein Farbwort, 2 Byte big-endian. Die STM32 legt es in die Mailbox
// (+0x0E), die Saturn-Firmware legt die Farbe bei jedem Bild als
// Farbverschiebung auf den Back Screen. 0xA000 | 0x0BGR heisst an, 0 aus.
// Die Folge erzeugt der PC, wie in MEGA-RAW: sechs Stufen hoch, zehn halten,
// sieben zurueck, dann 0; je Schritt kGoldblitzSchrittMs.
constexpr uint8_t kTypeTvBlitz     = 0x14;
constexpr int kGoldblitzSchrittMs  = 45;
std::vector<uint16_t> GoldblitzFolge();
std::vector<uint8_t> BaueTvBlitz(uint16_t wert, uint8_t seq = 0);

// Beliebige Datei auf die SD-Karte der SAROO schreiben, waehrend sie im Geraet
// steckt. Damit lassen sich auch ssfirm.bin und mcuapp.bin aufspielen, ohne die
// Karte herauszunehmen.
//
// Ablauf: BEGIN (Gesamtlaenge 4 Byte + Zielpfad als Text), dann Bloecke aus je
// 16 DATA-Rahmen mit reinen Nutzdaten, zuletzt END (Laenge + Bytesumme, je
// 4 Byte). Das Geraet quittiert das Oeffnen, jeden vollen Block und den
// Abschluss mit demselben ACK-Rahmen wie beim Hook-Satz.
//
// Die Quittung ist die Bremse: Ohne sie ueberholt die Leitung bei einem Megabit
// die SD-Karte. Erst nach der Quittung geht der naechste Block raus.
//
// Die Folgenummern muessen innerhalb eines Transfers luecklenlos aufsteigen; das
// Geraet bricht sonst ab. Ein abgebrochener Transfer laesst die Zieldatei
// unberuehrt, geschrieben wird geraeteseitig zuerst nach <pfad>.$$$.
constexpr uint8_t kTypeFileBegin   = 0x15;
constexpr uint8_t kTypeFileData    = 0x16;
constexpr uint8_t kTypeFileEnd     = 0x17;

// Muessen zu RA_FILE_BLOCK und zur Rahmengroesse in ra_mailbox.c passen.
constexpr int kFileBlockBytes      = 4000;
constexpr int kFileRahmenNutz      = 250;

std::vector<uint8_t> BaueFileBegin(const std::string& pfad, uint32_t laenge,
                                   uint8_t seq);
std::vector<uint8_t> BaueFileData(const uint8_t* daten, int len, uint8_t seq);
std::vector<uint8_t> BaueFileEnd(uint32_t laenge, uint32_t summe, uint8_t seq);

// Speicherstaende. PC -> Geraet: Vorgang und Slot. Geraet -> PC: Antwort der
// Saturn samt Uebersicht aller Slots, sobald ein Vorgang fertig ist -- auch
// nach Schnellspeichern mit L + R + Unten, das ohne Auftrag vom PC ausgeloest
// wird. Gegenstueck: TYPE_STAND in ra_mailbox.c, RA_AUFTRAG_WORT in
// ra_ingame.c.
constexpr uint8_t kTypeStand       = 0x18;
constexpr uint8_t kTypeStandAntw   = 0x19;
constexpr uint8_t kStandSpeichern  = 1;
constexpr uint8_t kStandLaden      = 2;
constexpr uint8_t kStandSlotsLesen = 3;
constexpr uint8_t kStandMenueNicht = 4;   // nur Antwort: Start + Unten, Menue hier nicht sicher
constexpr uint8_t kStandErgOk      = 1;
constexpr uint8_t kStandErgFehler  = 2;
constexpr uint8_t kStandErgLeer    = 3;
constexpr uint8_t kStandErgGesperrt = 4;
// Slots 0..3 werden im Werkzeug gewaehlt. 4..13 sind die fortlaufenden
// Schnellstaende "X.n" (L + R + Unten), ein Ring aus zehn Plaetzen; die Nummer
// n steht im Eintrag. kStandXNeu als Slot heisst "naechster Schnellstand".
constexpr int kStandSlots          = 14;
constexpr int kStandSlotX0         = 4;
constexpr uint8_t kStandXNeu       = 0xFF;
constexpr int kStandEintrag        = 16;    // Bytes je Slot in der Antwort

struct StandSlot {
    bool belegt = false;
    uint8_t zeit[7] = {};      // BCD: Jh, Jahr, Wochentag<<4|Monat, Tag, Std, Min, Sek
    int folge = 0;             // Schnellstand: fortlaufende Nummer
    uint32_t bilder = 0;       // Spielzeit in Bildern seit dem Spielstart
    bool pal = false;          // 50 statt 60 Bilder je Sekunde
};

// Uhrzeit des PC an das Geraet, 7 Byte BCD wie oben. Die Saturn nimmt sie fuer
// Datum und Uhrzeit eines Speicherstands, weil die Konsolenuhr oft nicht
// gelesen werden kann. Gegenstueck: TYPE_UHR in ra_mailbox.c.
constexpr uint8_t kTypeUhr         = 0x1A;
std::vector<uint8_t> BaueUhrJetzt(uint8_t seq = 0);
// Aenderungsprotokoll. Die Saturn legt in
// jedem Bild jede geaenderte Watch-Adresse ab, die STM32 reicht die
// Datensaetze unveraendert weiter. Nutzdaten: Merker (Bit 0 = davor fehlen
// Datensaetze), dann je 8 Byte Kopf und Wert, big-endian. Kopf = Eintrag<<16 |
// Bild (untere 16 Bit); Eintrag 0xFFFF schliesst ein Bild ab, Wert = volle
// Bildnummer. Gegenstueck: TYPE_PROT in ra_mailbox.c, RA_PROT_SDRAM in
// ra_ingame.c.
constexpr uint8_t kTypeProt        = 0x1B;
// Dauer der Erfassung auf der Saturn, letzte und groesste, je 4 Byte in us.
constexpr uint8_t kTypeProtZeit    = 0x1C;
constexpr uint16_t kProtBildEnde   = 0xFFFF;
struct ProtSatz {
    uint16_t eintrag = 0;
    uint16_t bild = 0;
    uint32_t wert = 0;
};
bool LiesProt(const struct Frame& f, bool* luecke, std::vector<ProtSatz>* aus);
bool LiesProtZeit(const struct Frame& f, uint32_t* letzte_us, uint32_t* max_us);

struct StandAntwort {
    uint8_t vorgang = 0;
    uint8_t ergebnis = 0;
    StandSlot slots[kStandSlots];
};
std::vector<uint8_t> BaueStand(uint8_t vorgang, uint8_t slot, uint8_t seq = 0);
bool LiesStandAntwort(const struct Frame& f, StandAntwort* aus);

// Klartext zu einem Ablehnungsgrund aus dem ACK. Deckt beide Richtungen ab,
// Hook-Satz wie Datei.
const char* AckGrundText(uint8_t status);

// Muss zu RA_HOOK_MAX in ra_mailbox.c passen.
constexpr int kHookMaxBytes        = 12288;
constexpr int kDiscKopfLaenge      = 512;
constexpr int kGameInfoTeilLaenge  = 128;

uint8_t Crc8(const uint8_t* data, size_t len);

struct WatchEntry {
    uint32_t addr = 0;
    uint8_t  size = 4;
    uint32_t value = 0;
};

struct Frame {
    uint8_t type = 0;
    uint8_t seq = 0;
    std::vector<uint8_t> payload;

    bool IstWatchReport() const { return type == kTypeWatchReport; }

    // false, wenn der Payload nicht zum Typ passt. Bewusst kein Werfen:
    // beschaedigte Frames sind Normalbetrieb, keine Ausnahme.
    bool LiesWatchReport(uint32_t* frame_count, std::vector<WatchEntry>* out) const;
    bool LiesHello(uint8_t* version, std::string* serial) const;
    // Offset und Datenteil eines GAME_INFO-Frames. false, wenn der Teil nicht
    // vollstaendig in den Disc-Kopf passt.
    bool LiesGameInfo(int* offset, std::vector<uint8_t>* daten) const;
};

// Sammelt die vier Teile zu 512 Byte zusammen. Erst wenn alle da sind, ist
// `Vollstaendig()` wahr -- eine halbe Pruefsumme waere eine falsche.
class DiscKopfSammler {
public:
    // true, wenn sich der Inhalt dadurch geaendert hat.
    bool Fuettern(int offset, const std::vector<uint8_t>& daten);
    bool Vollstaendig() const;
    const uint8_t* Bytes() const { return kopf_; }
    void Zuruecksetzen();

private:
    uint8_t kopf_[kDiscKopfLaenge] = {};
    bool da_[kDiscKopfLaenge / kGameInfoTeilLaenge] = {};
};

// Erzeugen, derzeit nur fuer Selbsttests gebraucht.
std::vector<uint8_t> BaueWatchReport(const std::vector<WatchEntry>& entries,
                                     uint32_t frame_count, uint8_t seq = 0);
// Alle Frames, die einen Hook-Satz zum Gerät bringen, in Sendereihenfolge.
// `kennung` sind die 16 Byte Produktnummer und Version, `inhalt` die fertigen
// M_-Zeilen als Text. Leere Liste, wenn der Satz zu groß ist.
std::vector<std::vector<uint8_t>> BaueHookUebertragung(const std::string& kennung,
                                                       const std::string& inhalt);

// Antwort des Geräts: auf welchen Frame, und mit welchem Ergebnis.
// status 0 = angenommen.
bool LiesHookAck(const Frame& f, uint8_t* auf_typ, uint8_t* status);

// Die vier GAME_INFO-Frames zu einem Disc-Kopf, so wie die Firmware sie sendet.
std::vector<std::vector<uint8_t>> BaueGameInfo(const uint8_t* kopf512, uint8_t seq = 0);
std::vector<uint8_t> BaueHeartbeat(uint8_t seq = 0);
std::vector<uint8_t> BaueHello(const std::string& serial,
                               uint8_t version = kProtocolVersion, uint8_t seq = 0);

// Zustandsbehafteter Empfaenger mit Resync. Beschaedigte Frames werden
// verworfen und gezaehlt, nicht gemeldet.
class FrameParser {
public:
    // Haengt Daten an und gibt alle darin vollstaendig erkannten, CRC-gueltigen
    // Frames zurueck.
    std::vector<Frame> Fuettern(const uint8_t* data, size_t len);

    long frames_ok = 0;
    long frames_crc_fehler = 0;
    long resyncs = 0;

    // Diagnose: die ersten Bytes jedes Rahmens mit falscher Pruefsumme, als
    // Hex und lesbar. Zeigt, ob Klartext hineingeraten ist, ob Bytes aus einem
    // aelteren Rahmen stammen oder ob einzelne Bits kippen. Der Aufrufer leert
    // die Liste.
    std::vector<std::string> crc_proben;

    // Alles, was nicht zu einem Rahmen gehoert: die Textausgabe der SAROO
    // (printk der STM32). Der Aufrufer leert es.
    std::string klartext;

private:
    std::vector<uint8_t> puffer_;
    // Index des naechsten Magic-Paares, oder -1.
    int FindeMagic() const;
};

}  // namespace core
