// Selbsttest der Kernschicht. Konsolenprogramm, kein Fenster.
//
// Prueft das, was ohne Geraet und ohne RA-Zugang pruefbar ist: Drahtformat,
// Spielerkennung und das Lesen echter Disc-Abbilder.
//
// Wichtigster Punkt: Wird ein Abbild uebergeben, rechnet der Test die
// RA-Pruefsumme zweimal -- einmal aus der Datei, einmal aus denselben 512 Byte,
// nachdem sie als GAME_INFO-Frames ueber die Leitung gegangen sind. Genau so
// meldet die SAROO das laufende Spiel. Beide muessen gleich sein.
//
// Aufruf:  selbsttest.exe [Pfad zu einem Disc-Abbild] ...

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/bps.h"
#include "core/firmware.h"
#include "core/mailbox.h"
#include "core/protocol.h"
#include "core/ra.h"
#include "core/setup.h"
#include "erzeugt_patches.h"

namespace {

int fehler = 0;

void Pruefe(const char* name, bool bedingung, const std::string& detail = {}) {
    if (bedingung) {
        std::printf("  OK   %s\n", name);
    } else {
        std::printf("  FEHL %s%s%s\n", name, detail.empty() ? "" : "  --  ",
                    detail.c_str());
        ++fehler;
    }
}

void TestProtokoll() {
    std::printf("Drahtformat:\n");

    std::vector<core::WatchEntry> rein{
        {0x00200000u, 4, 0x12345678u},
        {0x0607C6CCu, 4, 0x00000001u},
    };
    const std::vector<uint8_t> rahmen = core::BaueWatchReport(rein, 26218, 7);

    core::FrameParser p;
    std::vector<core::Frame> raus = p.Fuettern(rahmen.data(), rahmen.size());
    Pruefe("ein Frame erkannt", raus.size() == 1, std::to_string(raus.size()));
    if (raus.empty()) return;

    uint32_t zaehler = 0;
    std::vector<core::WatchEntry> zurueck;
    Pruefe("WATCH_REPORT lesbar", raus[0].LiesWatchReport(&zaehler, &zurueck));
    Pruefe("Bildzaehler stimmt", zaehler == 26218, std::to_string(zaehler));
    Pruefe("zwei Eintraege", zurueck.size() == 2);
    if (zurueck.size() == 2) {
        Pruefe("Adresse stimmt", zurueck[0].addr == 0x00200000u);
        Pruefe("Wert stimmt", zurueck[0].value == 0x12345678u);
    }
    Pruefe("Sequenz stimmt", raus[0].seq == 7);

    // Stoerbytes davor muessen uebersprungen werden.
    core::FrameParser p2;
    std::vector<uint8_t> schmutz{0x00, 0xFF, 0xA5, 0x13, 0x42};
    schmutz.insert(schmutz.end(), rahmen.begin(), rahmen.end());
    Pruefe("Resync nach Stoerbytes",
           p2.Fuettern(schmutz.data(), schmutz.size()).size() == 1);
    Pruefe("Resync wurde gezaehlt", p2.resyncs > 0);

    // Verbogene Pruefsumme muss verworfen werden.
    core::FrameParser p3;
    std::vector<uint8_t> kaputt = rahmen;
    kaputt.back() ^= 0xFF;
    Pruefe("CRC-Fehler wird verworfen",
           p3.Fuettern(kaputt.data(), kaputt.size()).empty());
    Pruefe("CRC-Fehler wurde gezaehlt", p3.frames_crc_fehler > 0);

    // Goldblitz: Folge wie in MEGA-RAW, endet mit 0, kommt als 2-Byte-Wort an.
    const std::vector<uint16_t> folge = core::GoldblitzFolge();
    Pruefe("Goldblitz hat 24 Stufen", folge.size() == 24, std::to_string(folge.size()));
    Pruefe("Goldblitz endet mit 0", !folge.empty() && folge.back() == 0);
    bool alle_an = true;
    for (size_t i = 0; i + 1 < folge.size(); ++i)
        if ((folge[i] & 0xF000) != 0xA000) alle_an = false;
    Pruefe("Goldblitz-Stufen tragen die Kennung A", alle_an);
    const std::vector<uint8_t> blitz = core::BaueTvBlitz(0xA0AE, 5);
    core::FrameParser p4;
    const std::vector<core::Frame> bf = p4.Fuettern(blitz.data(), blitz.size());
    Pruefe("Goldblitz-Frame kommt an", bf.size() == 1 && bf[0].type == core::kTypeTvBlitz &&
           bf[0].payload.size() == 2 && bf[0].payload[0] == 0xA0 && bf[0].payload[1] == 0xAE);

    // Dateitransfer: Ein Transfer wird so nachgestellt, wie der Lese-Thread ihn
    // sendet, und am anderen Ende wieder zusammengesetzt. Geprueft wird, was
    // die Firmware prueft: Folgenummern ohne Luecke, Gesamtlaenge, Bytesumme.
    std::printf("Datei auf die Karte uebertragen:\n");
    {
        // Etwas mehr als zwei Bloecke, damit ein kuerzerer Rest entsteht.
        std::vector<uint8_t> inhalt;
        for (int i = 0; i < 8600; ++i) inhalt.push_back((uint8_t)(i * 7 + (i >> 5)));
        const std::string pfad = "/SAROO/ssfirm.bin";

        uint32_t summe = 0;
        for (uint8_t b : inhalt) summe += b;

        std::vector<std::vector<uint8_t>> raus;
        uint8_t seq = 0;
        raus.push_back(core::BaueFileBegin(pfad, (uint32_t)inhalt.size(), seq++));
        size_t ab = 0;
        while (ab < inhalt.size()) {
            int n = (int)(inhalt.size() - ab);
            if (n > core::kFileRahmenNutz) n = core::kFileRahmenNutz;
            raus.push_back(core::BaueFileData(&inhalt[ab], n, seq++));
            ab += (size_t)n;
        }
        raus.push_back(core::BaueFileEnd((uint32_t)inhalt.size(), summe, seq++));

        core::FrameParser pf;
        std::vector<uint8_t> empfangen;
        std::string pfad_an;
        uint32_t soll = 0, end_len = 0, end_summe = 0;
        bool folge_ok = true;
        int erwartet = 0;
        int begin = 0, ende = 0, daten = 0;

        for (const std::vector<uint8_t>& r : raus) {
            for (const core::Frame& f : pf.Fuettern(r.data(), r.size())) {
                if (f.seq != (uint8_t)erwartet) folge_ok = false;
                ++erwartet;
                if (f.type == core::kTypeFileBegin) {
                    ++begin;
                    soll = ((uint32_t)f.payload[0] << 24) | ((uint32_t)f.payload[1] << 16) |
                           ((uint32_t)f.payload[2] << 8) | f.payload[3];
                    pfad_an.assign(f.payload.begin() + 4, f.payload.end());
                } else if (f.type == core::kTypeFileData) {
                    ++daten;
                    empfangen.insert(empfangen.end(), f.payload.begin(), f.payload.end());
                } else if (f.type == core::kTypeFileEnd) {
                    ++ende;
                    end_len = ((uint32_t)f.payload[0] << 24) | ((uint32_t)f.payload[1] << 16) |
                              ((uint32_t)f.payload[2] << 8) | f.payload[3];
                    end_summe = ((uint32_t)f.payload[4] << 24) | ((uint32_t)f.payload[5] << 16) |
                                ((uint32_t)f.payload[6] << 8) | f.payload[7];
                }
            }
        }

        uint32_t summe_an = 0;
        for (uint8_t b : empfangen) summe_an += b;

        Pruefe("genau ein BEGIN", begin == 1);
        Pruefe("genau ein END", ende == 1);
        Pruefe("Folgenummern ohne Luecke", folge_ok);
        Pruefe("Zielpfad kommt unveraendert an", pfad_an == pfad, pfad_an);
        Pruefe("angekuendigte Laenge stimmt", soll == inhalt.size());
        Pruefe("Inhalt kommt vollstaendig an", empfangen == inhalt,
               std::to_string(empfangen.size()) + " von " + std::to_string(inhalt.size()));
        Pruefe("Laenge im Abschluss stimmt", end_len == inhalt.size());
        Pruefe("Bytesumme stimmt", end_summe == summe && summe_an == summe);
        // Kein Rahmen darf mehr Nutzdaten tragen, als der Geraetepuffer je
        // Rahmen annimmt.
        bool klein_genug = true;
        for (const std::vector<uint8_t>& r : raus)
            if ((int)r.size() > core::kFileRahmenNutz + 7) klein_genug = false;
        Pruefe("kein Rahmen ist zu gross", klein_genug);
        // Ein zu langer Pfad wird gekappt, statt im Geraet ins Leere zu laufen.
        const std::vector<uint8_t> lang =
            core::BaueFileBegin(std::string(200, 'x'), 1, 0);
        Pruefe("langer Pfad wird gekappt", lang.size() <= (size_t)(4 + 63 + 7),
               std::to_string(lang.size()));
    }
}

// Den Weg nachstellen, den die Firmware geht: 512 Byte Disc-Kopf in vier
// GAME_INFO-Frames, durch den Empfaenger, wieder zusammengesetzt.
bool DurchDieLeitung(const unsigned char* kopf, core::DiscKopf* dk,
                     std::string* md5) {
    core::FrameParser parser;
    core::DiscKopfSammler sammler;
    for (const std::vector<uint8_t>& rahmen : core::BaueGameInfo(kopf, 3)) {
        for (const core::Frame& f : parser.Fuettern(rahmen.data(), rahmen.size())) {
            if (f.type != core::kTypeGameInfo) continue;
            int off = 0;
            std::vector<uint8_t> d;
            if (f.LiesGameInfo(&off, &d)) sammler.Fuettern(off, d);
        }
    }
    if (!sammler.Vollstaendig()) return false;
    if (!core::DeuteDiscKopf(sammler.Bytes(), dk)) return false;
    if (md5) *md5 = core::HashAusKopf(sammler.Bytes());
    return true;
}

void TestSpielerkennung() {
    std::printf("Spielerkennung ueber GAME_INFO:\n");

    Pruefe("vier Frames fuer 512 Byte", core::BaueGameInfo((const unsigned char*)
           "                                                                ", 0)
           .size() == 4);

    unsigned char kopf[512];
    std::memset(kopf, ' ', sizeof(kopf));
    std::memcpy(kopf + 0x00, "SEGA SEGASATURN ", 16);
    std::memcpy(kopf + 0x20, "GS-9079   V0.001", 16);
    std::memcpy(kopf + 0x40, "E", 1);
    std::memcpy(kopf + 0x60, "TESTSPIEL", 9);

    core::DiscKopf dk;
    std::string md5;
    Pruefe("Kopf kommt durch die Leitung", DurchDieLeitung(kopf, &dk, &md5));
    Pruefe("Kennung stimmt", dk.kennung == "GS-9079   V0.001", "'" + dk.kennung + "'");
    Pruefe("Titel stimmt", dk.titel == "TESTSPIEL", "'" + dk.titel + "'");
    Pruefe("Gebiet stimmt", dk.gebiete == "Europa", "'" + dk.gebiete + "'");
    Pruefe("Pruefsumme ist 32 Zeichen", md5.size() == 32, md5);

    // Ohne Pruefstein darf NICHTS behauptet werden.
    std::memcpy(kopf, "XXXX", 4);
    Pruefe("falscher Pruefstein wird abgelehnt", !DurchDieLeitung(kopf, &dk, &md5));

    // Ein fehlender Teil macht die Pruefsumme falsch -- also gar nichts melden.
    std::memcpy(kopf, "SEGA SEGASATURN ", 16);
    core::DiscKopfSammler halb;
    const std::vector<std::vector<uint8_t>> rahmen = core::BaueGameInfo(kopf, 0);
    core::FrameParser p;
    for (size_t i = 0; i + 1 < rahmen.size(); ++i)
        for (const core::Frame& f : p.Fuettern(rahmen[i].data(), rahmen[i].size())) {
            int off = 0; std::vector<uint8_t> d;
            if (f.LiesGameInfo(&off, &d)) halb.Fuettern(off, d);
        }
    Pruefe("unvollstaendiger Kopf gilt nicht", !halb.Vollstaendig());

    std::string f;
    Pruefe("Ueberlauf der Watch-Liste wird abgefangen",
           core::WatchZeilen(std::vector<uint32_t>(400, 0x00200000u),
                             core::kMailboxBasis, &f).empty());
    Pruefe("Ueberlauf wird begruendet", !f.empty());
    // 157 Adressen (Croc): eine Zeile je Eintrag plus Zaehler.
    const std::vector<std::string> croc =
        core::WatchZeilen(std::vector<uint32_t>(157, 0x06001000u), core::kMailboxBasis, &f);
    Pruefe("157 Adressen passen, eine Zeile je Eintrag", croc.size() == 158);
}
// Der Abschnittsname entscheidet, ob ein nachgetragener Hook ueberhaupt
// gelesen wird: Die Firmware nimmt nur den ERSTEN passenden Abschnitt.
void TestKennungsvergleich() {
    std::printf("Abschnittsnamen wie die Firmware vergleichen:\n");
    Pruefe("gleiche Kennung, andere Leerzeichen",
           core::KennungGleich("T-11304H  V3.08 ", "T-11304H   V3.08"));
    Pruefe("getrimmt gegen roh",
           core::KennungGleich("T-11304H  V3.08", "T-11304H  V3.08  "));
    Pruefe("andere Version zaehlt nicht als gleich",
           !core::KennungGleich("T-11304H  V3.08", "T-11304H  V1.000"));
    Pruefe("andere Nummer zaehlt nicht als gleich",
           !core::KennungGleich("T-11304H  V3.08", "T-10001G  V3.08"));
    Pruefe("Kennung aus dem Disc-Kopf passt auf den Abschnitt",
           core::KennungGleich("GS-9079   V0.001", "GS-9079 V0.001"));
}
// Der Hook-Satz geht jetzt ueber die Leitung, weil die SD-Karte beim Spielen
// in der SAROO steckt. Hier wird geprueft, dass er dabei unveraendert und
// vollstaendig ankommt -- Rahmen bauen, durch den Empfaenger schicken, wieder
// zusammensetzen, mit dem Original vergleichen.
void TestHookUebertragung() {
    std::printf("Hook-Satz zum Geraet uebertragen:\n");

    std::string inhalt;
    for (int i = 0; i < 200; ++i) {
        char z[32];
        std::snprintf(z, sizeof(z), "M_%08x=%08x\n", 0x023f0010u + i * 4, i * 7 + 1);
        inhalt += z;
    }
    const std::string kennung = "T-10001G  V1.000";

    const std::vector<std::vector<uint8_t>> rahmen =
        core::BaueHookUebertragung(kennung, inhalt);
    Pruefe("Uebertragung wird erzeugt", !rahmen.empty());
    if (rahmen.empty()) return;

    // Durch den Empfaenger schicken, alle Rahmen hintereinander wie am Draht.
    core::FrameParser p;
    std::vector<uint8_t> draht;
    for (const std::vector<uint8_t>& r : rahmen) draht.insert(draht.end(), r.begin(), r.end());

    std::string name;
    int soll = -1;
    std::vector<char> puffer(core::kHookMaxBytes, 0);
    int belegt = 0;
    int begin = 0, data = 0, ende = 0;

    for (const core::Frame& f : p.Fuettern(draht.data(), draht.size())) {
        if (f.type == core::kTypeHookBegin) {
            ++begin;
            name.assign((const char*)f.payload.data(), 16);
            soll = ((int)f.payload[16] << 8) | f.payload[17];
        } else if (f.type == core::kTypeHookData) {
            ++data;
            const int off = ((int)f.payload[0] << 8) | f.payload[1];
            for (size_t i = 2; i < f.payload.size(); ++i)
                puffer[(size_t)off + i - 2] = (char)f.payload[i];
            const int bis = off + (int)f.payload.size() - 2;
            if (bis > belegt) belegt = bis;
        } else if (f.type == core::kTypeHookEnd) {
            ++ende;
            uint32_t summe = 0;
            for (int i = 0; i < belegt; ++i) summe += (unsigned char)puffer[(size_t)i];
            const int laenge = ((int)f.payload[0] << 8) | f.payload[1];
            const uint32_t erwartet = ((uint32_t)f.payload[2] << 24) |
                                      ((uint32_t)f.payload[3] << 16) |
                                      ((uint32_t)f.payload[4] << 8) | f.payload[5];
            Pruefe("Laenge stimmt ueberein", laenge == belegt && laenge == soll);
            Pruefe("Bytesumme stimmt ueberein", summe == erwartet);
        }
    }

    Pruefe("genau ein BEGIN", begin == 1, std::to_string(begin));
    Pruefe("genau ein END", ende == 1, std::to_string(ende));
    Pruefe("mehrere DATA-Teile", data > 1, std::to_string(data));
    Pruefe("Kennung kommt unveraendert an", name == kennung, "'" + name + "'");
    Pruefe("Inhalt kommt unveraendert an",
           belegt == (int)inhalt.size() &&
           std::memcmp(puffer.data(), inhalt.data(), inhalt.size()) == 0);

    // Zu grosse Saetze muessen abgelehnt werden, statt am Geraet aufzulaufen.
    Pruefe("zu grosser Satz wird abgelehnt",
           core::BaueHookUebertragung(kennung,
               std::string(core::kHookMaxBytes + 1, 'x')).empty());

    // Titelzeile der Hook-Datei: erste Zeile, gekuerzt, UTF-8 heil.
    core::SetupErgebnis e;
    e.zeilen = {"M_023F0000=5241"};
    e.name = "Powerslave\r\n";
    Pruefe("Titelzeile vor den M_-Zeilen",
           core::HookDateiInhalt(e) == "; Powerslave\nM_023F0000=5241\n",
           core::HookDateiInhalt(e));
    e.name = std::string(59, 'a') + "\xC3\xA4" + "b";   // ä liegt auf Byte 60/61
    const std::string lang = core::HookDateiInhalt(e);
    Pruefe("langer Titel ohne halbes UTF-8-Zeichen",
           lang.substr(0, lang.find('\n')) == "; " + std::string(59, 'a'), lang);
    e.name.clear();
    Pruefe("ohne Titel keine Kommentarzeile",
           core::HookDateiInhalt(e) == "M_023F0000=5241\n");
}

void TestAbbild(const std::string& pfad) {
    std::printf("Disc-Abbild: %s\n", pfad.c_str());
    core::DiscKopf kopf;
    std::string f, roh;
    if (!core::LiesDiscKopf(pfad, &kopf, &f, &roh)) {
        std::printf("  FEHL Kopf nicht lesbar  --  %s\n", f.c_str());
        ++fehler;
        return;
    }
    std::printf("    Kennung : [%s]\n", kopf.kennung.c_str());
    std::printf("    Titel   : %s\n", kopf.titel.c_str());
    std::printf("    Gebiet  : %s\n", kopf.gebiete.c_str());
    std::printf("    Datum   : %s\n", kopf.datum.c_str());
    const std::string md5 = core::SaturnHash(pfad);
    std::printf("    MD5     : %s\n", md5.c_str());
    Pruefe("Kennung ist 16 Byte lang", kopf.kennung.size() == 16);
    Pruefe("Titel ist nicht leer", !kopf.titel.empty());
    Pruefe("Pruefsumme ist 32 Zeichen", md5.size() == 32, md5);

    // Der entscheidende Vergleich. Links: was rcheevos aus der DATEI rechnet.
    // Rechts: was entsteht, wenn dieselben 512 Byte den Weg der Firmware
    // nehmen, also als GAME_INFO-Frames ueber die Leitung. Sind beide gleich,
    // braucht der Betrieb keine Datei und keine Einrichtung.
    core::DiscKopf dk;
    std::string md5_konsole;
    Pruefe("Kopf kommt durch die Leitung",
           DurchDieLeitung((const unsigned char*)roh.data(), &dk, &md5_konsole));
    Pruefe("Kennung stimmt mit der Datei ueberein", dk.kennung == kopf.kennung,
           "'" + dk.kennung + "' gegen '" + kopf.kennung + "'");
    Pruefe("Titel stimmt mit der Datei ueberein", dk.titel == kopf.titel,
           "'" + dk.titel + "' gegen '" + kopf.titel + "'");
    Pruefe("Pruefsumme von der SAROO ist die der Datei", md5_konsole == md5,
           md5_konsole + " gegen " + md5);

    // Einrichtung ohne RA-Zugang: Sie ist nur noch fuer Achievements da. Ohne
    // Set gibt es keine Watch-Adressen und damit auch nichts zu installieren --
    // die Erkennung laeuft trotzdem, weil sie daran nicht haengt.
    core::RaClient ohne_zugang;
    const core::SetupErgebnis e = core::VerarbeiteAbbild(pfad, ohne_zugang, false);
    std::printf("    M_-Zeilen: %d\n", e.paare);
    Pruefe("Hook ist installierbar", e.installierbar, e.status + " " + e.hinweis);
}

// BPS: ein kleiner Patch von Hand, dazu die eingebetteten Firmware-Patches.
void TestBps() {
    std::printf("BPS-Patches:\n");
    const std::vector<uint8_t> quelle = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'};
    const std::vector<uint8_t> soll = {'A', 'B', 'C', 'D', 'x', 'y', 'G', 'H'};
    std::vector<uint8_t> p = {'B', 'P', 'S', '1', 0x88, 0x88, 0x80,
                              0x8C,              // 4 Byte aus dem Original
                              0x85, 'x', 'y',    // 2 neue Byte
                              0x84};             // 2 Byte aus dem Original
    auto le32 = [&p](uint32_t v) { for (int i = 0; i < 4; ++i) p.push_back((uint8_t)(v >> (8 * i))); };
    le32(core::Crc32(quelle.data(), quelle.size()));
    le32(core::Crc32(soll.data(), soll.size()));
    le32(core::Crc32(p.data(), p.size()));
    std::vector<uint8_t> ziel;
    std::string f;
    Pruefe("Handpatch angewendet", core::BpsAnwenden(p.data(), p.size(), quelle, &ziel, &f), f);
    Pruefe("Handpatch ergibt das Ziel", ziel == soll);
    std::vector<uint8_t> falsch = quelle;
    falsch[0] = 'Z';
    Pruefe("falsches Original abgelehnt", !core::BpsAnwenden(p.data(), p.size(), falsch, &ziel, &f));

    const struct { const char* name; const unsigned char* d; size_t n; } eingebettet[] = {
        {"mcuapp.bps", patches::kMcuapp, sizeof(patches::kMcuapp)},
        {"ssfirm.bps", patches::kSsfirm, sizeof(patches::kSsfirm)},
    };
    for (const auto& e : eingebettet) {
        uint32_t a = 0, b = 0;
        const bool kopf = core::BpsPruefsummen(e.d, e.n, &a, &b);
        const bool ganz = kopf && core::Crc32(e.d, e.n - 4) ==
            ((uint32_t)e.d[e.n - 4] | ((uint32_t)e.d[e.n - 3] << 8) |
             ((uint32_t)e.d[e.n - 2] << 16) | ((uint32_t)e.d[e.n - 1] << 24));
        Pruefe((std::string(e.name) + " eingebettet und unversehrt").c_str(), ganz);
    }
}

// Mit "--v09 <Ordner>": die eingebetteten Patches auf die offizielle v0.9
// anwenden, die dort im Aufbau des Pakets liegt (SAROO\...).
void TestFirmware(const std::string& ordner) {
    std::printf("Firmware-Patches auf v0.9:\n");
    const struct { const char* datei; const unsigned char* d; size_t n; } teile[] = {
        {"\\SAROO\\mcuapp.bin", patches::kMcuapp, sizeof(patches::kMcuapp)},
        {"\\SAROO\\ssfirm.bin", patches::kSsfirm, sizeof(patches::kSsfirm)},
    };
    for (const auto& t : teile) {
        std::vector<uint8_t> quelle;
        FILE* fp = std::fopen((ordner + t.datei).c_str(), "rb");
        if (fp) {
            int c;
            while ((c = std::fgetc(fp)) != EOF) quelle.push_back((uint8_t)c);
            std::fclose(fp);
        }
        std::vector<uint8_t> ziel;
        std::string f;
        Pruefe((std::string(t.datei) + " gepatcht").c_str(),
               core::BpsAnwenden(t.d, t.n, quelle, &ziel, &f), f);
    }
}

// Mit "--karte <Ordner>": den ganzen Ablauf an einer Karte oder ihrer Kopie.
// Patchen, Stand pruefen, zurueckstellen, Stand pruefen. Laedt bei Bedarf
// das offizielle Paket.
void TestKarte(const std::string& wurzel) {
    std::printf("Firmware auf der Karte %s:\n", wurzel.c_str());
    std::vector<std::string> log;
    std::string f;
    Pruefe("gepatcht", core::FirmwarePatchen(wurzel, &log, &f), f);
    for (const std::string& z : log) std::printf("    %s\n", z.c_str());
    core::FwStand st = core::FirmwarePruefen(wurzel);
    for (const core::FwTeilStand& t : st.teile)
        Pruefe((std::string(t.name) + " ist gepatcht").c_str(), t.lage == core::FwLage::RawTurn);
    log.clear();
    Pruefe("zurueckgestellt", core::FirmwareZurueck(wurzel, &log, &f), f);
    for (const std::string& z : log) std::printf("    %s\n", z.c_str());
    st = core::FirmwarePruefen(wurzel);
    for (const core::FwTeilStand& t : st.teile)
        Pruefe((std::string(t.name) + " ist wieder Original").c_str(),
               t.lage == core::FwLage::Original);
}

}  // namespace

int main(int argc, char** argv) {
    TestProtokoll();
    TestSpielerkennung();
    TestKennungsvergleich();
    TestHookUebertragung();
    TestBps();
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--v09") == 0 && i + 1 < argc) TestFirmware(argv[++i]);
        else if (std::strcmp(argv[i], "--karte") == 0 && i + 1 < argc) TestKarte(argv[++i]);
        else TestAbbild(argv[i]);
    }

    std::printf("\n");
    if (fehler) { std::printf("FEHLGESCHLAGEN: %d\n", fehler); return 1; }
    std::printf("Alle Selbsttests der Kernschicht erfolgreich.\n");
    return 0;
}
