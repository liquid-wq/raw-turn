// Hook-Installation fuer einen kompletten ROM-Ordner.
//
// Der Hook muss installiert sein, BEVOR ein Spiel startet: Die SAROO fuehrt
// die M_-Direktiven als Teil ihrer eigenen Boot-Sequenz aus, und dieses
// Zeitfenster ist vorbei, sobald das Spiel laeuft. Eine Automatik beim
// Spielstart kaeme grundsaetzlich zu spaet.
//
// Deshalb wird im Voraus eingerichtet. Das geht ohne Konsole, weil sowohl
// RA-Pruefsumme als auch Abschnittskennung in denselben 512 Kopf-Bytes des
// Disc-Abbilds stehen.
//
// WICHTIG: Ein Achievement-Set ist KEINE Voraussetzung. Es bestimmt nur,
// welche Adressen beobachtet werden. Hook, Mailbox und Selbstauskunft haengen
// nicht daran -- auch ein bei RA unbekanntes Spiel meldet sich damit im
// Betrieb selbst.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/game_index.h"
#include "core/ra.h"

namespace core {

// Felder des Saturn-Disc-Kopfes. Layout gegen yaul-org/libyaul geprueft,
// an echten Abbildern nachgemessen.
struct DiscKopf {
    std::string produktnummer;   // 0x20, 10 Byte
    std::string version;         // 0x2A, 6 Byte
    std::string kennung;         // beide zusammen, 16 Byte am Stueck
    std::string titel;           // 0x60, 112 Byte
    std::string hersteller;      // 0x10, 16 Byte
    std::string datum;           // 0x30, 8 Byte
    std::string gebiete;         // 0x40, ausgeschrieben
};

// `rohkopf` liefert auf Wunsch die 512 Byte selbst zurueck -- genau die Bytes,
// ueber die RetroAchievements hasht, und genau die, die die Firmware im Betrieb
// als GAME_INFO sendet. So laesst sich beides gegeneinander pruefen.
bool LiesDiscKopf(const std::string& pfad, DiscKopf* aus, std::string* fehler,
                  std::string* rohkopf = nullptr);

// Dieselbe Deutung, aber auf 512 Byte, die schon vorliegen -- der Weg fuer den
// Betrieb: Die Firmware schickt den Kopf des laufenden Spiels, hier entstehen
// daraus Titel, Kennung und Gebiet. Prueft „SEGA SEGASATURN " als Pruefstein
// und meldet false, wenn der fehlt.
bool DeuteDiscKopf(const uint8_t* roh512, DiscKopf* aus);

// RA-Pruefsumme aus 512 Byte Disc-Kopf: MD5 ueber genau diese Bytes. Damit
// rechnet der PC die Pruefsumme des laufenden Spiels selbst, ohne Abbilddatei.
// Bitgenau dasselbe Ergebnis wie SaturnHash() auf der Datei.
std::string HashAusKopf(const uint8_t* roh512);

// Disc-Abbilder finden, ohne dasselbe Spiel doppelt zu zaehlen: Liegt in einem
// Verzeichnis eine .cue, sind die .bin daneben ihre Spuren.
std::vector<std::string> FindeAbbilder(const std::string& ordner, bool rekursiv);

// Alle 4-Byte-ausgerichteten Watch-Adressen aus einem Achievement-Satz.
//
// Ermittelt ueber die rcheevos-Laufzeit, nicht ueber eigenes Zerlegen der
// Bedingungstexte. Die Ausrichtung ist Pflicht: Der Hook liest jeden Eintrag
// als ganzes 32-Bit-Wort, eine unausgerichtete Adresse waere auf dem SH-2
// kein falscher Wert, sondern ein Busfehler.
std::vector<uint32_t> WatchAdressen(const std::vector<RaAchievement>& achievements,
                                    int* nicht_abbildbar);

struct SetupErgebnis {
    std::string abbild;
    std::string kennung;
    std::string name;          // RA-Klarname, sonst Kopf-Titel
    DiscKopf kopf;
    std::string md5;
    uint32_t gameid = 0;
    int achievements = 0;
    // Wie viele Achievements RetroAchievements geliefert hat, die NICHT zum
    // Kernsatz gehoeren. Ohne diese Zahl sieht ein Spiel ganz ohne Set genauso
    // aus wie eines, dessen Set noch nicht offiziell freigegeben ist.
    int inoffiziell = 0;
    int watch = 0;
    int paare = 0;
    bool installierbar = false;
    std::string status;
    std::string hinweis;
    std::vector<std::string> zeilen;
};

// Ein Abbild verarbeiten. `client` darf nicht angemeldet sein -- dann entfaellt
// der RA-Teil und es wird trotzdem ein Hook erzeugt.
SetupErgebnis VerarbeiteAbbild(const std::string& pfad, RaClient& client,
                               bool nur_mit_set);

// Hook-Zeilen fuer ein Spiel, das GERADE LAEUFT -- ganz ohne Abbilddatei.
//
// Kennung, Name und Prüfsumme hat die Konsole selbst gemeldet, den
// Achievement-Satz liefert RetroAchievements dazu. Damit lässt sich der Hook
// nachtragen, während das Spiel läuft; wirksam wird er beim nächsten Start
// dieses Spiels, weil die SAROO die M_-Zeilen nur beim Laden ausführt.
// `sperre`: SAVE und LOAD im Ingame-Menü sperren (Hardcore).
SetupErgebnis HookFuerSpiel(const std::string& kennung, const std::string& name,
                            const std::string& md5, uint32_t gameid,
                            const std::vector<RaAchievement>& achievements,
                            bool sperre);

// Inhalt der Hook-Datei, die die Kartusche selbst ablegt. Vor den M_-Zeilen
// steht "; <Spieltitel>": Die Firmware ueberspringt die Zeile, das
// RetroAchievements-Menue der SAROO zeigt damit den Namen statt nur der
// Kennung. Der Titel ist auf 60 Byte gekuerzt, ohne ein UTF-8-Zeichen zu
// zerschneiden.
std::string HookDateiInhalt(const SetupErgebnis& e);

// Zwei Abschnittskennungen so vergleichen, wie gameid_match() in der Firmware
// es tut: an den Leerzeichen zerlegt, Produktnummer und Version einzeln.
// Oeffentlich, weil genau daran haengt, ob ein nachgetragener Hook im
// richtigen Abschnitt landet.
bool KennungGleich(const std::string& a, const std::string& b);

// Ist die saroocfg.txt gerade erreichbar? Beim Spielen meist NICHT, weil die
// SD-Karte dann in der SAROO steckt und das Laufwerk am PC nicht existiert.
bool CfgErreichbar(const std::string& pfad);

// Klartext, warum sie es nicht ist. Unterscheidet „Karte steckt in der SAROO"
// von „falscher Pfad" und von „Zugriff verweigert".
std::string CfgGrund(const std::string& pfad);

// saroocfg.txt ergaenzen. Bestehende Abschnitte und fremde Zeilen bleiben
// erhalten, eigene werden vorher entfernt -- der Lauf ist wiederholbar.
// Legt vorher eine datierte Sicherung an.
bool SchreibeSaroocfg(const std::string& pfad,
                      const std::vector<SetupErgebnis>& fertige,
                      std::string* sicherungspfad, std::string* fehler);

}  // namespace core
