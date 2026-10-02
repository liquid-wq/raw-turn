#include "connection.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "buildinfo.h"
#include "core/i18n.h"
#include "core/mailbox.h"
#include "core/serial.h"
#include "core/setup.h"    // DeuteDiscKopf, HashAusKopf

namespace {

// Ab so vielen Berichten mit unveraendertem FRAME_COUNT gilt der Hook als
// ausgefallen. Ein paar Wiederholungen sind normal, weil die STM32 unabhaengig vom VBlank
// der Saturn pollt und denselben Tick gelegentlich mehrfach erwischt.
constexpr int kStillstandSchwelle = 120;

// Dasselbe ueber die Zeit: Bleibt der Bildzaehler so lange unveraendert, steht
// der Hook, auch wenn gar keine Berichte mehr kommen. Die STM32 schickt bei
// stehendem Zaehler nur alle zwei Sekunden einen Bericht und bei einem
// haengenden Seqlock gar keinen; die Anzahl allein reicht deshalb nicht.
// Beim Nachladen von der CD steht der Zaehler ganz normal ein paar Sekunden,
// deshalb 10 s.
constexpr unsigned long kStillstandMs = 10000;

// Woerter, die in der Klartextausgabe der SAROO vorkommen. Aus echten
// Mitschnitten.
//
// Die erste Gruppe faellt nur bei Disc-Zugriff an. Wer im Startmenue steht,
// saehe davon nichts, und das Werkzeug meldete faelschlich, am Anschluss
// haenge keine SAROO. Die zweite Gruppe
// stammt aus Start und Startmenue und schliesst diese Luecke.
bool SarooKlartext(const std::string& s) {
    static const char* const hinweise[] = {
        "stm32>", "play_task", "fad_start", "fad_end", "seek_cd",
        "buffer full", "abort_file", "init_cdblock", "lba_",
        "SSMaster start", "SDCARD", "Mount SDFS", "Total discs",
        "FPGA config", "Saturn bootrom", "Load config", "disk_read",
        "ra: ",
    };
    for (const char* h : hinweise)
        if (s.find(h) != std::string::npos) return true;
    return false;
}

std::string Lesbar(const uint8_t* d, int n) {
    std::string s;
    for (int i = 0; i < n && (int)s.size() < 160; ++i) {
        const uint8_t b = d[i];
        if (b == '\n') s += "\\n";
        else if (b >= 32 && b < 127) s += (char)b;
        else s += '.';
    }
    return s;
}

// Aufzeichnung fuer die Fehlersuche: die komplette Textausgabe der SAROO und
// alle Meldungen des Werkzeugs, mit Uhrzeit, in eine Datei neben dem Programm
// (saroo_aufzeichnung.txt). Jede Zeile wird sofort geschrieben, damit nach
// einem Haenger nichts fehlt. Bleibt die STM32 stehen, endet ihre Ausgabe
// hier sichtbar; die Statuszeilen alle fuenf Sekunden laufen weiter.
class Aufzeichnung {
public:
    void Zeile(const char* quelle, const std::string& text) {
        std::lock_guard<std::mutex> g(sperre_);
        if (!Offen()) return;
        SYSTEMTIME st;
        ::GetLocalTime(&st);
        std::fprintf(datei_, "%02u:%02u:%02u.%03u  %-8s %s\n", st.wHour, st.wMinute,
                     st.wSecond, st.wMilliseconds, quelle, text.c_str());
        std::fflush(datei_);
    }

    // Rohe Textbytes der SAROO, an Zeilenenden zerlegt.
    void Text(const std::string& roh) {
        for (char c : roh) {
            if (c == '\r') continue;
            if (c == '\n') {
                if (!rest_.empty()) Zeile("SAROO", rest_);
                rest_.clear();
                continue;
            }
            const unsigned char u = (unsigned char)c;
            if (u >= 32 && u < 127) rest_ += c;
            else {
                char t[8];
                std::snprintf(t, sizeof(t), "<%02X>", u);
                rest_ += t;
            }
            if (rest_.size() > 400) { Zeile("SAROO", rest_); rest_.clear(); }
        }
    }

private:
    bool Offen() {
        if (datei_) return true;
        if (versucht_) return false;
        versucht_ = true;
        char pfad[MAX_PATH];
        const DWORD n = ::GetModuleFileNameA(nullptr, pfad, MAX_PATH);
        std::string p(pfad, n);
        const size_t schr = p.find_last_of("\\/");
        p = (schr == std::string::npos ? std::string() : p.substr(0, schr + 1)) +
            "saroo_aufzeichnung.txt";
        datei_ = std::fopen(p.c_str(), "ab");
        if (datei_) {
            std::fprintf(datei_, "\n===== %s =====\n", buildinfo::string());
            std::fflush(datei_);
        }
        return datei_ != nullptr;
    }

    std::mutex sperre_;
    FILE* datei_ = nullptr;
    bool versucht_ = false;
    std::string rest_;           // nur der Lese-Thread
};

Aufzeichnung g_aufzeichnung;

}  // namespace

Connection::~Connection() { Beenden(); }

void Connection::Melden(std::string text, app::Rang rang) {
    std::lock_guard<std::mutex> g(sperre_);
    ausgabe_.push_back({std::move(text), rang});
}

bool Connection::Starten(const std::string& anschluss, int baud, std::string* fehler) {
    Beenden();
    if (anschluss.empty()) {
        if (fehler) *fehler = T("Kein Anschluss gewählt.");
        return false;
    }

    // Einmal hier oeffnen, damit ein Fehler sofort und sichtbar zurueckkommt
    // statt still im Thread zu verschwinden.
    {
        core::SerialPort probe;
        if (!probe.Oeffnen(anschluss, baud, fehler)) return false;
    }

    abbruch_ = false;
    laeuft_ = true;
    getrennt_gemeldet_ = false;
    jemals_verbunden_ = true;
    thread_ = std::thread(&Connection::Schleife, this, anschluss, baud);
    return true;
}

void Connection::Beenden() {
    abbruch_ = true;
    if (thread_.joinable()) thread_.join();
    laeuft_ = false;
}

void Connection::HookSenden(const std::string& kennung, const std::string& inhalt) {
    if (!laeuft_.load()) return;
    std::lock_guard<std::mutex> g(sperre_);
    if (hook_auftrag_) return;              // einer nach dem anderen
    hook_kennung_ = kennung;
    hook_inhalt_ = inhalt;
    hook_auftrag_ = true;
    hook_sendet_ = true;
}

void Connection::StandAuftrag(uint8_t vorgang, uint8_t slot) {
    if (!laeuft_.load()) return;
    std::lock_guard<std::mutex> g(sperre_);
    stand_vorgang_ = vorgang;
    stand_slot_ = slot;
    stand_auftrag_ = true;
}

void Connection::GoldblitzStarten() {
    if (!laeuft_.load()) return;
    blitz_start_ = ::GetTickCount() | 1u;    // nie 0, das heisst „keiner"
}

