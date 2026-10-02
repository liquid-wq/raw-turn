// BPS-Patches anwenden.
//
// BPS ist das in der ROM-Hack-Szene uebliche Format. Ein Patch traegt die
// CRC32 des Originals, des Ergebnisses und seiner selbst. Er laesst sich nur
// auf genau das Original anwenden, fuer das er erzeugt wurde, und ein falsches
// Ergebnis faellt an der Pruefsumme auf.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace core {

uint32_t Crc32(const uint8_t* daten, size_t laenge);

// Pruefsummen aus dem Fuss eines Patches. false bei verstuemmeltem Patch.
bool BpsPruefsummen(const uint8_t* patch, size_t laenge,
                    uint32_t* crc_quelle, uint32_t* crc_ziel);

// Wendet `patch` auf `quelle` an. Prueft alle drei Pruefsummen.
bool BpsAnwenden(const uint8_t* patch, size_t patch_laenge,
                 const std::vector<uint8_t>& quelle,
                 std::vector<uint8_t>* ziel, std::string* fehler);

}  // namespace core
