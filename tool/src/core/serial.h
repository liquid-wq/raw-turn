// Serieller Anschluss und automatische Portsuche.
//
// Anders als bei einem EverDrive ist erfolgreiches Oeffnen kein Filter: An J2
// der SAROO haengt ein gewoehnlicher USB-TTL-Adapter, der laesst sich
// immer oeffnen. Deshalb zwei Stufen: nach USB-Kennung einordnen, dann kurz
// hineinhoeren.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace core {

constexpr int kStandardBaud = 1000000;

struct PortInfo {
    std::string geraet;        // "COM3"
    std::string beschreibung;  // Klartext aus dem Geraetemanager
    std::string adapter;       // "CH340" usw., leer wenn unbekannt
    uint16_t vid = 0;
    uint16_t pid = 0;
    int punkte = 0;            // hoeher = wahrscheinlicher
};

// Alle seriellen Anschluesse, wahrscheinlichster zuerst.
std::vector<PortInfo> OrdnePorts(const std::string& bevorzugt = "");

enum class Sicherheit {
    Nichts,      // still
    Bytes,       // irgendetwas kam an, nicht zuordenbar
    Text,        // SAROO-Klartext erkannt
    Frames,      // gueltige Frames -- sicherster Beleg
    Vermutung,   // nichts gehoert, nur nach USB-Kennung geraten
};

struct Suchergebnis {
    bool gefunden = false;
    PortInfo port;
    Sicherheit sicherheit = Sicherheit::Nichts;
    std::string erlaeuterung;
    std::vector<PortInfo> alle;
};

// Kurz an jedem Anschluss mithoeren. `fortschritt` wird je geprueftem Port
// aufgerufen, darf null sein.
Suchergebnis SuchePort(const std::string& bevorzugt = "",
                       int baud = kStandardBaud,
                       int millisekunden_je_port = 1500,
                       void (*fortschritt)(const char*, void*) = nullptr,
                       void* benutzerdaten = nullptr);

const char* SicherheitText(Sicherheit s);

// Offener Anschluss. Nicht kopierbar.
class SerialPort {
public:
    SerialPort() = default;
    ~SerialPort();
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    bool Oeffnen(const std::string& geraet, int baud, std::string* fehler = nullptr);
    void Schliessen();
    bool Offen() const { return handle_ != nullptr; }

    // Liest, was da ist. Blockiert hoechstens `timeout_ms`.
    // Rueckgabe: gelesene Bytes, oder -1 bei Fehler.
    int Lesen(uint8_t* puffer, int max_bytes, int timeout_ms = 100);

    // Alles oder nichts: true erst, wenn alle Bytes draussen sind.
    bool Schreiben(const uint8_t* daten, int anzahl);

private:
    void* handle_ = nullptr;   // HANDLE, ohne windows.h im Kopf
};

}  // namespace core
