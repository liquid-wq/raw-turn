#include "core/bps.h"

namespace core {

uint32_t Crc32(const uint8_t* daten, size_t laenge) {
    static uint32_t tabelle[256];
    static bool fertig = false;
    if (!fertig) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tabelle[i] = c;
        }
        fertig = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < laenge; ++i) crc = tabelle[(crc ^ daten[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

namespace {

uint32_t Le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Zahl in BPS-Kodierung. false, wenn sie ueber das Ende hinauslaeuft.
bool LiesZahl(const uint8_t* d, size_t ende, size_t* pos, uint64_t* wert) {
    uint64_t x = 0, s = 1;
    for (int i = 0; i < 10; ++i) {
        if (*pos >= ende) return false;
        const uint8_t b = d[(*pos)++];
        x += (uint64_t)(b & 0x7F) * s;
        if (b & 0x80) { *wert = x; return true; }
        s <<= 7;
        x += s;
    }
    return false;
}

}  // namespace

bool BpsPruefsummen(const uint8_t* patch, size_t laenge,
                    uint32_t* crc_quelle, uint32_t* crc_ziel) {
    if (laenge < 4 + 3 + 12) return false;
    if (patch[0] != 'B' || patch[1] != 'P' || patch[2] != 'S' || patch[3] != '1') return false;
    *crc_quelle = Le32(patch + laenge - 12);
    *crc_ziel = Le32(patch + laenge - 8);
    return true;
}

bool BpsAnwenden(const uint8_t* patch, size_t patch_laenge,
                 const std::vector<uint8_t>& quelle,
                 std::vector<uint8_t>* ziel, std::string* fehler) {
    uint32_t crc_quelle = 0, crc_ziel = 0;
    if (!BpsPruefsummen(patch, patch_laenge, &crc_quelle, &crc_ziel)) {
        *fehler = "Patch beschädigt";
        return false;
    }
    if (Crc32(patch, patch_laenge - 4) != Le32(patch + patch_laenge - 4)) {
        *fehler = "Patch beschädigt";
        return false;
    }
    if (Crc32(quelle.data(), quelle.size()) != crc_quelle) {
        *fehler = "Originaldatei passt nicht zum Patch";
        return false;
    }

    const size_t ende = patch_laenge - 12;
    size_t pos = 4;
    uint64_t q_laenge = 0, z_laenge = 0, meta = 0;
    if (!LiesZahl(patch, ende, &pos, &q_laenge) || !LiesZahl(patch, ende, &pos, &z_laenge) ||
        !LiesZahl(patch, ende, &pos, &meta) || q_laenge != quelle.size() ||
        z_laenge > 64u * 1024u * 1024u || meta > ende - pos) {
        *fehler = "Patch beschädigt";
        return false;
    }
    pos += (size_t)meta;

    std::vector<uint8_t> aus;
    aus.reserve((size_t)z_laenge);
    int64_t q_rel = 0, z_rel = 0;
    while (pos < ende) {
        uint64_t d = 0;
        if (!LiesZahl(patch, ende, &pos, &d)) { *fehler = "Patch beschädigt"; return false; }
        const int art = (int)(d & 3);
        const uint64_t n = (d >> 2) + 1;
        if (aus.size() + n > z_laenge) { *fehler = "Patch beschädigt"; return false; }
        if (art == 0) {
            if (aus.size() + n > quelle.size()) { *fehler = "Patch beschädigt"; return false; }
            const size_t von = aus.size();
            aus.insert(aus.end(), quelle.begin() + (ptrdiff_t)von,
                       quelle.begin() + (ptrdiff_t)(von + n));
        } else if (art == 1) {
            if (n > ende - pos) { *fehler = "Patch beschädigt"; return false; }
            aus.insert(aus.end(), patch + pos, patch + pos + n);
            pos += (size_t)n;
        } else {
            uint64_t o = 0;
            if (!LiesZahl(patch, ende, &pos, &o)) { *fehler = "Patch beschädigt"; return false; }
            const int64_t versatz = (o & 1) ? -(int64_t)(o >> 1) : (int64_t)(o >> 1);
            if (art == 2) {
                q_rel += versatz;
                if (q_rel < 0 || (uint64_t)q_rel + n > quelle.size()) {
                    *fehler = "Patch beschädigt";
                    return false;
                }
                aus.insert(aus.end(), quelle.begin() + (ptrdiff_t)q_rel,
                           quelle.begin() + (ptrdiff_t)(q_rel + (int64_t)n));
                q_rel += (int64_t)n;
            } else {
                z_rel += versatz;
                // Byteweise: Die Quelle darf sich mit dem Ziel ueberlappen.
                for (uint64_t i = 0; i < n; ++i) {
                    if (z_rel < 0 || (uint64_t)z_rel >= aus.size()) {
                        *fehler = "Patch beschädigt";
                        return false;
                    }
                    aus.push_back(aus[(size_t)z_rel++]);
                }
            }
        }
    }
    if (aus.size() != z_laenge || Crc32(aus.data(), aus.size()) != crc_ziel) {
        *fehler = "Ergebnis stimmt nicht mit der Prüfsumme überein";
        return false;
    }
    *ziel = std::move(aus);
    return true;
}

}  // namespace core
