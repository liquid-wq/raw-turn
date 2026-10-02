// Firmware der SAROO patchen und zurueckstellen.
//
// Das Werkzeug liefert keine SAROO-Datei aus, nur zwei BPS-Patches. Grundlage
// ist immer die offizielle Firmware v0.9 von tpunix: Liegt sie schon auf der
// Karte, wird sie dort genommen, sonst laedt das Werkzeug das offizielle Paket
// von der Release-Seite des SAROO-Projekts und prueft seine Pruefsumme.
//
// Gepatcht werden die beiden Dateien, die die SAROO bei jedem Start von der
// Karte laedt. Der FPGA bleibt der offizielle, ein Firmware Update im
// SAROO-Menue ist nicht noetig:
//   SAROO\mcuapp.bin   STM32
//   SAROO\ssfirm.bin   Saturn
#pragma once

#include <string>
#include <vector>

namespace core {

constexpr int kFwTeile = 2;

enum class FwLage {
    Fehlt,       // nicht auf der Karte
    Original,    // offizielle v0.9
    RawTurn,     // unser Ergebnis
    Anders,      // eine andere Fassung
};

struct FwTeilStand {
    const char* name;      // "mcuapp.bin"
    FwLage lage = FwLage::Fehlt;
    bool gesichert = false;
};

struct FwStand {
    bool karte = false;     // Ordner SAROO gefunden
    FwTeilStand teile[kFwTeile];
};

// Ordner, der SAROO enthaelt, ausgehend von einem beliebigen Pfad auf der
// Karte (Laufwerk, ROM-Ordner, Pfad der saroocfg.txt). Leer, wenn keiner.
std::string SdWurzelFinden(const std::string& pfad);

FwStand FirmwarePruefen(const std::string& wurzel);

// Sichert die vorhandenen Dateien, wendet die Patches an und schreibt die
// Ergebnisse. Meldungen landen in `log`, schon uebersetzt.
bool FirmwarePatchen(const std::string& wurzel, std::vector<std::string>* log,
                     std::string* fehler);

// Schreibt die Sicherung zurueck, sonst die offizielle v0.9.
bool FirmwareZurueck(const std::string& wurzel, std::vector<std::string>* log,
                     std::string* fehler);

}  // namespace core
