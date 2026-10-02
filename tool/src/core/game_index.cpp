#include "core/game_index.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace core {
namespace {

// -------------------------------------------------------------------------
// Kleiner JSON-Leser, bewusst nur fuer die eine Form, die diese Datei hat:
// {"games": {"<Kennung>": {"feld": wert, ...}, ...}}
//
// Eine vollstaendige JSON-Bibliothek waere hier Ballast. Unbekannte Felder
// werden ueberlesen, damit eine spaetere Fassung die Datei erweitern kann,
// ohne dass diese daran scheitert.
// -------------------------------------------------------------------------
struct Leser {
    const std::string& s;
    size_t i = 0;
    explicit Leser(const std::string& text) : s(text) {}

    void Leerraum() { while (i < s.size() && (unsigned char)s[i] <= ' ') ++i; }
    bool Ist(char c) { Leerraum(); return i < s.size() && s[i] == c; }
    bool Nimm(char c) { if (Ist(c)) { ++i; return true; } return false; }

    bool String(std::string* aus) {
        Leerraum();
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        aus->clear();
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                ++i;
                switch (s[i]) {
                    case 'n': *aus += '\n'; break;
                    case 't': *aus += '\t'; break;
                    case 'r': break;
                    case 'u': {
                        // Nur der lateinische Bereich; alles andere wird zu '?'.
                        if (i + 4 < s.size()) {
                            const unsigned v = (unsigned)std::strtoul(
                                s.substr(i + 1, 4).c_str(), nullptr, 16);
                            if (v < 0x80) *aus += (char)v;
                            else if (v < 0x800) {
                                *aus += (char)(0xC0 | (v >> 6));
                                *aus += (char)(0x80 | (v & 0x3F));
                            } else {
                                *aus += (char)(0xE0 | (v >> 12));
                                *aus += (char)(0x80 | ((v >> 6) & 0x3F));
                                *aus += (char)(0x80 | (v & 0x3F));
                            }
                            i += 4;
                        }
                        break;
                    }
                    default: *aus += s[i];
                }
                ++i;
            } else {
                *aus += s[i++];
            }
        }
        if (i >= s.size()) return false;
        ++i;   // schliessendes "
        return true;
    }

    // Ueberspringt einen beliebigen Wert.
    bool WertUeberspringen() {
        Leerraum();
        if (i >= s.size()) return false;
        if (s[i] == '"') { std::string t; return String(&t); }
        if (s[i] == '{' || s[i] == '[') {
            const char zu = (s[i] == '{') ? '}' : ']';
            int tiefe = 0;
            for (; i < s.size(); ++i) {
                if (s[i] == '"') { std::string t; if (!String(&t)) return false; --i; continue; }
                if (s[i] == '{' || s[i] == '[') ++tiefe;
                else if (s[i] == '}' || s[i] == ']') {
                    --tiefe;
                    if (tiefe == 0) { ++i; return s[i - 1] == zu || true; }
                }
            }
            return false;
        }
        while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') ++i;
        return true;
    }

    bool Zahl(long* aus) {
        Leerraum();
        if (i < s.size() && (s[i] == 'n')) {          // null
            while (i < s.size() && isalpha((unsigned char)s[i])) ++i;
            *aus = 0;
            return true;
        }
        char* ende = nullptr;
        *aus = std::strtol(s.c_str() + i, &ende, 10);
        if (ende == s.c_str() + i) return false;
        i = (size_t)(ende - s.c_str());
        return true;
    }
};

std::string JsonText(const std::string& s) {
    std::string aus = "\"";
    for (char c : s) {
        switch (c) {
            case '"':  aus += "\\\""; break;
            case '\\': aus += "\\\\"; break;
            case '\n': aus += "\\n"; break;
            case '\t': aus += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) { char t[8]; std::snprintf(t, sizeof(t), "\\u%04x", c); aus += t; }
                else aus += c;
        }
    }
    return aus + "\"";
}

}  // namespace

