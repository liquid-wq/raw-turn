#include "core/mailbox.h"

#include <cstdio>

#include "core/i18n.h"

namespace core {
namespace {

std::string Zeile(uint32_t adresse, uint32_t wert) {
    char t[32];
    std::snprintf(t, sizeof(t), "M_%08x=%08x", adresse, wert);
    return t;
}

}  // namespace

std::string MagicZeile(uint32_t basis) {
    return Zeile(basis + kMagicOffset, kMagicWort);
}

std::string SperrZeile(bool sperre, uint32_t basis) {
    return Zeile(basis + kStatusOffset, sperre ? kWunschSperre : kWunschFrei);
}

std::vector<std::string> WatchZeilen(const std::vector<uint32_t>& adressen,
                                     uint32_t basis, std::string* fehler) {
    const int n = (int)adressen.size();
    if (n > kFirmwareMaxEintraege) {
        if (fehler) {
            char t[220];
            std::snprintf(t, sizeof(t),
                T("%d Watch-Adressen, aber ra_mailbox.c akzeptiert nur %d. Dort zuerst "
                  "erhöhen und den Patch neu bauen, sonst schneidet die Firmware "
                  "überzählige Einträge ohne Fehlermeldung ab."),
                n, kFirmwareMaxEintraege);
            *fehler = t;
        }
        return {};
    }
    // Grenze ist die Spielkennung bei +0xFF0, nicht mehr der Hook-Code.
    const int platz = (kKennungOffset - kEintraegeOffset) / kEintragBreite;
    if (n > platz) {
        if (fehler) {
            char t[200];
            std::snprintf(t, sizeof(t),
                T("%d Watch-Adressen brauchen mehr Platz als bis zum Hook-Code bei "
                  "+0x%x verfügbar ist (höchstens %d)."), n, kKennungOffset, platz);
            *fehler = t;
        }
        return {};
    }

    std::vector<std::string> aus;
    aus.push_back(Zeile(basis + kEintragZaehlOffset, (uint32_t)n << 16));
    for (int i = 0; i < n; ++i) {
        const uint32_t off = (uint32_t)(kEintraegeOffset + i * kEintragBreite);
        // Nur die Adresse, eine Zeile je Eintrag. SIZE liest niemand, VALUE
        // setzt die Saturn-Firmware beim Spielstart selbst auf null
        // (ra_watch_laden). So passen mehr Eintraege in den
        // Konfigurationspuffer der SAROO.
        aus.push_back(Zeile(basis + off, adressen[(size_t)i]));
    }
    return aus;
}

}  // namespace core
