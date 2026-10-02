#include "setup_window.h"

#include <cstdio>
#include <cstring>

#include <windows.h>    // GetTickCount fuer die Restzeit im Fortschrittsbalken

#include "core/dialoge.h"
#include "core/i18n.h"
#include "imgui.h"
#include "theme.h"

namespace {
// Uebersetztes Format mit Werten fuellen.
std::string Fmt(const char* de_format, int a, int b = 0) {
    char t[256];
    std::snprintf(t, sizeof(t), T(de_format), a, b);
    return t;
}
}  // namespace

SetupWindow::~SetupWindow() { Beenden(); }

void SetupWindow::Beenden() {
    if (thread_.joinable()) thread_.join();
}

void SetupWindow::Oeffnen(const std::string& vorschlag) {
    offen_ = true;
    if (rom_ordner_[0] == '\0' && !vorschlag.empty())
        std::snprintf(rom_ordner_, sizeof(rom_ordner_), "%s", vorschlag.c_str());
    if (karte_[0] == '\0' && !vorschlag.empty()) {
        const std::string w = core::SdWurzelFinden(vorschlag);
        std::snprintf(karte_, sizeof(karte_), "%s", w.c_str());
    }
    if (!arbeitet_.load()) {
        std::lock_guard<std::mutex> g(sperre_);
        fw_ = core::FirmwarePruefen(karte_);
    }
}

void SetupWindow::FirmwareStarten(bool zurueck) {
    if (arbeitet_.load()) return;
    const std::string wurzel = karte_;
    arbeitet_ = true;
    gesamt_ = 0;
    {
        std::lock_guard<std::mutex> g(sperre_);
        fw_fertig_ = false;
        status_ = zurueck ? T("Stelle die Original-Firmware wieder her…")
                          : T("Patche die Firmware…");
    }
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread([this, wurzel, zurueck]() {
        std::vector<std::string> log;
        std::string fehler;
        const bool ok = zurueck ? core::FirmwareZurueck(wurzel, &log, &fehler)
                                : core::FirmwarePatchen(wurzel, &log, &fehler);
        std::lock_guard<std::mutex> g(sperre_);
        for (const std::string& z : log) ausgabe_.push_back({z, app::Rang::Normal});
        if (ok) {
            status_ = zurueck ? T("Original-Firmware liegt auf der Karte.")
                              : T("Firmware gepatcht.");
            ausgabe_.push_back({status_, app::Rang::Gut});
            fw_fertig_ = true;
        } else {
            status_ = T("Fehler: ") + fehler;
            ausgabe_.push_back({status_, app::Rang::Warnung});
        }
        fw_ = core::FirmwarePruefen(wurzel);
        arbeitet_ = false;
    });
}

void SetupWindow::FirmwareZeichnen(float dpi) {
    const bool beschaeftigt = arbeitet_.load();
    ImGui::SeparatorText(T("Firmware der SAROO"));

    ImGui::BeginDisabled(beschaeftigt);
    ImGui::SetNextItemWidth(-220.0f * dpi);
    if (ImGui::InputTextWithHint("##karte", T("SD-Karte, z. B. H:\\"), karte_, sizeof(karte_),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        const std::string w = core::SdWurzelFinden(karte_);
        if (!w.empty()) std::snprintf(karte_, sizeof(karte_), "%s", w.c_str());
        std::lock_guard<std::mutex> g(sperre_);
        fw_ = core::FirmwarePruefen(karte_);
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Karte wählen…"))) {
        const std::string p = core::OrdnerWaehlen(T("SD-Karte der SAROO wählen"), karte_);
        if (!p.empty()) {
            const std::string w = core::SdWurzelFinden(p);
            std::snprintf(karte_, sizeof(karte_), "%s", (w.empty() ? p : w).c_str());
            std::lock_guard<std::mutex> g(sperre_);
            fw_ = core::FirmwarePruefen(karte_);
            fw_fertig_ = false;
        }
    }
    ImGui::EndDisabled();

    core::FwStand st;
    bool fertig = false;
    {
        std::lock_guard<std::mutex> g(sperre_);
        st = fw_;
        fertig = fw_fertig_;
    }

    if (karte_[0] != '\0' && !st.karte) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kBernstein));
        ImGui::TextUnformatted(T("Dort gibt es keinen Ordner SAROO. Bitte das Laufwerk der SD-Karte wählen."));
        ImGui::PopStyleColor();
    } else if (st.karte) {
        for (const core::FwTeilStand& t : st.teile) {
            const char* text = T("fehlt");
            ImU32 farbe = theme::kSehrLeise;
            switch (t.lage) {
                case core::FwLage::Original: text = T("offizielle v0.9"); farbe = theme::kLeise; break;
                case core::FwLage::RawTurn:  text = T("gepatcht");        farbe = theme::kSalbei; break;
                case core::FwLage::Anders:   text = T("andere Fassung");  farbe = theme::kBernstein; break;
                case core::FwLage::Fehlt:    break;
            }
            ImGui::Text("%-14s", t.name);
            ImGui::SameLine(160.0f * dpi);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(farbe));
            ImGui::TextUnformatted(text);
            ImGui::PopStyleColor();
            if (t.gesichert) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", T("· vorheriger Stand gesichert"));
            }
        }

        ImGui::BeginDisabled(beschaeftigt);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kMessing));
        if (ImGui::Button(T("Firmware patchen"))) FirmwareStarten(false);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button(T("Original wiederherstellen"))) FirmwareStarten(true);
        ImGui::EndDisabled();

        if (fertig) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kMessing));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(T(
                "Jetzt die Karte in die SAROO stecken und die Saturn einschalten. Ein "
                "Firmware Update im SAROO-Menü ist nicht nötig."));
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
    }
    ImGui::Spacing();
    ImGui::SeparatorText(T("Spiele"));
}