bool GameIndex::Laden(const std::string& pfad) {
    std::ifstream f(pfad, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();

    Leser l(text);
    if (!l.Nimm('{')) return false;

    bool gefunden = false;
    for (;;) {
        std::string schluessel;
        if (!l.String(&schluessel)) break;
        if (!l.Nimm(':')) return false;
        if (schluessel == "games") {
            if (!l.Nimm('{')) return false;
            for (;;) {
                std::string kennung;
                if (!l.String(&kennung)) break;
                if (!l.Nimm(':')) return false;
                if (!l.Nimm('{')) return false;

                IndexEintrag e;
                for (;;) {
                    std::string feld;
                    if (!l.String(&feld)) break;
                    if (!l.Nimm(':')) return false;
                    if (feld == "gameid")            { long v = 0; l.Zahl(&v); e.gameid = (uint32_t)(v < 0 ? 0 : v); }
                    else if (feld == "achievements") { long v = 0; l.Zahl(&v); e.achievements = (int)v; }
                    else if (feld == "name")  { l.Leerraum(); if (l.Ist('"')) l.String(&e.name); else l.WertUeberspringen(); }
                    else if (feld == "title") { l.Leerraum(); if (l.Ist('"')) l.String(&e.titel); else l.WertUeberspringen(); }
                    else if (feld == "md5")   { l.Leerraum(); if (l.Ist('"')) l.String(&e.md5); else l.WertUeberspringen(); }
                    else if (feld == "image") { l.Leerraum(); if (l.Ist('"')) l.String(&e.abbild); else l.WertUeberspringen(); }
                    else                      { l.WertUeberspringen(); }
                    if (!l.Nimm(',')) break;
                }
                if (!l.Nimm('}')) return false;
                eintraege_[kennung] = e;
                gefunden = true;
                if (!l.Nimm(',')) break;
            }
            if (!l.Nimm('}')) return false;
        } else {
            l.WertUeberspringen();
        }
        if (!l.Nimm(',')) break;
    }
    return gefunden;
}

bool GameIndex::Speichern(const std::string& pfad) const {
    const std::string tmp = pfad + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f << "{\n \"games\": {\n";
        size_t n = 0;
        for (const auto& kv : eintraege_) {
            const IndexEintrag& e = kv.second;
            f << "  " << JsonText(kv.first) << ": {";
            if (e.gameid) f << "\"gameid\": " << e.gameid; else f << "\"gameid\": null";
            f << ", \"name\": "  << JsonText(e.name);
            f << ", \"title\": " << JsonText(e.titel);
            f << ", \"md5\": "   << JsonText(e.md5);
            f << ", \"image\": " << JsonText(e.abbild);
            f << ", \"achievements\": " << e.achievements;
            f << ", \"ra_supported\": " << (e.gameid ? "true" : "false");
            f << "}";
            if (++n < eintraege_.size()) f << ",";
            f << "\n";
        }
        f << " }\n}\n";
    }
    ::MoveFileExA(tmp.c_str(), pfad.c_str(), MOVEFILE_REPLACE_EXISTING);
    return true;
}

const IndexEintrag* GameIndex::Suchen(const std::string& kennung) const {
    const auto it = eintraege_.find(kennung);
    return it == eintraege_.end() ? nullptr : &it->second;
}

bool GameIndex::Setzen(const std::string& kennung, const IndexEintrag& e) {
    const bool neu = eintraege_.find(kennung) == eintraege_.end();
    eintraege_[kennung] = e;
    return neu;
}

std::string DatenOrdner() {
    // FESTER Ort, unabhaengig davon, wo die Exe gerade liegt.
    char pfad[MAX_PATH] = {};
    DWORD n = ::GetEnvironmentVariableA("LOCALAPPDATA", pfad, MAX_PATH);
    std::string d = (n > 0 && n < MAX_PATH) ? std::string(pfad) : std::string(".");
    d += "\\ra-saturn";
    ::CreateDirectoryA(d.c_str(), nullptr);
    return d;
}

std::string StandardIndexPfad() {
    return DatenOrdner() + "\\game_index.json";
}

std::string EinstellungsPfad() {
    return DatenOrdner() + "\\einstellungen.txt";
}

bool Einstellungen::Laden(const std::string& pfad) {
    std::ifstream f(pfad);
    if (!f) return false;
    std::string zeile;
    while (std::getline(f, zeile)) {
        if (!zeile.empty() && zeile.back() == '\r') zeile.pop_back();
        const size_t gleich = zeile.find('=');
        if (gleich == std::string::npos) continue;
        const std::string k = zeile.substr(0, gleich);
        const std::string v = zeile.substr(gleich + 1);
        if      (k == "rom_ordner") rom_ordner = v;
        else if (k == "saroocfg")   saroocfg = v;
        else if (k == "benutzer")   benutzer = v;
        else if (k == "port")       port = v;
        else if (k == "stil")       stil = v;
        else if (k == "sprache")    sprache = v;
        else if (k == "goldblitz")  goldblitz = v;
        else if (k == "anzeige")    anzeige = v;
    }
    return true;
}

bool Einstellungen::Speichern(const std::string& pfad) const {
    std::ofstream f(pfad, std::ios::binary);
    if (!f) return false;
    // Absichtlich OHNE Passwort. Zugangsdaten gehoeren nicht in eine
    // Klartextdatei, die niemand erwartet.
    f << "rom_ordner=" << rom_ordner << "\n";
    f << "saroocfg="   << saroocfg   << "\n";
    f << "benutzer="   << benutzer   << "\n";
    f << "port="       << port       << "\n";
    f << "stil="       << stil       << "\n";
    f << "sprache="    << sprache    << "\n";
    f << "goldblitz="  << goldblitz  << "\n";
    f << "anzeige="    << anzeige    << "\n";
    return true;
}

}  // namespace core