void Connection::GruppeUebernehmen(long frames_ok, long crc_fehler) {
    // Gemeldet wird erst, wenn sich der Bildzaehler bewegt hat.
    //
    // Der Inhalt der Mailbox samt Kennung uebersteht den Reset-Knopf. Nach
    // einem Neustart des Spiels schickt die Firmware deshalb weiter den alten
    // Stand, auch wenn fuer das laufende Spiel gar kein Hook installiert ist.
    // Ein bewegter Bildzaehler beweist, dass der Hook in diesem Lauf wirklich
    // laeuft; ein gerade geschriebener Hook-Satz wirkt erst beim naechsten
    // Spielstart.
    const bool zaehler_bewegt = zaehler_bekannt_ && sammel_zaehler_ != letzter_zaehler_;
    if (!erste_werte_gemeldet_ && zaehler_bewegt) {
        erste_werte_gemeldet_ = true;
        char t[160];
        std::snprintf(t, sizeof(t),
                      T("Hook liefert Daten: %d Einträge im ersten Bericht."),
                      (int)sammler_.size());
        Melden(t, app::Rang::Gut);
    }

    std::lock_guard<std::mutex> g(sperre_);

    // Nach einem Spielwechsel zaehlen Berichte erst, wenn sich ihr Bildzaehler
    // bewegt. Die Mailbox haelt den Hook-Satz des vorigen Spiels, die STM32
    // meldet dessen eingefrorene Werte weiter. Uebernommen, gaelte der Hook als
    // vorhanden, und fuer das neue Spiel ginge nie ein Hook-Satz hinaus.
    if (nach_wechsel_) {
        if (!wechsel_zaehler_bekannt_) {
            wechsel_zaehler_ = sammel_zaehler_;
            wechsel_zaehler_bekannt_ = true;
            return;
        }
        if (sammel_zaehler_ == wechsel_zaehler_) return;
        nach_wechsel_ = false;
    }

    // Hat sich irgendein beobachteter Wert geaendert? Das beantwortet die
    // eigentliche Frage „lebt der Hook noch" besser als jede Rate.
    if (letzte_.size() != sammler_.size()) {
        letzte_aenderung_ = ::GetTickCount();
    } else {
        for (size_t i = 0; i < sammler_.size(); ++i)
            if (sammler_[i].value != letzte_[i].value) {
                letzte_aenderung_ = ::GetTickCount();
                break;
            }
    }
    // Diagnose: Wechselt die Zahl der Eintraege zwischen zwei Berichten, ist
    // etwas faul, denn der Hook-Satz legt sie fest. Hoechstens 20 Meldungen.
    if (!letzte_.empty() && sammler_.size() != letzte_.size() && groesse_gemeldet_ < 20) {
        ++groesse_gemeldet_;
        char t[200];
        std::snprintf(t, sizeof(t),
                      T("Bericht mit %d statt %d Einträgen, Bildzähler %08X, erste Adresse %08X."),
                      (int)sammler_.size(), (int)letzte_.size(), (unsigned)sammel_zaehler_,
                      sammler_.empty() ? 0u : (unsigned)sammler_.front().addr);
        ausgabe_.push_back({t, app::Rang::Warnung});
    }

    // Kleiner als der vorige heisst fast immer: Ein Teil-Frame ging verloren
    // (CRC-Fehler). So eine Gruppe wird verworfen, statt halbe Werte zu zeigen
    // und auszuwerten.
    // Bleibt die Gruppe dreimal hintereinander kleiner, ist das der neue Stand,
    // etwa ein anderes Spiel mit weniger Watch-Adressen.
    if (!letzte_.empty() && sammler_.size() < letzte_.size()) {
        if (++kleiner_folge_ < 3) return;
    }
    kleiner_folge_ = 0;
    vorletzte_ = letzte_;
    letzte_ = sammler_;
    bildzaehler_ = sammel_zaehler_;
    daten_ = true;
    frames_ok_ = frames_ok;
    // Abgerissene Rahmen durch einen Neustart zaehlen nicht als Fehler.
    crc_fehler_ = crc_fehler - crc_neustart_;
    if (crc_fehler_ < 0) crc_fehler_ = 0;
    ++watch_frames_;

    // Neustart desselben Spiels auf der Konsole: Der Bildzaehler faengt wieder
    // klein an. Dann den Stand der Achievements neu von RetroAchievements
    // holen, etwa nach einem Zuruecksetzen auf der Webseite. Ein kleiner Ruecksprung
    // ist ein verlorener Rahmen, deshalb erst ab 64.
    if (zaehler_bekannt_ && sammel_zaehler_ + 64 < letzter_zaehler_)
        spiel_neustart_ = true;

    if (zaehler_bekannt_ && sammel_zaehler_ == letzter_zaehler_) {
        if (stillstand_ < kStillstandSchwelle + 1) ++stillstand_;
    } else {
        stillstand_ = 0;
        zaehler_wechsel_ = ::GetTickCount();
    }
    letzter_zaehler_ = sammel_zaehler_;
    zaehler_bekannt_ = true;
}

// Aenderungsprotokoll. Die Datensaetze eines Bildes kommen
// hintereinander, abgeschlossen von einem Satz mit Eintrag kProtBildEnde. Erst
// dann ist das Bild vollstaendig und geht an die Auswertung.
//
// Umgesetzt auf Adressen wird mit der Adressliste aus dem letzten
// WATCH_REPORT: Eintrag i ist dort der i-te. Ohne Liste kein Bild, und das
// naechste gilt als Bild nach einer Luecke.
void Connection::ProtSaetze(bool luecke, const std::vector<core::ProtSatz>& saetze) {
    if (luecke) {
        prot_luecke_ = true;
        prot_offen_.clear();
    }
    for (const core::ProtSatz& s : saetze) {
        if (s.eintrag != core::kProtBildEnde) {
            prot_offen_.push_back({s.eintrag, s.wert});
            // Reissleine: mehr als ein Bild je Eintrag ohne Abschluss ist kaputt.
            if (prot_offen_.size() > (size_t)core::kFirmwareMaxEintraege * 2) {
                prot_offen_.clear();
                prot_luecke_ = true;
            }
            continue;
        }
        const uint32_t bild = s.wert;
        // Bild 1 ist ein Neustart des Spiels und bringt ohnehin alle Werte.
        if (bild != 1 && prot_bild_bekannt_ && bild != prot_letztes_bild_ + 1)
            prot_luecke_ = true;
        prot_letztes_bild_ = bild;
        prot_bild_bekannt_ = true;

        std::lock_guard<std::mutex> g(sperre_);
        prot_zuletzt_ = ::GetTickCount();
        if (letzte_.empty()) {
            prot_luecke_ = true;
            prot_offen_.clear();
            continue;
        }
        ProtBild b;
        b.bild = bild;
        b.luecke = prot_luecke_;
        if (prot_luecke_) ++prot_luecken_;
        if (prot_werte_.size() != letzte_.size()) prot_werte_.assign(letzte_.size(), 0);
        for (const auto& e : prot_offen_) {
            if (e.first >= letzte_.size()) continue;
            b.werte.push_back({letzte_[e.first].addr, e.second});
            prot_werte_[e.first] = e.second;
        }
        prot_offen_.clear();
        prot_luecke_ = false;
        // Holt die Oberflaeche nicht ab (Fenster verdeckt, Programm haengt),
        // nicht endlos sammeln: zehn Sekunden, danach Luecke.
        if (prot_fertig_.size() >= 600) {
            prot_fertig_.clear();
            b.luecke = true;
            ++prot_luecken_;
        }
        prot_fertig_.push_back(std::move(b));
    }
}

std::vector<Connection::ProtBild> Connection::ProtokollAbholen() {
    std::lock_guard<std::mutex> g(sperre_);
    std::vector<ProtBild> aus;
    aus.swap(prot_fertig_);
    return aus;
}

bool Connection::ProtokollAktiv() {
    std::lock_guard<std::mutex> g(sperre_);
    return prot_zuletzt_ != 0 && ::GetTickCount() - prot_zuletzt_ < 2000;
}

