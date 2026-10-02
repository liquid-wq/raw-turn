#include "core/protocol.h"

#include <cstdio>
#include <cstring>

#include <windows.h>   // GetLocalTime fuer BaueUhrJetzt

namespace core {
namespace {

uint32_t LiesBE32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

void SchreibeBE(std::vector<uint8_t>& v, uint32_t wert, int bytes) {
    for (int i = bytes - 1; i >= 0; --i)
        v.push_back((uint8_t)((wert >> (8 * i)) & 0xFF));
}

bool GroesseGueltig(uint8_t s) { return s == 1 || s == 2 || s == 4; }

std::vector<uint8_t> BaueRahmen(uint8_t typ, uint8_t seq,
                                const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> body;
    body.reserve(3 + payload.size());
    body.push_back(typ);
    body.push_back(seq);
    body.push_back((uint8_t)payload.size());
    body.insert(body.end(), payload.begin(), payload.end());

    std::vector<uint8_t> rahmen;
    rahmen.reserve(2 + body.size() + 1);
    rahmen.push_back(kMagic1);
    rahmen.push_back(kMagic2);
    rahmen.insert(rahmen.end(), body.begin(), body.end());
    rahmen.push_back(Crc8(body.data(), body.size()));
    return rahmen;
}

}  // namespace

uint8_t Crc8(const uint8_t* data, size_t len) {
    // Polynom 0x07, MSB zuerst, Startwert 0x00, kein abschliessendes XOR.
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}

bool Frame::LiesWatchReport(uint32_t* frame_count, std::vector<WatchEntry>* out) const {
    if (type != kTypeWatchReport) return false;
    if (payload.size() < (size_t)kWatchReportHeaderLen) return false;

    if (frame_count) *frame_count = LiesBE32(payload.data());
    if (!out) return true;

    out->clear();
    size_t off = kWatchReportHeaderLen;
    while (off < payload.size()) {
        if (off + 5 > payload.size()) return false;   // abgeschnitten
        WatchEntry e;
        e.addr = LiesBE32(&payload[off]);
        e.size = payload[off + 4];
        if (!GroesseGueltig(e.size)) return false;
        const size_t wert_start = off + 5;
        if (wert_start + e.size > payload.size()) return false;
        uint32_t v = 0;
        for (int i = 0; i < e.size; ++i) v = (v << 8) | payload[wert_start + i];
        e.value = v;
        out->push_back(e);
        off = wert_start + e.size;
    }
    return true;
}

bool Frame::LiesHello(uint8_t* version, std::string* serial) const {
    if (type != kTypeHello) return false;
    if (payload.size() < 2) return false;
    const uint8_t len = payload[1];
    if ((size_t)2 + len > payload.size()) return false;
    if (version) *version = payload[0];
    if (serial) serial->assign((const char*)&payload[2], len);
    return true;
}

bool Frame::LiesGameInfo(int* offset, std::vector<uint8_t>* daten) const {
    if (type != kTypeGameInfo) return false;
    if (payload.size() < 3) return false;
    const int off = ((int)payload[0] << 8) | (int)payload[1];
    const int n = (int)payload.size() - 2;
    if (off < 0 || n <= 0 || off + n > kDiscKopfLaenge) return false;
    if (offset) *offset = off;
    if (daten) daten->assign(payload.begin() + 2, payload.end());
    return true;
}

bool DiscKopfSammler::Fuettern(int offset, const std::vector<uint8_t>& daten) {
    if (offset % kGameInfoTeilLaenge != 0) return false;
    if ((int)daten.size() != kGameInfoTeilLaenge) return false;
    const int i = offset / kGameInfoTeilLaenge;
    if (i < 0 || i >= (int)(sizeof(da_) / sizeof(da_[0]))) return false;

    // Teil 0 beginnt einen neuen Kopf (die Firmware sendet 0, 1, 2, 3 der Reihe
    // nach). Sonst gaelte der Kopf nach einem Spielwechsel schon mit dem
    // ersten neuen Teil als vollstaendig: ein neuer und drei alte Teile,
    // richtige Kennung, falsche Pruefsumme.
    if (i == 0) Zuruecksetzen();

    bool anders = !da_[i];
    for (int b = 0; b < kGameInfoTeilLaenge; ++b) {
        if (kopf_[offset + b] != daten[(size_t)b]) anders = true;
        kopf_[offset + b] = daten[(size_t)b];
    }
    da_[i] = true;
    return anders;
}

bool DiscKopfSammler::Vollstaendig() const {
    for (bool d : da_) if (!d) return false;
    return true;
}

void DiscKopfSammler::Zuruecksetzen() {
    for (bool& d : da_) d = false;
}

std::vector<uint8_t> BaueWatchReport(const std::vector<WatchEntry>& entries,
                                     uint32_t frame_count, uint8_t seq) {
    std::vector<uint8_t> p;
    SchreibeBE(p, frame_count, kWatchReportHeaderLen);
    for (const WatchEntry& e : entries) {
        SchreibeBE(p, e.addr, 4);
        p.push_back(e.size);
        SchreibeBE(p, e.value, e.size);
    }
    return BaueRahmen(kTypeWatchReport, seq, p);
}

std::vector<std::vector<uint8_t>> BaueHookUebertragung(const std::string& kennung,
                                                       const std::string& inhalt) {
    if ((int)inhalt.size() > kHookMaxBytes) return {};

    std::vector<std::vector<uint8_t>> aus;
    uint8_t seq = 0;

    // BEGIN: 16 Byte Kennung, dann die Gesamtlaenge. Die Kennung wird auf
    // genau 16 Byte gebracht, weil das Geraet daraus den Dateinamen bildet.
    std::string k = kennung.substr(0, 16);
    k.resize(16, ' ');
    std::vector<uint8_t> p(k.begin(), k.end());
    p.push_back((uint8_t)((inhalt.size() >> 8) & 0xFF));
    p.push_back((uint8_t)(inhalt.size() & 0xFF));
    aus.push_back(BaueRahmen(kTypeHookBegin, seq++, p));

    // DATA: Offset plus Nutzdaten. 240 je Frame laesst Luft unter der
    // LEN-Grenze von 250 und haelt die Stuecke gleich gross.
    const int kStueck = 240;
    for (int off = 0; off < (int)inhalt.size(); off += kStueck) {
        const int n = (int)inhalt.size() - off < kStueck
                          ? (int)inhalt.size() - off : kStueck;
        std::vector<uint8_t> d;
        d.push_back((uint8_t)((off >> 8) & 0xFF));
        d.push_back((uint8_t)(off & 0xFF));
        d.insert(d.end(), inhalt.begin() + off, inhalt.begin() + off + n);
        aus.push_back(BaueRahmen(kTypeHookData, seq++, d));
    }

    // END: Laenge und eine einfache Bytesumme. Die CRC-8 je Frame sichert die
    // Uebertragung, die Summe sichert, dass alle Stuecke angekommen sind.
    uint32_t summe = 0;
    for (unsigned char c : inhalt) summe += c;
    std::vector<uint8_t> e;
    e.push_back((uint8_t)((inhalt.size() >> 8) & 0xFF));
    e.push_back((uint8_t)(inhalt.size() & 0xFF));
    e.push_back((uint8_t)((summe >> 24) & 0xFF));
    e.push_back((uint8_t)((summe >> 16) & 0xFF));
    e.push_back((uint8_t)((summe >> 8) & 0xFF));
    e.push_back((uint8_t)(summe & 0xFF));
    aus.push_back(BaueRahmen(kTypeHookEnd, seq++, e));

    return aus;
}

bool LiesHookAck(const Frame& f, uint8_t* auf_typ, uint8_t* status) {
    if (f.type != kTypeHookAck) return false;
    if (f.payload.size() < 2) return false;
    if (auf_typ) *auf_typ = f.payload[0];
    if (status)  *status  = f.payload[1];
    return true;
}

std::vector<std::vector<uint8_t>> BaueGameInfo(const uint8_t* kopf512, uint8_t seq) {
    std::vector<std::vector<uint8_t>> aus;
    for (int off = 0; off < kDiscKopfLaenge; off += kGameInfoTeilLaenge) {
        std::vector<uint8_t> p;
        p.push_back((uint8_t)(off >> 8));
        p.push_back((uint8_t)(off & 0xFF));
        p.insert(p.end(), kopf512 + off, kopf512 + off + kGameInfoTeilLaenge);
        aus.push_back(BaueRahmen(kTypeGameInfo, seq++, p));
    }
    return aus;
}

std::vector<uint16_t> GoldblitzFolge() {
    // Farbstufen 0x0BGR aus MEGA-RAW: Rot fuehrt, Gruen folgt, Blau bleibt 0.
    static const uint16_t hoch[]   = {0x002, 0x024, 0x048, 0x06A, 0x08C, 0x0AE};
    static const uint16_t runter[] = {0x0AE, 0x08C, 0x06A, 0x048, 0x024, 0x002, 0x000};
    std::vector<uint16_t> folge;
    for (uint16_t f : hoch) folge.push_back(0xA000 | f);
    for (int i = 0; i < 10; ++i) folge.push_back(0xA000 | 0x0AE);
    for (uint16_t f : runter) folge.push_back(0xA000 | f);
    folge.push_back(0x0000);
    return folge;
}

std::vector<uint8_t> BaueFileBegin(const std::string& pfad, uint32_t laenge,
                                   uint8_t seq) {
    std::vector<uint8_t> p;
    p.push_back((uint8_t)((laenge >> 24) & 0xFF));
    p.push_back((uint8_t)((laenge >> 16) & 0xFF));
    p.push_back((uint8_t)((laenge >> 8) & 0xFF));
    p.push_back((uint8_t)(laenge & 0xFF));
    // Der Pfad geht ohne Abschlussnull raus, die Laenge steht im Rahmenkopf.
    // Das Geraet fasst 63 Zeichen; laengeres wuerde dort abgeschnitten und
    // liefe ins Leere, deshalb hier schon begrenzen.
    for (size_t i = 0; i < pfad.size() && i < 63; ++i)
        p.push_back((uint8_t)pfad[i]);
    return BaueRahmen(kTypeFileBegin, seq, p);
}

std::vector<uint8_t> BaueFileData(const uint8_t* daten, int len, uint8_t seq) {
    if (len < 0) len = 0;
    if (len > kFileRahmenNutz) len = kFileRahmenNutz;
    return BaueRahmen(kTypeFileData, seq,
                      std::vector<uint8_t>(daten, daten + len));
}

std::vector<uint8_t> BaueFileEnd(uint32_t laenge, uint32_t summe, uint8_t seq) {
    std::vector<uint8_t> p;
    p.push_back((uint8_t)((laenge >> 24) & 0xFF));
    p.push_back((uint8_t)((laenge >> 16) & 0xFF));
    p.push_back((uint8_t)((laenge >> 8) & 0xFF));
    p.push_back((uint8_t)(laenge & 0xFF));
    p.push_back((uint8_t)((summe >> 24) & 0xFF));
    p.push_back((uint8_t)((summe >> 16) & 0xFF));
    p.push_back((uint8_t)((summe >> 8) & 0xFF));
    p.push_back((uint8_t)(summe & 0xFF));
    return BaueRahmen(kTypeFileEnd, seq, p);
}

const char* AckGrundText(uint8_t status) {
    switch (status) {
        case 0: return "angenommen";
        case 1: return "zu gross fuer den Geraetepuffer";
        case 2: return "Laengen stimmen nicht ueberein";
        case 3: return "Pruefsumme stimmt nicht";
        case 4: return "Datei liess sich auf der Karte nicht anlegen";
        case 5: return "ein Rahmen ist unterwegs verloren gegangen";
        case 6: return "Fehler beim Schreiben auf die Karte";
        case 7: return "Rahmen passt nicht zum Stand des Transfers";
        default: return "unbekannter Grund";
    }
}

std::vector<uint8_t> BaueUhrJetzt(uint8_t seq) {
    SYSTEMTIME t;
    ::GetLocalTime(&t);
    auto bcd = [](int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); };
    // Monat wie beim SMPC als Zahl 1..12 in den unteren vier Bit, darueber
    // der Wochentag (0 = Sonntag).
    return BaueRahmen(kTypeUhr, seq,
                      {bcd(t.wYear / 100), bcd(t.wYear % 100),
                       (uint8_t)(((t.wDayOfWeek & 7) << 4) | (t.wMonth & 0x0F)),
                       bcd(t.wDay), bcd(t.wHour), bcd(t.wMinute), bcd(t.wSecond)});
}

