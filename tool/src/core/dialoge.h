// Windows-eigene Auswahldialoge. Kein Fremdcode, keine Bibliothek.
#pragma once

#include <string>

namespace core {

// Leerer String, wenn abgebrochen wurde.
std::string OrdnerWaehlen(const char* titel, const std::string& start = "");
std::string DateiWaehlen(const char* titel, const char* filter,
                         const std::string& start = "");

}  // namespace core