// Spielerkennung. Sie haengt an keinem Hook und an keiner Config-Zeile: Die
// Firmware liest den Disc-Kopf beim Laden des Abbilds ohnehin von der SD-Karte
// und schickt ihn seither mit. Erst wenn alle vier Teile da sind, entstehen
// daraus Titel, Kennung, Gebiet und RA-Pruefsumme -- eine halbe Pruefsumme
// waere eine falsche.
void Connection::GameInfoTeil(int offset, const std::vector<uint8_t>& daten) {
    if (!kopf_.Fuettern(offset, daten)) return;
    if (!kopf_.Vollstaendig()) return;

    core::DiscKopf dk;
    if (!core::DeuteDiscKopf(kopf_.Bytes(), &dk)) return;
    const std::string md5 = core::HashAusKopf(kopf_.Bytes());

    std::lock_guard<std::mutex> g(sperre_);
    if (dk.kennung == kennung_ && md5 == md5_) return;
    // Anderes Spiel: Werte und Hook-Zustand des vorigen verwerfen, siehe
    // nach_wechsel_ in GruppeUebernehmen.
    nach_wechsel_ = true;
    wechsel_zaehler_bekannt_ = false;
    daten_ = false;
    letzte_.clear();
    vorletzte_.clear();
    prot_werte_.clear();
    prot_fertig_.clear();
    prot_offen_.clear();
    prot_luecke_ = true;
    prot_zeit_da_ = false;
    stillstand_ = 0;
    zaehler_bekannt_ = false;
    erste_werte_gemeldet_ = false;
    // Auch die Proben der Bildrate: Der Sprung vom Zaehlerstand des vorigen
    // Spiels auf den des neuen erschien sonst als Millionen Bilder je Sekunde.
    rate_anzahl_ = 0;
    rate_kopf_ = 0;
    bilder_pro_sekunde_ = 0.0;
    // Speicherstaende gehoeren zum Spiel: Uebersicht des vorigen verwerfen.
    stand_bekannt_ = false;
    stand_neu_ = true;
    kennung_ = dk.kennung;
    titel_ = dk.titel;
    gebiete_ = dk.gebiete;
    md5_ = md5;
    kennung_neu_ = true;
}