std::vector<uint8_t> BaueStand(uint8_t vorgang, uint8_t slot, uint8_t seq) {
    return BaueRahmen(kTypeStand, seq, {vorgang, slot});
}

bool LiesStandAntwort(const Frame& f, StandAntwort* aus) {
    if (f.type != kTypeStandAntw) return false;
    if (f.payload.size() < 4 + (size_t)kStandSlots * kStandEintrag) return false;
    if (f.payload[0] != 0x53 || f.payload[1] != 0x51) return false;   // "SQ"
    StandAntwort a;
    a.vorgang = f.payload[2];
    a.ergebnis = f.payload[3] & 0x7F;   // Bit 7: Wechselbit der Firmware
    for (int s = 0; s < kStandSlots; ++s) {
        const uint8_t* z = &f.payload[4 + (size_t)s * kStandEintrag];
        a.slots[s].belegt = z[0] != 0;
        for (int i = 0; i < 7; ++i) a.slots[s].zeit[i] = z[1 + i];
        a.slots[s].folge = ((int)z[8] << 8) | z[9];
        a.slots[s].bilder = ((uint32_t)z[10] << 24) | ((uint32_t)z[11] << 16) |
                            ((uint32_t)z[12] << 8) | z[13];
        a.slots[s].pal = z[14] != 0;
    }
    if (aus) *aus = a;
    return true;
}

