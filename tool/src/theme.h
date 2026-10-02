// Schrift und Farben.
//
// Die Voreinstellung von Dear ImGui ist ein 13-Pixel-Bitmapzeichensatz ohne
// Umlaute. Hier werden stattdessen Systemschriften geladen -- die liegen auf
// jedem Windows und muessen deshalb nicht mitgeliefert werden. Das einzelne
// Programm bleibt damit eine einzelne Datei.
//
// Farben: fuenf umschaltbare Schemata, waehlbar in den Optionen. Die
// Protokollfarben sind in ALLEN Schemata dieselben und stehen deshalb getrennt
// als feste Werte.
#pragma once

#include "imgui.h"

namespace theme {

// Schriften. nullptr heisst: Standard benutzen.
extern ImFont* ui;        // Fliesstext
extern ImFont* bold;      // Fenstername, hervorgehobene Werte
extern ImFont* klein;     // gesperrte Bereichsbeschriftungen
extern ImFont* mono;      // Adressen, Zahlen, Protokoll
extern ImFont* kursiv;    // Hinweis im Ko-fi- und Optionen-Dialog, wie in mega-raw

// --- Farbschemata --------------------------------------------------------
enum class Stil { Heute, Graphit, SaturnGrau, Bernstein, Hell };
constexpr int kStilAnzahl = 5;

const char* StilName(Stil s);            // fuer die Anzeige
const char* StilKennung(Stil s);         // fuer die Einstellungsdatei
Stil StilAusKennung(const char* kennung); // Unbekanntes ergibt Stil::Heute
Stil AktuellerStil();

// Setzt die Farben unten UND die Farben von Dear ImGui. Wirkt sofort.
void StilSetzen(Stil s);

// Farben des aktuellen Schemas. Namen wie bisher, damit die Aufrufer bleiben
// koennen; seit den Schemata sind es Variablen statt Konstanten.
extern ImU32 kGrund;       // Fenstergrund
extern ImU32 kFeld;        // Eingabefelder, Knoepfe
extern ImU32 kLinie;       // Raender von Feldern und Kaesten
extern ImU32 kFein;        // Haarlinien zwischen Bereichen
extern ImU32 kText;
extern ImU32 kLeise;       // Beschriftungen
extern ImU32 kSehrLeise;   // Hinweistexte
extern ImU32 kMessing;     // Akzent: Freigeschaltetes, geaenderte Werte
extern ImU32 kSalbei;      // Zustand gut
extern ImU32 kBernstein;   // Zustand Warnung
extern ImU32 kRost;        // Zustand Fehler
extern ImU32 kKarte;       // Flaeche hinter einer Gruppe; 0 = nur Haarlinien
extern ImU32 kKartenrand;
extern ImU32 kMarke;       // Gruppenbeschriftung
extern ImU32 kTabGrund;    // Tabellen und Live-Werte
extern ImU32 kLogGrund;    // Protokoll
extern bool  gMarkeMonoGross;  // Gruppenbeschriftung in Mono und Grossbuchstaben

// Protokollfarben, in jedem Schema gleich.
constexpr ImU32 kLogNormal  = IM_COL32(0x8b, 0x8b, 0x85, 0xff);
constexpr ImU32 kLogGut     = IM_COL32(0x83, 0xa7, 0x8c, 0xff);
constexpr ImU32 kLogWarnung = IM_COL32(0xc3, 0x93, 0x52, 0xff);
constexpr ImU32 kLogWichtig = IM_COL32(0xc8, 0xa7, 0x65, 0xff);

// dpi_scale: 1.0 entspricht 96 dpi. Laedt Schriften, setzt Abstaende und das
// aktuelle Farbschema.
void Apply(float dpi_scale);

// --- Bausteine, damit die Bereiche ueberall gleich aussehen -------------

// Beschriftung ueber einer Gruppe.
void Marke(const char* text);

// Duenne Trennlinie zwischen zwei Bereichen.
void Haarlinie();

// Beschriftung und Wert nebeneinander, in einer Zeile.
void Feld(const char* beschriftung, const char* wert, ImFont* wertschrift = nullptr);

// Zustandspunkt mit Text, z. B. gruener Punkt plus "Hook laeuft".
void Zustand(ImU32 farbe, const char* text);

}  // namespace theme
