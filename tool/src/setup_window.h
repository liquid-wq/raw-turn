// Fenster "SAROO einrichten".
//
// Oben die Firmware der SAROO: patchen und zurueckstellen, siehe
// core/firmware.h. Darunter die Spiele in zwei Schritten: Pruefen liest nur
// und fuellt die Tabelle, Schreiben traegt ein und legt vorher eine Sicherung
// an.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app_state.h"
#include "core/firmware.h"
#include "core/game_index.h"
#include "core/ra.h"
#include "core/setup.h"

class SetupWindow {
public:
    ~SetupWindow();

    void Oeffnen(const std::string& rom_ordner_vorschlag);
    bool Offen() const { return offen_; }

    // Einmal je Bild. Zeichnet und uebertraegt Meldungen in den Zustand.
    void Zeichnen(app::State& state, core::RaClient& client,
                  core::GameIndex& index, float dpi);

    void Beenden();

private:
    void PruefenStarten(core::RaClient& client);
    void SchreibenStarten(core::GameIndex& index);
    void FirmwareZeichnen(float dpi);
    void FirmwareStarten(bool zurueck);

    bool offen_ = false;
    char karte_[512] = "";
    core::FwStand fw_;
    bool fw_fertig_ = false;      // Patchen erfolgreich, Hinweis zum Firmware Update zeigen
    char rom_ordner_[512] = "";
    char cfg_pfad_[512] = "";
    bool rekursiv_ = true;
    bool nur_mit_set_ = false;

    std::thread thread_;
    std::atomic<bool> arbeitet_{false};
    std::atomic<int> fortschritt_{0};
    std::atomic<int> gesamt_{0};
    std::atomic<unsigned long> begonnen_{0};   // GetTickCount beim Start, fuer die Restzeit

    std::mutex sperre_;
    std::vector<core::SetupErgebnis> ergebnisse_;
    std::vector<app::Logzeile> ausgabe_;
    std::string status_;    // leer: Grundhinweis, beim Zeichnen übersetzt
    int ausgewaehlt_ = -1;
    bool geschrieben_ = false;
};