static uint32_t Be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

bool LiesProt(const Frame& f, bool* luecke, std::vector<ProtSatz>* aus) {
    if (f.type != kTypeProt) return false;
    if (f.payload.empty() || (f.payload.size() - 1) % 8 != 0) return false;
    if (luecke) *luecke = (f.payload[0] & 1) != 0;
    if (aus) {
        aus->clear();
        for (size_t i = 1; i + 8 <= f.payload.size(); i += 8) {
            const uint32_t kopf = Be32(&f.payload[i]);
            ProtSatz s;
            s.eintrag = (uint16_t)(kopf >> 16);
            s.bild = (uint16_t)kopf;
            s.wert = Be32(&f.payload[i + 4]);
            aus->push_back(s);
        }
    }
    return true;
}

bool LiesProtZeit(const Frame& f, uint32_t* letzte_us, uint32_t* max_us) {
    if (f.type != kTypeProtZeit || f.payload.size() < 8) return false;
    if (letzte_us) *letzte_us = Be32(&f.payload[0]);
    if (max_us) *max_us = Be32(&f.payload[4]);
    return true;
}

std::vector<uint8_t> BaueTvBlitz(uint16_t wert, uint8_t seq) {
    return BaueRahmen(kTypeTvBlitz, seq,
                      {(uint8_t)((wert >> 8) & 0xFF), (uint8_t)(wert & 0xFF)});
}