void SetupWindow::PruefenStarten(core::RaClient& client) {
    if (arbeitet_.load()) return;
    const std::string ordner = rom_ordner_;
    const std::string cfg = cfg_pfad_;
    if (ordner.empty()) {
        std::lock_guard<std::mutex> g(sperre_);
        status_ = T("Bitte einen ROM-Ordner wählen.");
        return;
    }

    {
        std::lock_guard<std::mutex> g(sperre_);
        ergebnisse_.clear();
        ausgewaehlt_ = -1;
        geschrieben_ = false;
        status_ = T("Suche Disc-Abbilder…");
    }

    arbeitet_ = true;
    fortschritt_ = 0;
    gesamt_ = 0;
    begonnen_ = ::GetTickCount();
    const bool rek = rekursiv_;
    const bool nur_set = nur_mit_set_;

    if (thread_.joinable()) thread_.join();
    thread_ = std::thread([this, ordner, rek, nur_set, &client]() {
        const std::vector<std::string> abbilder = core::FindeAbbilder(ordner, rek);
        gesamt_ = (int)abbilder.size();
        if (abbilder.empty()) {
            std::lock_guard<std::mutex> g(sperre_);
            status_ = T("Keine Disc-Abbilder in diesem Ordner gefunden.");
            arbeitet_ = false;
            return;
        }
        for (size_t i = 0; i < abbilder.size(); ++i) {
            core::SetupErgebnis e = core::VerarbeiteAbbild(abbilder[i], client, nur_set);
            fortschritt_ = (int)i + 1;
            std::lock_guard<std::mutex> g(sperre_);
            const size_t schr = abbilder[i].find_last_of("\\/");
            status_ = "[" + std::to_string(i + 1) + "/" + std::to_string(abbilder.size()) +
                      "] " + (schr == std::string::npos ? abbilder[i]
                                                        : abbilder[i].substr(schr + 1));
            ergebnisse_.push_back(std::move(e));
        }
        int bereit = 0;
        {
            std::lock_guard<std::mutex> g(sperre_);
            for (const core::SetupErgebnis& e : ergebnisse_) if (e.installierbar) ++bereit;
            status_ = Fmt("Fertig. %d von %d Spielen bekommen einen Hook.",
                          bereit, (int)ergebnisse_.size());
            ausgabe_.push_back({Fmt("SAROO einrichten: %d Spiel(e) bereit.", bereit),
                                app::Rang::Normal});
        }
        arbeitet_ = false;
    });
}