void Connection::Schleife(std::string anschluss, int baud) {
    core::SerialPort port;
    std::string fehler;
    if (!port.Oeffnen(anschluss, baud, &fehler)) {
        Melden(fehler, app::Rang::Warnung);
        laeuft_ = false;
        return;
    }

    {
        char t[128];
        std::snprintf(t, sizeof(t), T("Verbunden mit %s @ %d"), anschluss.c_str(), baud);
        Melden(t, app::Rang::Gut);
    }

    core::FrameParser parser;
    uint8_t puffer[4096];
    std::string roh_anfang;
    DWORD letzte_rate = ::GetTickCount();
    DWORD letzter_status = letzte_rate;
    rate_anzahl_ = 0;
    rate_kopf_ = 0;
    letzte_aenderung_ = 0;
    const DWORD begonnen = ::GetTickCount();

    // Jeder Zustandswechsel wird GENAU EINMAL gemeldet; eine laufend
    // wiederholte Byte-Zahl wuerde das Protokoll nur fuellen.
    bool gemeldet_stumm = false;
    bool gemeldet_saroo = false;
    bool gemeldet_fremd = false;

    {
        std::lock_guard<std::mutex> g(sperre_);
        geraet_ = app::State::Geraet::PortOffen;
        hinweis_ = TK("Anschluss offen, warte auf das Gerät.");
        rohbytes_ = 0;
    }
    sammler_.clear();
    sammel_bekannt_ = false;
    zaehler_bekannt_ = false;
    erste_werte_gemeldet_ = false;
    crc_gemeldet_ = 0;
    { std::lock_guard<std::mutex> g(sperre_); groesse_gemeldet_ = 0; }
    kopf_.Zuruecksetzen();
    { std::lock_guard<std::mutex> g(sperre_); watch_frames_ = 0; }

    while (!abbruch_.load()) {
        // Waehrend eines Goldblitzes kuerzer warten, damit die Stufen im Takt
        // rausgehen, auch wenn gerade keine Frames kommen.
        const int n = port.Lesen(puffer, sizeof(puffer), blitz_start_.load() ? 10 : 100);
        if (n < 0) {
            Melden(T("Serieller Fehler, Verbindung verloren."), app::Rang::Warnung);
            break;
        }
        if (n > 0) {
            {
                std::lock_guard<std::mutex> g(sperre_);
                rohbytes_ += n;
            }
            if (parser.frames_ok == 0 && roh_anfang.size() < 200)
                roh_anfang += Lesbar(puffer, n);

            const std::vector<core::Frame> neue_frames = parser.Fuettern(puffer, (size_t)n);
            if (!parser.klartext.empty()) {
                g_aufzeichnung.Text(parser.klartext);
                KlartextStrom(parser.klartext);
                parser.klartext.clear();
            }
            for (const std::string& probe : parser.crc_proben)
                g_aufzeichnung.Zeile("CRC", probe);

            // Kaputte Rahmen mit ihren Rohbytes ins Protokoll, damit die Ursache
            // sichtbar wird (Klartext dazwischen, alte Bytes, gekippte Bits).
            // Hoechstens 20 je Verbindung, sonst flutet eine gestoerte Leitung
            // das Protokoll.
            for (const std::string& probe : parser.crc_proben) {
                // Ein Neustart des Geraets zerreisst den Rahmen, der gerade
                // unterwegs ist. Das ist eine Zustandsaenderung, kein
                // Uebertragungsfehler.
                // Gesucht wird nur der Anfang des Banners. Der Rohbytes-Auszug
                // bricht nach 48 Bytes ab; faengt der Klartext erst gegen Ende
                // an, steht dort nur noch "SSMa".
                if (probe.find("SSMa") != std::string::npos) {
                    ++crc_neustart_;
                    Melden(T("Gerät neu gestartet. Der laufende Frame ist dabei "
                             "abgerissen, das ist kein Fehler."));
                    continue;
                }
                if (crc_gemeldet_ >= 20) continue;
                ++crc_gemeldet_;
                Melden(T("CRC-Fehler, Rohbytes: ") + probe, app::Rang::Warnung);
            }
            parser.crc_proben.clear();

            for (const core::Frame& f : neue_frames) {
                if (f.type == core::kTypeHeartbeat) continue;
                if (f.type == core::kTypeHello) {
                    uint8_t v = 0; std::string s;
                    if (f.LiesHello(&v, &s)) {
                        char t[128];
                        std::snprintf(t, sizeof(t), T("HELLO, Protokollversion %u, Spiel %s"),
                                      (unsigned)v, s.c_str());
                        Melden(t);
                    }
                    continue;
                }
                if (f.type == core::kTypeHookAck) {
                    uint8_t auf = 0, status = 0;
                    if (!core::LiesHookAck(f, &auf, &status)) continue;
                    if (auf == core::kTypeHookEnd) {
                        std::lock_guard<std::mutex> g(sperre_);
                        hook_auftrag_ = false;
                        hook_sendet_ = false;
                        if (status == 0) {
                            // Der Satz wirkt SOFORT beim nächsten Start dieses
                            // Spiels, nicht erst beim übernächsten: Die
                            // Firmware schreibt ihn beim Laden auf die Karte
                            // und liest ihn im selben Durchgang wieder ein,
                            // bevor sie das Spiel patcht.
                            ausgabe_.push_back(
                                {T("SAROO hat den Hook-Satz angenommen. Er kommt auf "
                                   "die Karte und ist ab dem nächsten Start dieses "
                                   "Spiels aktiv."), app::Rang::Gut});
                        } else {
                            const char* grund =
                                status == 1 ? T("zu groß für den Gerätepuffer")
                              : status == 2 ? T("Längen stimmen nicht überein")
                              : status == 3 ? T("Prüfsumme stimmt nicht")
                                            : T("unbekannter Grund");
                            ausgabe_.push_back(
                                {std::string(T("SAROO hat den Hook-Satz abgelehnt: ")) +
                                 grund + ".", app::Rang::Warnung});
                        }
                    } else if (auf == core::kTypeHookBegin && status != 0) {
                        std::lock_guard<std::mutex> g(sperre_);
                        hook_auftrag_ = false;
                        hook_sendet_ = false;
                        ausgabe_.push_back({T("SAROO lehnt den Hook-Satz ab: zu groß "
                                              "für den Gerätepuffer."), app::Rang::Warnung});
                    } else if (auf == core::kTypeFileBegin ||
                               auf == core::kTypeFileData ||
                               auf == core::kTypeFileEnd) {
                        DateiAck(auf, status);
                    }
                    continue;
                }
                if (f.type == core::kTypeStandAntw) {
                    core::StandAntwort a;
                    if (!core::LiesStandAntwort(f, &a)) continue;
                    std::lock_guard<std::mutex> g(sperre_);
                    // Die Antwort nennt keinen Slot. Bei einem Auftrag vom PC
                    // ist es der beauftragte; ohne Auftrag (L + R + Unten am
                    // Pad) oder bei "naechster Schnellstand" der neueste X.
                    int slot = stand_gesendet_ ? (int)stand_slot_ : (int)core::kStandXNeu;
                    if (slot == core::kStandXNeu) {
                        int beste = -1;
                        for (int s = core::kStandSlotX0; s < core::kStandSlots; ++s)
                            if (a.slots[s].belegt &&
                                (beste < 0 || a.slots[s].folge > a.slots[beste].folge))
                                beste = s;
                        slot = beste;
                    }
                    std::string sn = "?";
                    if (slot >= 0 && slot < core::kStandSlotX0)
                        sn = std::to_string(slot + 1);
                    else if (slot >= core::kStandSlotX0 && slot < core::kStandSlots)
                        sn = "X." + std::to_string(a.slots[slot].folge);
                    stand_gesendet_ = 0;
                    stand_antwort_ = a;
                    stand_bekannt_ = true;
                    stand_neu_ = true;
                    std::string text;
                    app::Rang rang = app::Rang::Gut;
                    if (a.vorgang == core::kStandSlotsLesen) {
                        continue;                       // nur die Uebersicht
                    } else if (a.vorgang == core::kStandMenueNicht) {
                        if (a.ergebnis == core::kStandErgGesperrt)
                            text = T("Ingame-Menü im Hardcore-Modus gesperrt.");
                        else
                            text = T("Das Ingame-Menü ist in diesem Spiel nicht möglich. "
                                     "Speichern mit L + R + Unten oder hier im Werkzeug.");
                        rang = app::Rang::Normal;
                    } else if (a.ergebnis == core::kStandErgOk) {
                        text = a.vorgang == core::kStandSpeichern
                                   ? std::string(T("Gespeichert in Slot ")) + sn + "."
                                   : std::string(T("Slot ")) + sn + T(" geladen.");
                    } else {
                        rang = app::Rang::Warnung;
                        const char* grund =
                            a.ergebnis == core::kStandErgLeer     ? T("der Slot ist leer")
                          : a.ergebnis == core::kStandErgGesperrt ? T("im Hardcore-Modus gesperrt")
                          : a.ergebnis == core::kStandErgFehler   ? T("Fehler beim Zugriff auf die Karte")
                                                                  : T("unbekannter Auftrag");
                        text = std::string(a.vorgang == core::kStandSpeichern
                                               ? T("Speichern in Slot ") : T("Laden aus Slot ")) +
                               sn + T(" ging nicht: ") + grund + ".";
                    }
                    ausgabe_.push_back({text, rang});
                    continue;
                }
                if (f.type == core::kTypeProt) {
                    bool luecke = false;
                    std::vector<core::ProtSatz> saetze;
                    if (core::LiesProt(f, &luecke, &saetze)) ProtSaetze(luecke, saetze);
                    continue;
                }
                if (f.type == core::kTypeProtZeit) {
                    uint32_t d = 0, m = 0;
                    if (core::LiesProtZeit(f, &d, &m)) {
                        std::lock_guard<std::mutex> g(sperre_);
                        prot_dauer_us_ = d;
                        prot_max_us_ = m;
                        prot_zeit_da_ = true;
                    }
                    continue;
                }
                if (f.type == core::kTypeGameInfo) {
                    // Spielerkennung. Kommt von der Firmware aus, ohne Hook und
                    // ohne Zutun -- auch dann, wenn fuer dieses Spiel gar nichts
                    // eingerichtet ist.
                    int off = 0;
                    std::vector<uint8_t> d;
                    if (f.LiesGameInfo(&off, &d)) GameInfoTeil(off, d);
                    continue;
                }
                if (!f.IstWatchReport()) continue;

                uint32_t zaehler = 0;
                std::vector<core::WatchEntry> teil;
                if (!f.LiesWatchReport(&zaehler, &teil)) continue;

                // Ein Bericht kann aus mehreren Frames bestehen.
                //
                // Ein Frame fasst nur 27 Eintraege (LEN<=250 bei 9 Byte je
                // Eintrag). Ein Achievement-Set mit mehr Watch-Adressen kommt
                // deshalb in mehreren Frames, die alle denselben FRAME_COUNT
                // tragen. Wer jeden Frame einzeln auswertet, sieht immer nur
                // einen Teil der Werte.
                //
                // Zusammengesetzt wird nach FRAME_COUNT. Wechselt er, ist die
                // vorige Gruppe vollstaendig. Schickt die Firmware denselben
                // Schnappschuss erneut (Drosselbetrieb, siehe
                // RA_STALE_PING_POLLS), faengt sie wieder beim ersten Eintrag
                // an -- daran wird der Neustart erkannt.
                const bool neuer_zaehler = !sammel_bekannt_ || zaehler != sammel_zaehler_;
                const bool wiederholung =
                    !neuer_zaehler && !sammler_.empty() && !teil.empty() &&
                    teil.front().addr == sammler_.front().addr;

                if (neuer_zaehler || wiederholung) {
                    if (!sammler_.empty())
                        GruppeUebernehmen(parser.frames_ok, parser.frames_crc_fehler);
                    sammler_.clear();
                    sammel_zaehler_ = zaehler;
                    sammel_bekannt_ = true;
                }
                if ((int)sammler_.size() + (int)teil.size() <= core::kFirmwareMaxEintraege)
                    sammler_.insert(sammler_.end(), teil.begin(), teil.end());
            }
        }

        // Wartet ein Hook-Satz, geht er jetzt raus. Hier und nicht aus dem
        // Zeichen-Thread, damit kein zweiter Schreiber zwischen die Frames
        // gerät. Die Bestätigung kommt später als HOOK_ACK zurück.
        {
            std::string kennung, inhalt;
            {
                std::lock_guard<std::mutex> g(sperre_);
                if (hook_auftrag_ && !hook_inhalt_.empty()) {
                    kennung = hook_kennung_;
                    inhalt = hook_inhalt_;
                    hook_inhalt_.clear();     // nur einmal senden, Auftrag bleibt
                }                              // offen bis zum ACK
            }
            if (core::g_nur_zuhoeren) kennung.clear();   // Messschalter
            if (!kennung.empty()) {
                const std::vector<std::vector<uint8_t>> rahmen =
                    core::BaueHookUebertragung(kennung, inhalt);
                if (rahmen.empty()) {
                    std::lock_guard<std::mutex> g(sperre_);
                    hook_auftrag_ = false;
                    hook_sendet_ = false;
                    ausgabe_.push_back({T("Hook-Satz ist zu groß zum Übertragen."),
                                        app::Rang::Warnung});
                } else {
                    bool ok = true;
                    for (const std::vector<uint8_t>& r : rahmen)
                        if (!port.Schreiben(r.data(), (int)r.size())) { ok = false; break; }
                    std::lock_guard<std::mutex> g(sperre_);
                    if (!ok) {
                        hook_auftrag_ = false;
                        hook_sendet_ = false;
                        ausgabe_.push_back({T("Hook-Satz konnte nicht gesendet werden."),
                                            app::Rang::Warnung});
                    } else {
                        char t[160];
                        std::snprintf(t, sizeof(t),
                            T("Hook-Satz an die SAROO gesendet: %d Bytes in %d Teilen."),
                            (int)inhalt.size(), (int)rahmen.size());
                        ausgabe_.push_back({t, app::Rang::Normal});
                    }
                }
            }
        }

        // Uhrzeit des PC an das Geraet, alle zehn Sekunden. Die Saturn nimmt
        // sie fuer Datum und Uhrzeit eines Speicherstands.
        {
            static unsigned long uhr_zuletzt = 0;
            const unsigned long jetzt_uhr = ::GetTickCount();
            if (!core::g_nur_zuhoeren &&
                (uhr_zuletzt == 0 || jetzt_uhr - uhr_zuletzt >= 10000)) {
                const std::vector<uint8_t> r = core::BaueUhrJetzt();
                if (port.Schreiben(r.data(), (int)r.size())) uhr_zuletzt = jetzt_uhr | 1u;
            }
        }

        // Speicherstand-Auftrag. Ebenfalls nur hier gesendet.
        {
            bool senden = false;
            uint8_t vorgang = 0, slot = 0;
            {
                std::lock_guard<std::mutex> g(sperre_);
                if (stand_auftrag_) {
                    stand_auftrag_ = false;
                    senden = true;
                    vorgang = stand_vorgang_;
                    slot = stand_slot_;
                }
            }
            if (senden && !core::g_nur_zuhoeren) {
                const std::vector<uint8_t> r = core::BaueStand(vorgang, slot);
                const bool ok = port.Schreiben(r.data(), (int)r.size());
                std::lock_guard<std::mutex> g(sperre_);
                stand_gesendet_ = ok ? (::GetTickCount() | 1u) : 0;
                if (!ok)
                    ausgabe_.push_back({T("Speicherstand-Auftrag konnte nicht gesendet "
                                          "werden."), app::Rang::Warnung});
            }
        }

        // Dateitransfer. Ebenfalls hier, aus demselben Grund. Er zieht sich
        // ueber viele Durchlaeufe, weil nach jedem Block die Quittung des
        // Geraets abgewartet wird.
        DateiSchritt(port);

        // Goldblitz. Hier im Lese-Thread wie der Hook-Satz, damit kein zweiter
        // Schreiber zwischen die Frames geraet. Liegt der Thread hinter dem
        // Takt, geht nur die aktuelle Stufe raus; die letzte (0) immer.
        {
            unsigned long start = blitz_start_.load();
            if (core::g_nur_zuhoeren) start = 0;         // Messschalter
            if (start != 0) {
                static const std::vector<uint16_t> folge = core::GoldblitzFolge();
                if (start != blitz_lauf_) { blitz_lauf_ = start; blitz_gesendet_ = -1; }
                const int letzte = (int)folge.size() - 1;
                int schritt = (int)((::GetTickCount() - start) / core::kGoldblitzSchrittMs);
                if (schritt > letzte) schritt = letzte;
                if (schritt > blitz_gesendet_) {
                    const std::vector<uint8_t> r = core::BaueTvBlitz(folge[(size_t)schritt]);
                    port.Schreiben(r.data(), (int)r.size());
                    blitz_gesendet_ = schritt;
                }
                if (schritt == letzte) blitz_start_.compare_exchange_strong(start, 0ul);
            }
        }

        const DWORD jetzt = ::GetTickCount();

        // Bildrate, gemittelt ueber die letzten fuenf Sekunden. Der Hook laeuft
        // schubweise -- er haengt am V-Blank-OUT-Interrupt, und den sperren
        // manche Spiele ueber laengere Strecken. Ueber nur eine Sekunde
        // gemittelt, zeigte eine Sekunde ohne Bericht 0 und die naechste einen
        // Ausreisser mit dem ganzen aufgelaufenen Rest.
        // Statuszeile fuer die Aufzeichnung, alle fuenf Sekunden. Zeigt auch
        // dann einen Verlauf, wenn von der SAROO gar nichts mehr kommt.
        if (jetzt - letzter_status >= 5000) {
            letzter_status = jetzt;
            long bytes, berichte;
            uint32_t zaehler;
            {
                std::lock_guard<std::mutex> g(sperre_);
                bytes = rohbytes_;
                berichte = watch_frames_;
                zaehler = bildzaehler_;
            }
            char t[200];
            std::snprintf(t, sizeof(t),
                          "Bytes %ld  Rahmen ok %ld  CRC %ld  Berichte %ld  Bildzaehler %08X",
                          bytes, parser.frames_ok, parser.frames_crc_fehler, berichte,
                          (unsigned)zaehler);
            g_aufzeichnung.Zeile("STATUS", t);
        }

        if (jetzt - letzte_rate >= 1000) {
            std::lock_guard<std::mutex> g(sperre_);
            if (daten_) {
                rate_proben_[rate_kopf_] = {jetzt, bildzaehler_};
                rate_kopf_ = (rate_kopf_ + 1) % kRateProben;
                if (rate_anzahl_ < kRateProben) ++rate_anzahl_;
                if (rate_anzahl_ >= 2) {
                    const int neu = (rate_kopf_ + kRateProben - 1) % kRateProben;
                    const int alt = (rate_kopf_ + kRateProben - rate_anzahl_) % kRateProben;
                    const DWORD dt = rate_proben_[neu].zeit - rate_proben_[alt].zeit;
                    const uint32_t db = rate_proben_[neu].bild - rate_proben_[alt].bild;
                    if (dt > 0) bilder_pro_sekunde_ = (double)db * 1000.0 / (double)dt;
                }
            }
            letzte_rate = jetzt;
        }

        // Zustand bestimmen. Jede Stufe wird genau einmal gemeldet.
        //
        // Entscheidend ist die Zahl der WATCH_REPORTs, nicht die aller Frames.
        // Seit die Firmware die Spielerkennung von sich aus schickt, kommen
        // auch ohne jeden Hook Frames an. Wer hier alle zaehlt, haelt ein
        // erkanntes Spiel faelschlich fuer einen laufenden Hook.
        long watch_frames = 0;
        { std::lock_guard<std::mutex> g(sperre_); watch_frames = watch_frames_; }

        if (watch_frames == 0) {
            const bool saroo = SarooKlartext(roh_anfang) || parser.frames_ok > 0;
            long bytes = 0;
            bool erkannt = false;
            { std::lock_guard<std::mutex> g(sperre_); bytes = rohbytes_; erkannt = !kennung_.empty(); }

            if (saroo && !gemeldet_saroo && jetzt - begonnen >= 4000) {
                gemeldet_saroo = true;
                std::lock_guard<std::mutex> g(sperre_);
                geraet_ = app::State::Geraet::SarooErkannt;
                probe_ = roh_anfang.substr(0, 120);
                hinweis_ = erkannt
                    ? TK("SAROO meldet sich und das Spiel ist erkannt, aber es kommen "
                         "keine Watch-Werte: für dieses Spiel ist kein Hook aktiv. "
                         "Entweder hat es kein Achievement-Set, oder das Spiel wurde "
                         "seit dem Einrichten nicht neu gestartet — die M_-Zeilen "
                         "wirken erst beim nächsten Spielstart.")
                    : TK("SAROO erkannt, sie meldet sich. Es kommen aber keine "
                         "Watch-Werte: für das laufende Spiel ist kein Hook "
                         "installiert. Dafür unter „Firmware und Spiele“ den "
                         "ROM-Ordner einrichten.");
                // hinweis_ bleibt ein deutscher Schluessel (uebersetzt wird bei
                // der Anzeige), die Protokollzeile wird gleich uebersetzt.
                ausgabe_.push_back({T(hinweis_.c_str()), app::Rang::Warnung});
            } else if (!saroo && bytes > 0 && !gemeldet_fremd &&
                       jetzt - begonnen >= 6000) {
                gemeldet_fremd = true;
                std::lock_guard<std::mutex> g(sperre_);
                geraet_ = app::State::Geraet::PortOffen;
                probe_ = roh_anfang.substr(0, 120);
                hinweis_ = TK("Es kommen Daten an, aber sie sehen nicht nach SAROO aus. "
                              "Stimmt der Anschluss und die Rate 1000000?");
                // hinweis_ bleibt ein deutscher Schluessel (uebersetzt wird bei
                // der Anzeige), die Protokollzeile wird gleich uebersetzt.
                ausgabe_.push_back({T(hinweis_.c_str()), app::Rang::Warnung});
            } else if (bytes == 0 && !gemeldet_stumm && jetzt - begonnen >= 12000) {
                // Erst nach 12 Sekunden meckern. Die SAROO redet nur bei
                // Plattenzugriffen; in einem Menue kann sie lange still sein.
                gemeldet_stumm = true;
                std::lock_guard<std::mutex> g(sperre_);
                geraet_ = app::State::Geraet::Stumm;
                hinweis_ = TK("Seit 12 Sekunden kommt nichts. Das kann normal sein, die "
                              "SAROO meldet sich nur bei Plattenzugriffen. Bleibt es "
                              "still: Anschluss, Rate 1000000, Verkabelung G an G, "
                              "RXD an T, TXD an R, Konsole eingeschaltet prüfen.");
                // hinweis_ bleibt ein deutscher Schluessel (uebersetzt wird bei
                // der Anzeige), die Protokollzeile wird gleich uebersetzt.
                ausgabe_.push_back({T(hinweis_.c_str()), app::Rang::Warnung});
            }
        } else if (gemeldet_saroo && erste_werte_gemeldet_) {
            // Der Hook hat doch angefangen zu liefern. Die zuvor gemeldete
            // Stufe wird ausdruecklich zurueckgenommen, statt als Warnung
            // stehenzubleiben und zu widersprechen.
            //
            // Gekoppelt an erste_werte_gemeldet_, also an einen bewegten
            // Bildzaehler: Ein stehengebliebener Mailbox-Inhalt aus dem
            // vorigen Lauf ist kein laufender Hook (siehe
            // GruppeUebernehmen).
            gemeldet_saroo = false;
            std::lock_guard<std::mutex> g(sperre_);
            hinweis_.clear();
            ausgabe_.push_back({T("Hook liefert jetzt Watch-Werte."), app::Rang::Gut});
        }
    }

    port.Schliessen();
    laeuft_ = false;
    if (!getrennt_gemeldet_) {
        getrennt_gemeldet_ = true;
        Melden(T("Verbindung getrennt."));
    }
}