std::vector<uint8_t> BaueHeartbeat(uint8_t seq) {
    return BaueRahmen(kTypeHeartbeat, seq, {});
}

std::vector<uint8_t> BaueHello(const std::string& serial, uint8_t version, uint8_t seq) {
    std::vector<uint8_t> p;
    p.push_back(version);
    p.push_back((uint8_t)serial.size());
    p.insert(p.end(), serial.begin(), serial.end());
    return BaueRahmen(kTypeHello, seq, p);
}

int FrameParser::FindeMagic() const {
    if (puffer_.size() < 2) return -1;
    for (size_t i = 0; i + 1 < puffer_.size(); ++i)
        if (puffer_[i] == kMagic1 && puffer_[i + 1] == kMagic2) return (int)i;
    return -1;
}

std::vector<Frame> FrameParser::Fuettern(const uint8_t* data, size_t len) {
    puffer_.insert(puffer_.end(), data, data + len);
    std::vector<Frame> ergebnis;

    for (;;) {
        const int idx = FindeMagic();
        if (idx < 0) {
            // Kein Magic: nur ein moeglicher Anfang bleibt stehen.
            if (!puffer_.empty() && puffer_.back() == kMagic1) {
                klartext.append(puffer_.begin(), puffer_.end() - 1);
                puffer_.erase(puffer_.begin(), puffer_.end() - 1);
            } else {
                klartext.append(puffer_.begin(), puffer_.end());
                puffer_.clear();
            }
            break;
        }
        if (idx > 0) {
            klartext.append(puffer_.begin(), puffer_.begin() + idx);
            puffer_.erase(puffer_.begin(), puffer_.begin() + idx);
            ++resyncs;
        }
        if (puffer_.size() < (size_t)kHeaderLen) break;   // auf mehr warten

        const uint8_t typ = puffer_[2];
        const uint8_t seq = puffer_[3];
        const uint8_t laenge = puffer_[4];

        if (laenge > kMaxPayload) {
            // Zufallstreffer auf die Magic-Bytes mitten in Nutzdaten.
            puffer_.erase(puffer_.begin());
            ++resyncs;
            continue;
        }

        const size_t gesamt = (size_t)kHeaderLen + laenge + kCrcLen;
        if (puffer_.size() < gesamt) break;              // auf den Rest warten

        const uint8_t* body = &puffer_[2];
        const size_t body_len = (size_t)3 + laenge;
        const uint8_t crc_soll = puffer_[kHeaderLen + laenge];

        if (Crc8(body, body_len) == crc_soll) {
            Frame f;
            f.type = typ;
            f.seq = seq;
            f.payload.assign(puffer_.begin() + kHeaderLen,
                             puffer_.begin() + kHeaderLen + laenge);
            ergebnis.push_back(std::move(f));
            ++frames_ok;
            puffer_.erase(puffer_.begin(), puffer_.begin() + gesamt);
        } else {
            ++frames_crc_fehler;
            if (crc_proben.size() < 64) {
                // Kopf, dann bis zu 40 Nutzbytes, dann die Pruefsumme.
                static const char* hex = "0123456789ABCDEF";
                std::string h, lesbar;
                const size_t n = gesamt < (size_t)48 ? gesamt : (size_t)48;
                for (size_t i = 0; i < n; ++i) {
                    const uint8_t b = puffer_[i];
                    h += hex[b >> 4];
                    h += hex[b & 15];
                    h += ' ';
                    lesbar += (b >= 0x20 && b < 0x7F) ? (char)b : '.';
                }
                char t[64];
                std::snprintf(t, sizeof(t), "typ %02X seq %02X len %u crc %02X/%02X: ",
                              typ, seq, (unsigned)laenge, crc_soll, Crc8(body, body_len));
                crc_proben.push_back(t + h + "| " + lesbar);
            }
            puffer_.erase(puffer_.begin());
            ++resyncs;
        }
    }
    return ergebnis;
}

}  // namespace core