void SetupWindow::SchreibenStarten(core::GameIndex& index) {
    if (arbeitet_.load()) return;
    const std::string cfg = cfg_pfad_;
    if (cfg.empty()) {
        std::lock_guard<std::mutex> g(sperre_);
        status_ = T("Bitte die saroocfg.txt wählen.");
        return;
    }

    std::vector<core::SetupErgebnis> fertige;
    {
        std::lock_guard<std::mutex> g(sperre_);
        for (const core::SetupErgebnis& e : ergebnisse_) if (e.installierbar) fertige.push_back(e);
    }
    if (fertige.empty()) return;

    arbeitet_ = true;
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread([this, cfg, fertige, &index]() {
        std::string sicherung, fehler;
        const bool ok = core::SchreibeSaroocfg(cfg, fertige, &sicherung, &fehler);

        // Nachschlagetabelle nebenbei fuellen: JEDE lesbare Kennung kommt
        // hinein, auch ohne RA-Spielkennung. Die Identitaet steht im Disc-Kopf
        // und hat mit RetroAchievements nichts zu tun.
        int neu = 0;
        {
            std::lock_guard<std::mutex> g(sperre_);
            for (const core::SetupErgebnis& e : ergebnisse_) {
                if (e.kennung.empty()) continue;
                core::IndexEintrag ie;
                ie.gameid = e.gameid;
                ie.name = e.name;
                ie.titel = e.kopf.titel;
                ie.md5 = e.md5;
                ie.abbild = e.abbild;
                ie.gebiete = e.kopf.gebiete;
                ie.achievements = e.achievements;
                if (index.Setzen(e.kennung, ie)) ++neu;
            }
        }
        index.Speichern(core::StandardIndexPfad());

        // ROM-Ordner merken. Damit kann das Hauptfenster ein spaeter
        // gemeldetes, noch unbekanntes Spiel selbst aufloesen, ohne dass
        // jemand etwas auswaehlen muss.
        core::Einstellungen ein;
        ein.Laden();
        ein.rom_ordner = rom_ordner_;
        ein.saroocfg = cfg_pfad_;
        ein.Speichern();

        std::lock_guard<std::mutex> g(sperre_);
        if (ok) {
            status_ = Fmt("Geschrieben. %d Spiel(e) eingetragen.", (int)fertige.size());
            geschrieben_ = true;
            ausgabe_.push_back({T("saroocfg.txt geschrieben, Sicherung: ") + sicherung,
                                app::Rang::Gut});
            ausgabe_.push_back({Fmt("%d neue Kennung(en) in der Nachschlagetabelle.", neu),
                                app::Rang::Normal});
        } else {
            status_ = T("Fehler: ") + fehler;
            ausgabe_.push_back({T("Schreiben fehlgeschlagen: ") + fehler, app::Rang::Warnung});
        }
        arbeitet_ = false;
    });
}