void Connection::Abholen(app::State& state) {
    std::lock_guard<std::mutex> g(sperre_);

    // Der laufende Firmware-Stand gilt unabhaengig davon, ob Frames ankommen:
    // Gerade wenn keine kommen, ist die Frage wichtig, welcher Stand laeuft.
    state.firmware = firmware_;

    // Speicherstaende. Zeitgrenze fuer die Antwort: Speichern dauert einige
    // Sekunden (2,6 MByte auf die Karte); nach 60 s gilt der Auftrag als
    // verloren, damit die Knoepfe nicht fuer immer gesperrt bleiben.
    if (stand_gesendet_ && ::GetTickCount() - stand_gesendet_ > 60000) {
        stand_gesendet_ = 0;
        ausgabe_.push_back({T("Keine Antwort der Konsole auf den Speicherstand-"
                              "Auftrag."), app::Rang::Warnung});
    }
    state.stand_laeuft = stand_auftrag_ || stand_gesendet_ != 0;
    if (spiel_neustart_) {
        spiel_neustart_ = false;
        state.spiel_neustart = true;
    }
    if (stand_neu_) {
        stand_neu_ = false;
        state.stand_bekannt = stand_bekannt_;
        for (int s = 0; s < app::State::kStandSlots; ++s) {
            app::State::StandSlot& z = state.stand_slots[s];
            const core::StandSlot& q = stand_antwort_.slots[s];
            z.belegt = stand_bekannt_ && q.belegt;
            z.folge = z.belegt ? q.folge : 0;
            z.spielzeit.clear();
            if (z.belegt) {
                const unsigned s = q.bilder / (q.pal ? 50u : 60u);
                char t[24];
                std::snprintf(t, sizeof(t), "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
                z.spielzeit = t;
            }
            z.zeit.clear();
            bool uhr_leer = true;
            for (int i = 0; i < 7; ++i) if (q.zeit[i]) uhr_leer = false;
            if (z.belegt && uhr_leer) {
                z.zeit = "--.--.----  --:--";   // Stand ohne Uhrzeit (aeltere Fassung)
            } else if (z.belegt) {
                char t[32];
                const int monat = q.zeit[2] & 0x0f;
                std::snprintf(t, sizeof(t), "%02x.%02x.%02x%02x  %02x:%02x",
                              q.zeit[3], monat / 10 * 16 + monat % 10,
                              q.zeit[0], q.zeit[1], q.zeit[4], q.zeit[5]);
                z.zeit = t;
            }
        }
    }

    for (const app::Logzeile& z : ausgabe_) g_aufzeichnung.Zeile("WERKZEUG", T(z.text.c_str()));
    for (app::Logzeile& z : ausgabe_) state.log.push_back(std::move(z));
    ausgabe_.clear();

    state.verbunden = laeuft_.load();
    if (!state.verbunden) {
        // Getrennt heisst getrennt. Alles, was nur mit laufender Verbindung
        // gilt, verschwindet: Spielname, Kennung, Prüfsumme, Live-Werte. Sonst
        // steht dort weiter ein Spiel, von dem niemand mehr weiss, ob es noch
        // läuft.
        geraet_ = app::State::Geraet::Getrennt;
        // „Verloren" nur, wenn es vorher eine Verbindung gab. Beim Programmstart
        // waere das eine falsche Aussage.
        hinweis_ = jemals_verbunden_
            ? TK("Verbindung verloren. Keine Angaben zum laufenden Spiel.")
            : "";
        kennung_.clear();
        titel_.clear();
        gebiete_.clear();
        md5_.clear();
        kennung_neu_ = false;
        daten_ = false;
        letzte_.clear();
        vorletzte_.clear();

        state.geraet = geraet_;
        state.geraet_hinweis = hinweis_;
        state.saroo_probe.clear();
        state.hook = app::State::Hook::Unbekannt;
        state.daten_empfangen = false;
        state.spiel_erkannt = false;
        state.spiel_name.clear();
        state.spiel_kennung.clear();
        state.spiel_gebiet.clear();
        state.spiel_md5.clear();
        state.ra_kennt_abzug = false;
        state.werte.clear();
        state.saturn_status_da = false;
        state.saturn_status = 0;
        state.bildzaehler = 0;
        state.bilder_pro_sekunde = 0.0f;
        return;
    }
    state.geraet = geraet_;
    state.geraet_hinweis = hinweis_;
    state.saroo_probe = probe_;
    if (!state.verbunden && !daten_) state.hook = app::State::Hook::Unbekannt;

    // Die Spielerkennung wird ZUERST und unabhaengig von allem anderen
    // uebergeben. Sie braucht keinen Hook und keine Watch-Werte: Sobald die
    // Firmware den Disc-Kopf geschickt hat, steht hier, was laeuft.
    if (kennung_neu_) {
        kennung_neu_ = false;
        state.spiel_kennung = kennung_;
        state.spiel_md5 = md5_;
        state.spiel_gebiet = gebiete_;
        state.spiel_erkannt = true;
        state.saturn_status_da = false;
        state.saturn_status = 0;
        // Hook-Zustand und Werte gehoeren zum vorigen Spiel.
        state.hook = app::State::Hook::Unbekannt;
        state.daten_empfangen = false;
        state.werte.clear();
        state.bildzaehler = 0;
        if (!titel_.empty()) state.spiel_name = titel_;
        state.Protokoll(T("Spiel erkannt: ") +
                        (titel_.empty() ? std::string() : titel_ + "  ") +
                        "[" + kennung_ + "]" +
                        (gebiete_.empty() ? "" : T("  Gebiet ") + gebiete_),
                        app::Rang::Wichtig);
        state.Protokoll(T("Prüfsumme aus dem Disc-Kopf gerechnet: ") + md5_);
    }

    if (!daten_) return;

    state.daten_empfangen = true;
    state.bildzaehler = bildzaehler_;
    state.bilder_pro_sekunde = (float)bilder_pro_sekunde_;
    state.sekunden_seit_aenderung = letzte_aenderung_
        ? (float)(::GetTickCount() - letzte_aenderung_) / 1000.0f : -1.0f;
    state.crc_fehler = (int)crc_fehler_;
    const bool steht = stillstand_ >= kStillstandSchwelle ||
                       ::GetTickCount() - zaehler_wechsel_ > kStillstandMs;
    state.hook = steht ? app::State::Hook::Steht : app::State::Hook::Laeuft;
    state.geraet = steht ? app::State::Geraet::HookSteht : app::State::Geraet::HookLaeuft;
    state.geraet_hinweis = steht
        ? TK("Frames kommen an, aber der Bildzähler steht seit über zehn Sekunden. "
             "Solange wird nicht ausgewertet, damit eingefrorene Werte nichts auslösen. "
             "Beim Laden ist das normal.")
        : TK("Hook läuft, Werte sind frisch.");

    // Mit Aenderungsprotokoll stammen die angezeigten Werte von dort: Die
    // Mailbox frischt die Saturn dann nur noch in jeder achten Runde auf.
    const bool prot = prot_zuletzt_ != 0 && ::GetTickCount() - prot_zuletzt_ < 2000;
    state.prot_aktiv = prot;
    state.prot_zeit_da = prot_zeit_da_;
    state.prot_dauer_us = prot_dauer_us_;
    state.prot_max_us = prot_max_us_;
    state.prot_luecken = prot_luecken_;
    if (prot) state.hook = app::State::Hook::Laeuft;

    state.werte.clear();
    state.saturn_status_da = false;
    state.saturn_status = 0;
    for (size_t i = 0; i < letzte_.size(); ++i) {
        const core::WatchEntry& e = letzte_[i];
        if (e.addr == core::kMailboxBasis + core::kStatusOffset) {
            state.saturn_status_da = true;
            state.saturn_status = e.value;
            continue;
        }
        app::Watchwert w;
        w.adresse = e.addr;
        w.wert = e.value;
        w.geaendert = (i < vorletzte_.size() && vorletzte_[i].addr == e.addr)
                          ? vorletzte_[i].value != e.value
                          : false;
        if (prot && i < prot_werte_.size()) {
            w.geaendert = false;
            w.wert = prot_werte_[i];
        }
        state.werte.push_back(w);
    }
}

// ---------------------------------------------------------------------------
// Welcher Firmware-Stand läuft wirklich?
//
// Alles daraus steht in der Klartextausgabe der SAROO, wo es im Strom
// untergeht. Das Werkzeug zeigt deshalb den laufenden Stand an; so faellt auf,
// wenn eine Firmware gar nicht auf der Karte gelandet ist.
// ---------------------------------------------------------------------------

void Connection::KlartextStrom(const std::string& roh) {
    for (char c : roh) {
        if (c == '\r') continue;
        if (c == '\n') {
            if (!klartext_rest_.empty()) KlartextZeile(klartext_rest_);
            klartext_rest_.clear();
            continue;
        }
        klartext_rest_ += c;
        // Reissleine gegen eine Zeile ohne Ende, etwa bei gestoerter Leitung.
        if (klartext_rest_.size() > 400) {
            KlartextZeile(klartext_rest_);
            klartext_rest_.clear();
        }
    }
}

void Connection::KlartextZeile(const std::string& zeile) {
    auto hinter = [&](const char* marke) -> std::string {
        const size_t p = zeile.find(marke);
        if (p == std::string::npos) return {};
        std::string s = zeile.substr(p + std::strlen(marke));
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
        return s;
    };

    // "SSMaster start! 20260918" -- Baudatum der laufenden STM32-App. Kommt bei
    // jedem Einschalten, auch vom Bootloader; der letzte Wert gilt.
    const std::string start = hinter("SSMaster start!");
    if (!start.empty()) {
        {
            std::lock_guard<std::mutex> g(sperre_);
            firmware_.stm32_bau = start;
            // Neustart des Geraets: Alles andere gilt nicht mehr.
            firmware_.ra_patch = false;
            firmware_.ra_bau.clear();
            firmware_.ssfirm_bytes = 0;
            firmware_.hook_paare = -1;
            firmware_.spiel_abschnitt.clear();
        }
        g_aufzeichnung.Zeile("FIRMWARE", "STM32-App gestartet, gebaut " + start);
        return;
    }

    // "ra: hook 22 pairs, mcu Sep 18 2026" -- nur unser Patch schreibt das.
    // Die Zeile beweist zugleich, dass die gepatchte mcuapp.bin laeuft.
    if (zeile.compare(0, 4, "ra: ") == 0) {
        std::string neu_bau;
        {
            std::lock_guard<std::mutex> g(sperre_);
            firmware_.ra_patch = true;
            // Zwei Quellen: "ra: mcuapp <Datum> <Zeit>" beim Einschalten und
            // "ra: hook N pairs, mcu <Datum>" beim Spielstart. Die erste ist
            // genauer, weil sie die Uhrzeit mitbringt.
            std::string mcu = hinter("ra: mcuapp ");
            if (mcu.empty()) mcu = hinter(", mcu ");
            if (!mcu.empty() && mcu != firmware_.ra_bau) {
                firmware_.ra_bau = mcu;
                neu_bau = mcu;
            }
            const size_t p = zeile.find("hook ");
            if (p != std::string::npos && zeile.find(" pairs") != std::string::npos)
                firmware_.hook_paare = std::atoi(zeile.c_str() + p + 5);
        }
        // In die Aufzeichnung, damit beim spaeteren Lesen ohne Suchen
        // dasteht, welcher Stand lief.
        if (!neu_bau.empty())
            g_aufzeichnung.Zeile("FIRMWARE", "RA-Patch, gebaut " + neu_bau);
        return;
    }

    // "Found Saturn bootrom file." und in der Zeile danach "Size 00076396".
    if (zeile.find("Saturn bootrom file") != std::string::npos) {
        bootrom_erwartet_ = true;
        return;
    }
    if (bootrom_erwartet_) {
        const std::string groesse = hinter("Size");
        if (!groesse.empty()) {
            std::lock_guard<std::mutex> g(sperre_);
            firmware_.ssfirm_bytes = std::strtol(groesse.c_str(), nullptr, 16);
        }
        bootrom_erwartet_ = false;
        return;
    }

    // "Load config [/SAROO/saroocfg.txt] for [T-11304H  V3.08 ]"
    if (zeile.find("Load config") != std::string::npos) {
        const size_t a = zeile.find("for [");
        const size_t b = (a == std::string::npos) ? std::string::npos
                                                  : zeile.find(']', a + 5);
        if (b != std::string::npos) {
            std::lock_guard<std::mutex> g(sperre_);
            firmware_.spiel_abschnitt = zeile.substr(a + 5, b - a - 5);
        }
    }
}

// ---------------------------------------------------------------------------
// Dateien auf die SD-Karte der SAROO
//
// Der Auftrag kommt aus dem Zeichen-Thread, ausgefuehrt wird er im Lese-Thread:
// Anmelden, dann Block fuer Block senden und jeweils die Quittung des Geraets
// abwarten. Ohne dieses Warten ueberholt die Leitung bei einem Megabit die
// Karte. Die Zustandsfelder ausser Auftrag und Fortschritt gehoeren allein dem
// Lese-Thread und brauchen deshalb keine Sperre.
// ---------------------------------------------------------------------------

// Wie lange auf eine Quittung gewartet wird. Grosszuegig, weil das Geraet
// zwischendurch die Disc-Emulation bedient und ein Schreibvorgang auf eine
// langsame Karte mehrere hundert Millisekunden dauern kann.
static constexpr unsigned long kDateiWarteMs = 15000;

bool Connection::DateiSenden(const std::string& quelle, const std::string& zielpfad,
                             std::string* fehler) {
    if (!laeuft_.load()) {
        if (fehler) *fehler = T("Keine Verbindung zum Gerät.");
        return false;
    }
    if (datei_sendet_.load()) {
        if (fehler) *fehler = T("Es läuft schon eine Übertragung.");
        return false;
    }

    FILE* f = std::fopen(quelle.c_str(), "rb");
    if (!f) {
        if (fehler) *fehler = std::string(T("Datei lässt sich nicht öffnen: ")) + quelle;
        return false;
    }
    std::vector<uint8_t> inhalt;
    uint8_t puffer[4096];
    for (;;) {
        const size_t n = std::fread(puffer, 1, sizeof(puffer), f);
        if (n == 0) break;
        inhalt.insert(inhalt.end(), puffer, puffer + n);
    }
    std::fclose(f);

    if (inhalt.empty()) {
        if (fehler) *fehler = T("Die Datei ist leer.");
        return false;
    }

    std::lock_guard<std::mutex> g(sperre_);
    datei_inhalt_.swap(inhalt);
    datei_ziel_ = zielpfad;
    datei_quelle_ = quelle;
    datei_auftrag_ = true;
    datei_abbruch_ = false;
    datei_prozent_ = 0;
    datei_sendet_ = true;
    return true;
}

void Connection::DateiAbbrechen() { datei_abbruch_ = true; }

void Connection::DateiEnde(bool gut, const std::string& text) {
    datei_stand_ = DateiStand::Aus;
    datei_daten_.clear();
    datei_ab_ = 0;
    datei_abbruch_ = false;
    std::lock_guard<std::mutex> g(sperre_);
    datei_auftrag_ = false;
    datei_inhalt_.clear();
    datei_sendet_ = false;
    ausgabe_.push_back({text, gut ? app::Rang::Gut : app::Rang::Warnung});
}

void Connection::DateiAck(uint8_t auf_typ, uint8_t status) {
    if (datei_stand_ == DateiStand::Aus) return;

    if (status != 0) {
        std::string t = T("SAROO hat die Übertragung abgelehnt: ");
        t += core::AckGrundText(status);
        t += ". ";
        t += T("Die Datei auf der Karte ist unverändert.");
        DateiEnde(false, t);
        return;
    }

    if (auf_typ == core::kTypeFileBegin && datei_stand_ == DateiStand::WarteBegin) {
        datei_stand_ = DateiStand::Block;
    } else if (auf_typ == core::kTypeFileData && datei_stand_ == DateiStand::WarteBlock) {
        datei_stand_ = DateiStand::Block;
    } else if (auf_typ == core::kTypeFileEnd && datei_stand_ == DateiStand::WarteEnde) {
        const unsigned long s = (::GetTickCount() - datei_beginn_) / 1000;
        char t[240];
        std::snprintf(t, sizeof(t),
                      T("%s liegt auf der Karte: %d Bytes in %lu Sekunden. "
                        "Wirkt beim nächsten Einschalten."),
                      datei_pfad_.c_str(), (int)datei_daten_.size(), s);
        DateiEnde(true, t);
    }
}

void Connection::DateiSchritt(core::SerialPort& port) {
    if (datei_stand_ == DateiStand::Aus) {
        std::lock_guard<std::mutex> g(sperre_);
        if (!datei_auftrag_ || datei_inhalt_.empty()) return;
        datei_daten_.swap(datei_inhalt_);
        datei_inhalt_.clear();
        datei_pfad_ = datei_ziel_;
        datei_von_ = datei_quelle_;
        datei_ab_ = 0;
        datei_seq_ = 0;
        datei_summe_ = 0;
        for (size_t i = 0; i < datei_daten_.size(); ++i)
            datei_summe_ += datei_daten_[i];
        datei_beginn_ = ::GetTickCount();
        datei_stand_ = DateiStand::Anmelden;
    }

    if (datei_abbruch_.load()) {
        DateiEnde(false, T("Übertragung abgebrochen. Die Datei auf der Karte ist "
                           "unverändert."));
        return;
    }

    const unsigned long jetzt = ::GetTickCount();

    if (datei_stand_ == DateiStand::WarteBegin ||
        datei_stand_ == DateiStand::WarteBlock ||
        datei_stand_ == DateiStand::WarteEnde) {
        if (jetzt - datei_gewartet_ > kDateiWarteMs)
            DateiEnde(false, T("Übertragung abgebrochen: Das Gerät hat nicht "
                               "geantwortet. Die Datei auf der Karte ist unverändert."));
        return;
    }

    if (datei_stand_ == DateiStand::Anmelden) {
        const std::vector<uint8_t> r =
            core::BaueFileBegin(datei_pfad_, (uint32_t)datei_daten_.size(), datei_seq_++);
        if (!port.Schreiben(r.data(), (int)r.size())) {
            DateiEnde(false, T("Übertragung abgebrochen: Der Anschluss nimmt nichts "
                               "mehr an."));
            return;
        }
        // Quelle UND Ziel nennen. Das Ziel ist fest, die Quelle frei gewaehlt;
        // wer nur das Ziel sieht, merkt nicht, welche Datei unter dem
        // regulaeren Namen landet.
        char t[320];
        std::snprintf(t, sizeof(t), T("Übertrage %s nach %s auf der Karte: %d Bytes."),
                      datei_von_.c_str(), datei_pfad_.c_str(),
                      (int)datei_daten_.size());
        Melden(t);
        datei_gewartet_ = jetzt;
        datei_stand_ = DateiStand::WarteBegin;
        return;
    }

    if (datei_stand_ != DateiStand::Block) return;

    // Ein Block, danach warten. Das Geraet quittiert erst, wenn sein Puffer
    // voll ist -- ein kuerzerer Rest am Ende wird vom Abschluss miterledigt.
    const size_t rest = datei_daten_.size() - datei_ab_;
    const size_t block = rest < (size_t)core::kFileBlockBytes
                             ? rest : (size_t)core::kFileBlockBytes;
    size_t ab = 0;
    while (ab < block) {
        int n = (int)(block - ab);
        if (n > core::kFileRahmenNutz) n = core::kFileRahmenNutz;
        const std::vector<uint8_t> r =
            core::BaueFileData(&datei_daten_[datei_ab_ + ab], n, datei_seq_++);
        if (!port.Schreiben(r.data(), (int)r.size())) {
            DateiEnde(false, T("Übertragung abgebrochen: Der Anschluss nimmt nichts "
                               "mehr an."));
            return;
        }
        ab += (size_t)n;
    }
    datei_ab_ += ab;
    datei_prozent_ = (int)((datei_ab_ * 100) / datei_daten_.size());
    datei_gewartet_ = jetzt;

    if (datei_ab_ < datei_daten_.size()) {
        datei_stand_ = DateiStand::WarteBlock;
        return;
    }

    const std::vector<uint8_t> e =
        core::BaueFileEnd((uint32_t)datei_daten_.size(), datei_summe_, datei_seq_++);
    if (!port.Schreiben(e.data(), (int)e.size())) {
        DateiEnde(false, T("Übertragung abgebrochen: Der Anschluss nimmt nichts mehr an."));
        return;
    }
    datei_stand_ = DateiStand::WarteEnde;
}

