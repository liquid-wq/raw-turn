#include "core/firmware.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "core/bps.h"
#include "core/http.h"
#include "core/i18n.h"
#include "erzeugt_patches.h"
#include "md5.h"

namespace core {

namespace {

// Offizielles Paket v0.9 von der Release-Seite des SAROO-Projekts.
const char* const kPaketHost = "github.com";
const char* const kPaketPfad = "/tpunix/SAROO/releases/download/v0.9/firm_v0.9.rar";
const char* const kPaketMd5 = "0584a2c2a09d69e5cc9f474327c31b83";

// Sicherung der Dateien, die vor dem ersten Patchen auf der Karte lagen.
const char* const kSicherung = "SAROO\\RAW-TURN_Original";

struct Teil {
    const char* name;
    const char* kartenpfad;    // relativ zur Wurzel
    const char* paketpfad;     // relativ zum entpackten Paket
    const unsigned char* patch;
    size_t patch_laenge;
};

const Teil kTeile[kFwTeile] = {
    {"mcuapp.bin", "SAROO\\mcuapp.bin", "SAROO\\mcuapp.bin",
     patches::kMcuapp, sizeof(patches::kMcuapp)},
    {"ssfirm.bin", "SAROO\\ssfirm.bin", "SAROO\\ssfirm.bin",
     patches::kSsfirm, sizeof(patches::kSsfirm)},
};

std::string Pfad(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    const char z = a.back();
    return (z == '\\' || z == '/') ? a + b : a + "\\" + b;
}

bool Lesen(const std::string& pfad, std::vector<uint8_t>* aus) {
    FILE* f = std::fopen(pfad.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0) { std::fclose(f); return false; }
    aus->resize((size_t)n);
    const bool ok = n == 0 || std::fread(aus->data(), 1, (size_t)n, f) == (size_t)n;
    std::fclose(f);
    return ok;
}

// Erst in eine Nebendatei schreiben, dann ersetzen: Eine halb geschriebene
// Firmware-Datei auf der Karte waere schlimmer als gar keine Aenderung.
bool Schreiben(const std::string& pfad, const std::vector<uint8_t>& daten) {
    const std::string neu = pfad + ".neu";
    FILE* f = std::fopen(neu.c_str(), "wb");
    if (!f) return false;
    const bool ok = daten.empty() || std::fwrite(daten.data(), 1, daten.size(), f) == daten.size();
    if (std::fclose(f) != 0 || !ok) { ::DeleteFileA(neu.c_str()); return false; }
    if (!::MoveFileExA(neu.c_str(), pfad.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ::DeleteFileA(neu.c_str());
        return false;
    }
    return true;
}

bool Existiert(const std::string& pfad) {
    const DWORD a = ::GetFileAttributesA(pfad.c_str());
    return a != INVALID_FILE_ATTRIBUTES;
}

bool IstOrdner(const std::string& pfad) {
    const DWORD a = ::GetFileAttributesA(pfad.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

void OrdnerAnlegen(const std::string& pfad) {
    // Schrittweise, CreateDirectory legt nur eine Ebene an.
    for (size_t i = 3; i <= pfad.size(); ++i) {
        if (i == pfad.size() || pfad[i] == '\\')
            ::CreateDirectoryA(pfad.substr(0, i).c_str(), nullptr);
    }
}

uint32_t Crc(const std::vector<uint8_t>& d) { return Crc32(d.data(), d.size()); }

FwLage Lage(const Teil& t, const std::vector<uint8_t>& d) {
    uint32_t cq = 0, cz = 0;
    BpsPruefsummen(t.patch, t.patch_laenge, &cq, &cz);
    const uint32_t c = Crc(d);
    if (c == cq) return FwLage::Original;
    if (c == cz) return FwLage::RawTurn;
    return FwLage::Anders;
}

std::string Md5Hex(const std::string& d) {
    md5_state_t st;
    md5_byte_t digest[16];
    md5_init(&st);
    md5_append(&st, (const md5_byte_t*)d.data(), (int)d.size());
    md5_finish(&st, digest);
    char hex[33];
    for (int i = 0; i < 16; ++i) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return hex;
}

// Laedt und entpackt das offizielle Paket in einen Ordner unter %TEMP%.
// Entpackt wird mit dem tar, das Windows 10 und 11 mitbringen; es liest auch
// RAR. Einmal geholt, bleibt das Paket fuer weitere Laeufe liegen.
bool PaketHolen(std::string* ordner, std::vector<std::string>* log, std::string* fehler) {
    char tmp[MAX_PATH];
    if (!::GetTempPathA(MAX_PATH, tmp)) { *fehler = T("Kein Ordner für Zwischendateien."); return false; }
    const std::string basis = Pfad(tmp, "raw-turn_saroo_v0.9");
    *ordner = basis;
    if (Existiert(Pfad(basis, kTeile[0].paketpfad)) && Existiert(Pfad(basis, kTeile[1].paketpfad)))
        return true;

    log->push_back(T("Lade die offizielle SAROO-Firmware v0.9 von der Seite des SAROO-Projekts…"));
    const HttpAntwort a = HttpGet(kPaketHost, kPaketPfad, 60000);
    if (!a.ok || a.status != 200) {
        *fehler = std::string(T("Herunterladen fehlgeschlagen: ")) +
                  (a.fehler.empty() ? std::to_string(a.status) : a.fehler);
        return false;
    }
    if (Md5Hex(a.koerper) != kPaketMd5) {
        *fehler = T("Das heruntergeladene Paket ist nicht die offizielle v0.9.");
        return false;
    }
    OrdnerAnlegen(basis);
    const std::string rar = Pfad(basis, "firm_v0.9.rar");
    if (!Schreiben(rar, std::vector<uint8_t>(a.koerper.begin(), a.koerper.end()))) {
        *fehler = T("Paket ließ sich nicht zwischenspeichern.");
        return false;
    }

    char sys[MAX_PATH];
    ::GetSystemDirectoryA(sys, MAX_PATH);
    std::string befehl = "\"" + Pfad(sys, "tar.exe") + "\" -xf \"" + rar + "\" -C \"" + basis + "\"";
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::vector<char> zeile(befehl.begin(), befehl.end());
    zeile.push_back('\0');
    if (!::CreateProcessA(nullptr, zeile.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                          nullptr, nullptr, &si, &pi)) {
        *fehler = T("Entpacken nicht möglich: tar.exe fehlt.");
        return false;
    }
    ::WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    for (const Teil& t : kTeile) {
        if (!Existiert(Pfad(basis, t.paketpfad))) {
            *fehler = T("Das Paket ließ sich nicht entpacken.");
            return false;
        }
    }
    return true;
}

// Die offizielle v0.9 eines Teils: von der Karte, aus der Sicherung oder aus
// dem Paket, in dieser Reihenfolge.
bool OriginalHolen(const std::string& wurzel, const Teil& t, std::string* paket,
                   std::vector<uint8_t>* aus, std::vector<std::string>* log,
                   std::string* fehler) {
    const std::string orte[2] = {Pfad(wurzel, t.kartenpfad),
                                 Pfad(Pfad(wurzel, kSicherung), t.name)};
    for (const std::string& o : orte) {
        std::vector<uint8_t> d;
        if (Lesen(o, &d) && Lage(t, d) == FwLage::Original) { *aus = std::move(d); return true; }
    }
    if (paket->empty() && !PaketHolen(paket, log, fehler)) return false;
    std::vector<uint8_t> d;
    if (!Lesen(Pfad(*paket, t.paketpfad), &d) || Lage(t, d) != FwLage::Original) {
        *fehler = std::string(T("Im Paket fehlt: ")) + t.name;
        return false;
    }
    *aus = std::move(d);
    return true;
}

}  // namespace

std::string SdWurzelFinden(const std::string& pfad) {
    std::string d = pfad;
    while (!d.empty() && (d.back() == '\\' || d.back() == '/')) d.pop_back();
    // Eine Datei wie saroocfg.txt: mit ihrem Ordner beginnen.
    if (!d.empty() && Existiert(d) && !IstOrdner(d)) {
        const size_t s = d.find_last_of("\\/");
        d = s == std::string::npos ? "" : d.substr(0, s);
    }
    for (int stufe = 0; stufe < 6 && !d.empty(); ++stufe) {
        const std::string w = d.size() == 2 && d[1] == ':' ? d + "\\" : d;
        if (IstOrdner(Pfad(w, "SAROO"))) return w;
        const size_t s = d.find_last_of("\\/");
        if (s == std::string::npos) break;
        d.resize(s);
    }
    return "";
}

FwStand FirmwarePruefen(const std::string& wurzel) {
    FwStand st;
    for (int i = 0; i < kFwTeile; ++i) st.teile[i].name = kTeile[i].name;
    if (wurzel.empty() || !IstOrdner(Pfad(wurzel, "SAROO"))) return st;
    st.karte = true;
    for (int i = 0; i < kFwTeile; ++i) {
        std::vector<uint8_t> d;
        if (Lesen(Pfad(wurzel, kTeile[i].kartenpfad), &d)) st.teile[i].lage = Lage(kTeile[i], d);
        st.teile[i].gesichert = Existiert(Pfad(Pfad(wurzel, kSicherung), kTeile[i].name));
    }
    return st;
}

bool FirmwarePatchen(const std::string& wurzel, std::vector<std::string>* log,
                     std::string* fehler) {
    if (!IstOrdner(Pfad(wurzel, "SAROO"))) {
        *fehler = T("Auf diesem Laufwerk gibt es keinen Ordner SAROO.");
        return false;
    }

    // Erst alles vorbereiten, dann schreiben: Scheitert ein Teil, bleibt die
    // Karte unberuehrt.
    std::string paket;
    std::vector<uint8_t> ergebnis[kFwTeile];
    for (int i = 0; i < kFwTeile; ++i) {
        std::vector<uint8_t> original;
        if (!OriginalHolen(wurzel, kTeile[i], &paket, &original, log, fehler)) return false;
        std::string f;
        if (!BpsAnwenden(kTeile[i].patch, kTeile[i].patch_laenge, original, &ergebnis[i], &f)) {
            *fehler = std::string(kTeile[i].name) + ": " + T(f.c_str());
            return false;
        }
    }

    // Sichern, was vorher auf der Karte lag, aber nur beim ersten Mal und nie
    // unsere eigene Datei.
    const std::string sicherung = Pfad(wurzel, kSicherung);
    for (const Teil& t : kTeile) {
        std::vector<uint8_t> d;
        const std::string ziel = Pfad(sicherung, t.name);
        if (!Lesen(Pfad(wurzel, t.kartenpfad), &d) || Lage(t, d) == FwLage::RawTurn ||
            Existiert(ziel))
            continue;
        OrdnerAnlegen(sicherung);
        if (!Schreiben(ziel, d)) {
            *fehler = std::string(T("Sicherung fehlgeschlagen: ")) + t.name;
            return false;
        }
        log->push_back(std::string(T("Gesichert: ")) + t.kartenpfad);
    }

    for (int i = 0; i < kFwTeile; ++i) {
        if (!Schreiben(Pfad(wurzel, kTeile[i].kartenpfad), ergebnis[i])) {
            *fehler = std::string(T("Schreiben fehlgeschlagen: ")) + kTeile[i].kartenpfad;
            return false;
        }
        log->push_back(std::string(T("Gepatcht: ")) + kTeile[i].kartenpfad);
    }
    return true;
}

bool FirmwareZurueck(const std::string& wurzel, std::vector<std::string>* log,
                     std::string* fehler) {
    if (!IstOrdner(Pfad(wurzel, "SAROO"))) {
        *fehler = T("Auf diesem Laufwerk gibt es keinen Ordner SAROO.");
        return false;
    }
    std::string paket;
    std::vector<uint8_t> daten[kFwTeile];
    bool aus_sicherung[kFwTeile] = {};
    for (int i = 0; i < kFwTeile; ++i) {
        if (Lesen(Pfad(Pfad(wurzel, kSicherung), kTeile[i].name), &daten[i])) {
            aus_sicherung[i] = true;
            continue;
        }
        if (!OriginalHolen(wurzel, kTeile[i], &paket, &daten[i], log, fehler)) return false;
    }
    for (int i = 0; i < kFwTeile; ++i) {
        if (!Schreiben(Pfad(wurzel, kTeile[i].kartenpfad), daten[i])) {
            *fehler = std::string(T("Schreiben fehlgeschlagen: ")) + kTeile[i].kartenpfad;
            return false;
        }
        log->push_back(std::string(aus_sicherung[i] ? T("Aus der Sicherung zurück: ")
                                                    : T("Offizielle v0.9 geschrieben: ")) +
                       kTeile[i].kartenpfad);
    }
    return true;
}

}  // namespace core
