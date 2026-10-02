// Mailbox-Aufbau und die M_-Zeilen des Hook-Satzes.
//
// Die Mailbox traegt ausschliesslich Watch-Werte fuer Achievements. Welches
// Spiel laeuft, meldet die Firmware getrennt davon: Sie liest den Disc-Kopf
// beim Laden des Abbilds und schickt ihn als GAME_INFO-Rahmen (siehe
// core/protocol.h). Die Erkennung haengt damit an keinem Hook.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/protocol.h"

namespace core {

constexpr uint32_t kMailboxBasis   = 0x023F0000u;   // Saturn-Sicht, gecacht
constexpr uint32_t kHookBasis      = 0x023F1000u;
constexpr uint32_t kVektorAdresse  = 0x06000104u;   // V-Blank-OUT, VBR 0x06000000
constexpr uint32_t kBiosHandler    = 0x06000846u;
constexpr uint32_t kUngecacht      = 0x20000000u;   // 0x023f.. -> 0x223f..

constexpr uint32_t kMagicWort      = 0x52415341u;   // "RASA"
constexpr int kMagicOffset         = 0x00;
constexpr int kEintragZaehlOffset  = 0x0C;
constexpr int kEintraegeOffset     = 0x10;
/* Spielkennung in der Mailbox, 16 Byte ab +0xFF0.
 *
 * Der Inhalt der Mailbox liegt im SDRAM der SAROO und uebersteht Reset und
 * Spielwechsel. Mit der Spielkennung daneben verwirft die Saturn-Firmware den
 * Satz eines anderen Spiels von selbst. +0xFF0 liegt hinter den Eintraegen
 * und direkt vor dem Hook-Bereich bei +0x1000. */
constexpr int kKennungOffset       = 0xFF0;
constexpr int kKennungLaenge       = 16;
constexpr int kEintragBreite       = 12;            // ADDR + SIZE/PAD + VALUE
constexpr int kCodeOffset          = 0x1000;

// Statuswort, Gegenstueck in ra_ingame.c (RA_STATUS_WORT).
//
// Das Wort +0x04 nutzen weder der Hook noch die STM32. Der Hook-Satz schreibt
// hinein, ob Speichern und Laden gesperrt sein sollen. Die Saturn-Firmware
// ersetzt das beim Spielstart durch ihren Status, und derselbe Hook-Satz
// beobachtet das Wort wie eine Watch-Adresse. Steht dort noch der Wunsch,
// hat die Firmware ihn nicht verstanden.
constexpr int kStatusOffset          = 0x04;
constexpr uint32_t kWunschSperre     = 0x52480001u;   // "RH", 1
constexpr uint32_t kWunschFrei       = 0x52480000u;   // "RH", 0
constexpr uint32_t kStatusKennMaske  = 0xFFFF0000u;
constexpr uint32_t kStatusKenn       = 0x52530000u;   // "RS"
constexpr uint32_t kStatusGesperrt   = 0x0001u;
constexpr uint32_t kStatusGeladen    = 0x0002u;       // seit Spielstart ein Stand geladen

// Muss mit RA_MAX_ENTRIES in ra_mailbox.c und RA_MAX_WATCH in ra_ingame.c
// uebereinstimmen. Obergrenzen: Konfigurationspuffer der SAROO (kMaxPaare in
// setup.cpp) und der Platz bis zur Kennung bei +0xFF0 (336 Eintraege).
constexpr int kFirmwareMaxEintraege = 300;

// Schalter zur Fehlersuche, alle standardmaessig aus. Siehe ZeilenBauen() in
// setup.cpp.
//
// g_ohne_vektorzeile: laesst die Vektorzeile aus dem Hook-Satz weg.
extern bool g_ohne_vektorzeile;

// g_hook_im_wram: legt die Hook-Worte in den Arbeitsspeicher der Saturn statt
// in den Kartuschenspeicher (kHookBasisWram). Der Bereich kann vom Spiel
// belegt sein, deshalb aus.
extern bool g_hook_im_wram;
constexpr uint32_t kHookBasisWram  = 0x06003000u;

// Basis, an der die Hook-Worte abgelegt werden -- je nach Schalter.
uint32_t HookBasis();

// Bestandteile des Hook-Satzes. Der Normalfall ist ein Satz aus reinen Daten:
// Die Saturn-Firmware haengt sich selbst in die V-Blank-Kette, Hook-Code und
// Vektorzeile entfallen. Die uebrigen Stufen lassen zur Fehlersuche je einen
// Teil weg.
enum class SatzTeil {
    Vollstaendig = 0,   // Normalfall: nur Daten, kein Code, kein Vektor
    MitHookCode,        // mit Hook-Worten und Vektorzeile
    OhneVektor,         // Code liegt da, die CPU springt nie hin
    ZaehlerNull,        // keine Eintraege, sonst wie der Normalfall
    NurCode,            // Hook-Worte und Vektor; keine Kennung, keine Eintraege
    Leer,               // gar keine M_-Zeile; die Datei wird nur gelesen
};
extern SatzTeil g_satz_teil;

// g_nur_zuhoeren: Das Werkzeug sendet nichts an die SAROO.
extern bool g_nur_zuhoeren;

// g_mailbox_verlegt: Mailbox und Hook-Code an kMailboxBasisAlt. Die Firmware
// sucht die Kennung weiterhin bei 0x023F0000 und liefert dann keine Werte.
extern bool g_mailbox_verlegt;
constexpr uint32_t kMailboxBasisAlt = 0x02380000u;
uint32_t MailboxBasis();

// M_-Zeilen fuer saroocfg.txt.
std::string MagicZeile(uint32_t basis = kMailboxBasis);
std::string SperrZeile(bool sperre, uint32_t basis = kMailboxBasis);
// Wirft nichts: gibt bei Ueberlauf eine leere Liste zurueck und setzt `fehler`.
std::vector<std::string> WatchZeilen(const std::vector<uint32_t>& adressen,
                                     uint32_t basis, std::string* fehler);

}  // namespace core