void SetupWindow::Zeichnen(app::State& state, core::RaClient& client,
                           core::GameIndex& index, float dpi) {
    {
        std::lock_guard<std::mutex> g(sperre_);
        for (app::Logzeile& z : ausgabe_) state.log.push_back(std::move(z));
        ausgabe_.clear();
    }
    if (!offen_) return;

    // Innerhalb des Hauptfensters bleiben: mittig oeffnen, hoechstens so gross
    // wie das Hauptfenster abzueglich Rand, und mit sichtbarem Rahmen -- die
    // Hauptflaeche selbst hat keinen, dort waere er nur Unruhe.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float rand = 24.0f * dpi;
    const ImVec2 hoechst(vp->WorkSize.x - 2.0f * rand, vp->WorkSize.y - 2.0f * rand);
    ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f * dpi, 240.0f * dpi), hoechst);
    ImGui::SetNextWindowSize(ImVec2(880.0f * dpi < hoechst.x ? 880.0f * dpi : hoechst.x,
                                    620.0f * dpi < hoechst.y ? 620.0f * dpi : hoechst.y),
                             ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(vp->GetWorkCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    // Feste Kennung hinter "###", damit das Fenster bei einem Sprachwechsel
    // offen bleibt und seine Groesse behaelt.
    const std::string titel = std::string(T("SAROO einrichten")) + "###einrichten";
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * dpi, 10.0f * dpi));
    const bool sichtbar = ImGui::Begin(titel.c_str(), &offen_, ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    if (!sichtbar) { ImGui::End(); return; }

    FirmwareZeichnen(dpi);

    const bool beschaeftigt = arbeitet_.load();

    ImGui::BeginDisabled(beschaeftigt);
    ImGui::SetNextItemWidth(-220.0f * dpi);
    ImGui::InputTextWithHint("##romordner", T("ROM-Ordner der SD-Karte"),
                             rom_ordner_, sizeof(rom_ordner_));
    ImGui::SameLine();
    if (ImGui::Button(T("Ordner wählen…"))) {
        const std::string p = core::OrdnerWaehlen(T("ROM-Ordner wählen"), rom_ordner_);
        if (!p.empty()) {
            std::snprintf(rom_ordner_, sizeof(rom_ordner_), "%s", p.c_str());
            // saroocfg.txt liegt bei der SAROO im Wurzelverzeichnis der Karte,
            // die Spiele aber meist in einem Unterordner wie \SAROO\ISO.
            // Deshalb aufwaerts suchen, nicht nur im gewaehlten Ordner.
            if (cfg_pfad_[0] == '\0') {
                std::string d = p;
                for (int stufe = 0; stufe < 4 && !d.empty(); ++stufe) {
                    const std::string v = d + "\\saroocfg.txt";
                    FILE* f = std::fopen(v.c_str(), "rb");
                    if (f) {
                        std::fclose(f);
                        std::snprintf(cfg_pfad_, sizeof(cfg_pfad_), "%s", v.c_str());
                        break;
                    }
                    const size_t schr = d.find_last_of("\\/");
                    if (schr == std::string::npos || schr < 2) break;
                    d.resize(schr);
                }
            }
        }
    }

    ImGui::SetNextItemWidth(-220.0f * dpi);
    ImGui::InputTextWithHint("##cfg", T("Pfad zur saroocfg.txt"), cfg_pfad_, sizeof(cfg_pfad_));
    ImGui::SameLine();
    if (ImGui::Button(T("Datei wählen…"))) {
        // Der Dateifilter enthaelt eingebettete Nullzeichen und bleibt deshalb
        // zweisprachig fest, statt ueber T() zu laufen.
        const std::string p = core::DateiWaehlen(
            T("saroocfg.txt wählen"), "saroocfg.txt\0saroocfg.txt\0*.txt\0*.txt\0"
            "*.*\0*.*\0\0", rom_ordner_);
        if (!p.empty()) std::snprintf(cfg_pfad_, sizeof(cfg_pfad_), "%s", p.c_str());
    }

    const std::string unterordner = std::string(T("Unterordner mit durchsuchen")) + "###rek";
    ImGui::Checkbox(unterordner.c_str(), &rekursiv_);
    ImGui::SameLine(0.0f, 24.0f * dpi);
    const std::string nur_set = std::string(T("Nur Spiele mit Achievement-Set einrichten")) +
                                "###nurset";
    ImGui::Checkbox(nur_set.c_str(), &nur_mit_set_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Aus: jedes erkannte Saturn-Abbild wird eingerichtet.\n"
                                  "Der Hook hängt nicht am Set, und nur so meldet jedes\n"
                                  "laufende Spiel im Betrieb selbst seine Kennung."));
    ImGui::EndDisabled();

    ImGui::Separator();

    ImGui::BeginDisabled(beschaeftigt);
    if (ImGui::Button(T("Prüfen"))) PruefenStarten(client);
    ImGui::EndDisabled();
    ImGui::SameLine();

    int bereit = 0;
    {
        std::lock_guard<std::mutex> g(sperre_);
        for (const core::SetupErgebnis& e : ergebnisse_) if (e.installierbar) ++bereit;
    }
    const bool cfg_fehlt = (cfg_pfad_[0] == '\0');
    ImGui::BeginDisabled(beschaeftigt || bereit == 0);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(
        cfg_fehlt ? theme::kBernstein : theme::kMessing));
    if (ImGui::Button(T("Schreiben"))) SchreibenStarten(index);
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (cfg_fehlt) {
        // Fehlt die Zieldatei, sagt es der Knopf selbst; in der Statuszeile
        // wuerde der Hinweis leicht uebersehen.
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertU32ToFloat4(theme::kBernstein));
        ImGui::TextUnformatted(T("Erst die saroocfg.txt oben auswählen."));
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("%s", T("Bestehende Einträge bleiben erhalten. Vorher wird gesichert."));
    }

    // Fortschritt: Anteil, Zaehler und geschaetzte Restzeit aus dem bisherigen
    // Mittel je Abbild.
    if (beschaeftigt && gesamt_.load() > 0) {
        const int fertig = fortschritt_.load();
        const int gesamt = gesamt_.load();
        const float anteil = (float)fertig / (float)gesamt;
        char t[96];
        const unsigned long start = begonnen_.load();
        const unsigned long jetzt = ::GetTickCount();
        if (fertig > 0 && start != 0 && jetzt > start) {
            const double je = (double)(jetzt - start) / (double)fertig;
            const int rest = (int)((je * (double)(gesamt - fertig)) / 1000.0 + 0.5);
            if (rest >= 60)
                std::snprintf(t, sizeof(t), T("%d/%d · %d %% · noch etwa %d:%02d min"),
                              fertig, gesamt, (int)(anteil * 100.0f + 0.5f),
                              rest / 60, rest % 60);
            else
                std::snprintf(t, sizeof(t), T("%d/%d · %d %% · noch etwa %d s"),
                              fertig, gesamt, (int)(anteil * 100.0f + 0.5f), rest);
        } else {
            std::snprintf(t, sizeof(t), "%d/%d", fertig, gesamt);
        }
        ImGui::ProgressBar(anteil, ImVec2(-1.0f, 18.0f * dpi), t);
    }

    {
        std::lock_guard<std::mutex> g(sperre_);
        ImGui::TextUnformatted(status_.empty() ? T("Ordner wählen und auf Prüfen klicken.")
                                               : status_.c_str());

        const float hoehe = ImGui::GetContentRegionAvail().y - 70.0f * dpi;
        if (ImGui::BeginTable("setup", 5,
                ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp,
                ImVec2(0.0f, hoehe > 80.0f * dpi ? hoehe : 80.0f * dpi))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(T("Kennung"), ImGuiTableColumnFlags_WidthFixed, 150.0f * dpi);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 180.0f * dpi);
            ImGui::TableSetupColumn("Ach.", ImGuiTableColumnFlags_WidthFixed, 46.0f * dpi);
            ImGui::TableSetupColumn("Watch", ImGuiTableColumnFlags_WidthFixed, 54.0f * dpi);
            ImGui::TableSetupColumn(T("Spiel"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (int i = 0; i < (int)ergebnisse_.size(); ++i) {
                const core::SetupErgebnis& e = ergebnisse_[(size_t)i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                // Eigene Kennung je Zeile: Dieselbe Spielkennung kommt in der
                // Liste mehrfach vor (zwei Abbilder desselben Spiels, etwa
                // Mr. Bones mit CD 1 und CD 2). Ohne PushID haben beide
                // Zeilen dieselbe ImGui-Kennung, und die Bibliothek meldet
                // "2 visible items with conflicting ID".
                ImGui::PushID(i);
                if (theme::mono) ImGui::PushFont(theme::mono, 0.0f);
                if (ImGui::Selectable((e.kennung.empty() ? "-" : e.kennung).c_str(),
                                      ausgewaehlt_ == i, ImGuiSelectableFlags_SpanAllColumns))
                    ausgewaehlt_ = i;
                if (theme::mono) ImGui::PopFont();
                ImGui::PopID();

                ImGui::TableSetColumnIndex(1);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(
                    e.installierbar ? theme::kSalbei : theme::kSehrLeise));
                // Status ist ein fester deutscher Schluessel aus core/setup.cpp.
                ImGui::TextUnformatted(T(e.status.c_str()));
                ImGui::PopStyleColor();

                ImGui::TableSetColumnIndex(2);
                // Inoffizielle mit ausweisen. Sonst steht hier 0, waehrend die
                // RA-Seite sehr wohl Achievements zeigt, und das sieht nach
                // einem Fehler des Werkzeugs aus.
                if (e.inoffiziell > 0) ImGui::Text(T("%d (+%d inoff.)"), e.achievements,
                                                   e.inoffiziell);
                else                   ImGui::Text("%d", e.achievements);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%d", e.watch);
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(e.name.c_str());
            }
            ImGui::EndTable();
        }

        // Begruendung zur ausgewaehlten Zeile. Ohne das sieht "nicht erkannt"
        // aus wie "kaputt", statt zu sagen, woran es liegt.
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kLeise));
        if (ausgewaehlt_ >= 0 && ausgewaehlt_ < (int)ergebnisse_.size()) {
            const core::SetupErgebnis& e = ergebnisse_[(size_t)ausgewaehlt_];
            ImGui::TextWrapped("%s", e.hinweis.empty()
                ? T("Nichts weiter anzumerken.") : e.hinweis.c_str());
        } else {
            ImGui::TextWrapped("%s", T("Zeile anklicken, um die Begründung zu sehen."));
        }
        ImGui::PopStyleColor();
    }

    ImGui::End();
}
